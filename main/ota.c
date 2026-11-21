#include "ota.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "o3e_db.h"

static const char *TAG = "ota";

/* How long a freshly flashed image has to prove it works before the health
 * check confirms it. Long enough to cover Wi-Fi association and a first MQTT
 * connect, short enough that nobody is left waiting. */
#define OTA_HEALTH_DELAY_MS 60000

static esp_ota_handle_t handle;
static const esp_partition_t *target;
static size_t written;
static bool in_progress;

void ota_info(ota_info_t *out)
{
    memset(out, 0, sizeof(*out));

    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) {
        snprintf(out->running, sizeof(out->running), "%s", run->label);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        snprintf(out->version, sizeof(out->version), "%s", app->version);
        snprintf(out->idf_version, sizeof(out->idf_version), "%s", app->idf_ver);
    }

    esp_ota_img_states_t state;
    if (run && esp_ota_get_state_partition(run, &state) == ESP_OK) {
        out->pending_verify = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    out->update_size = (uint32_t)written;
}

void ota_mark_healthy(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!run || esp_ota_get_state_partition(run, &state) != ESP_OK) {
        return;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;   /* already confirmed, or rollback is not enabled */
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "running image confirmed, rollback cancelled");
    }
}

static void health_timer_cb(void *arg)
{
    (void)arg;
    ota_mark_healthy();
}

void ota_arm_health_check(void)
{
    ota_info_t info;
    ota_info(&info);
    if (!info.pending_verify) {
        return;
    }
    ESP_LOGW(TAG, "running a new image on trial; confirming in %d s if it stays up",
             OTA_HEALTH_DELAY_MS / 1000);

    const esp_timer_create_args_t ta = { .callback = health_timer_cb, .name = "ota_health" };
    esp_timer_handle_t t;
    if (esp_timer_create(&ta, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)OTA_HEALTH_DELAY_MS * 1000);
    }
}

#define OTA_FAIL(...) do { \
        if (err && err_sz) { snprintf(err, err_sz, __VA_ARGS__); } \
        ESP_LOGE(TAG, __VA_ARGS__); \
        ota_abort(); \
        return false; \
    } while (0)

bool ota_begin(size_t total_size, char *err, size_t err_sz)
{
    if (in_progress) {
        if (err && err_sz) {
            snprintf(err, err_sz, "an update is already running");
        }
        return false;
    }

    target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        if (err && err_sz) {
            snprintf(err, err_sz, "no second app partition available");
        }
        return false;
    }
    if (total_size && total_size > target->size) {
        if (err && err_sz) {
            snprintf(err, err_sz, "image is %u KiB, the partition holds %u KiB",
                     (unsigned)(total_size / 1024), (unsigned)(target->size / 1024));
        }
        return false;
    }

    written = 0;
    in_progress = true;
    /* OTA_SIZE_UNKNOWN erases the whole partition up front, which takes a few
     * seconds; passing the real size only erases what is needed. */
    esp_err_t e = esp_ota_begin(target, total_size ? total_size : OTA_SIZE_UNKNOWN, &handle);
    if (e != ESP_OK) {
        in_progress = false;
        if (err && err_sz) {
            snprintf(err, err_sz, "could not open the update partition: %s",
                     esp_err_to_name(e));
        }
        return false;
    }
    ESP_LOGI(TAG, "update started, writing to '%s' (%u KiB free)",
             target->label, (unsigned)(target->size / 1024));
    return true;
}

bool ota_write(const void *data, size_t len, char *err, size_t err_sz)
{
    if (!in_progress) {
        if (err && err_sz) {
            snprintf(err, err_sz, "no update in progress");
        }
        return false;
    }

    /* Reject anything that is not an ESP32 application image before writing a
     * single byte, so picking the wrong file cannot brick the spare partition
     * half way through. */
    if (written == 0) {
        const uint8_t *p = data;
        if (len < 1 || p[0] != ESP_IMAGE_HEADER_MAGIC) {
            OTA_FAIL("this file is not an ESP32 firmware image");
        }
        if (len >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                   sizeof(esp_app_desc_t)) {
            esp_app_desc_t desc;
            memcpy(&desc, p + sizeof(esp_image_header_t) +
                          sizeof(esp_image_segment_header_t), sizeof(desc));
            if (desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
                ESP_LOGI(TAG, "incoming image: %s (%s, IDF %s)",
                         desc.project_name, desc.version, desc.idf_ver);
            }
        }
    }

    esp_err_t e = esp_ota_write(handle, data, len);
    if (e != ESP_OK) {
        OTA_FAIL("write failed: %s", esp_err_to_name(e));
    }
    written += len;
    return true;
}

bool ota_end(char *err, size_t err_sz)
{
    return ota_validate(err, err_sz) && ota_activate(err, err_sz);
}

bool ota_validate(char *err, size_t err_sz)
{
    if (!in_progress) {
        if (err && err_sz) {
            snprintf(err, err_sz, "no update in progress");
        }
        return false;
    }

    esp_err_t e = esp_ota_end(handle);
    in_progress = false;
    if (e != ESP_OK) {
        handle = 0;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
            if (err && err_sz) {
                snprintf(err, err_sz, "the uploaded image is corrupt or incomplete");
            }
        } else if (err && err_sz) {
            snprintf(err, err_sz, "update failed: %s", esp_err_to_name(e));
        }
        return false;
    }
    handle = 0;
    ESP_LOGI(TAG, "image valid: %u KiB in '%s'", (unsigned)(written / 1024), target->label);
    return true;
}

bool ota_activate(char *err, size_t err_sz)
{
    if (!target || in_progress) {
        if (err && err_sz) {
            snprintf(err, err_sz, "no validated image to switch to");
        }
        return false;
    }
    esp_err_t e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) {
        if (err && err_sz) {
            snprintf(err, err_sz, "could not switch the boot partition: %s",
                     esp_err_to_name(e));
        }
        return false;
    }
    ESP_LOGI(TAG, "update complete: %u KiB written to '%s', rebooting",
             (unsigned)(written / 1024), target->label);
    return true;
}

void ota_abort(void)
{
    if (in_progress && handle) {
        esp_ota_abort(handle);
    }
    handle = 0;
    in_progress = false;
    written = 0;
}

/* ------------------------------------------------------------------ */
/* Storage partition: single file                                       */

static FILE *sf_fp;
static char  sf_target[160];
static char  sf_tmp[168];

/* Uploads may only land under the storage mount, and may not contain ".." --
 * the path comes straight from a query string. */
static bool path_is_safe(const char *path)
{
    if (!path || strncmp(path, CFG_MOUNT "/", sizeof(CFG_MOUNT)) != 0) {
        return false;
    }
    return strstr(path, "..") == NULL;
}

bool storage_file_begin(const char *path, char *err, size_t err_sz)
{
    if (sf_fp) {
        snprintf(err, err_sz, "an upload is already running");
        return false;
    }
    if (!path_is_safe(path)) {
        snprintf(err, err_sz, "path must be under %s and must not contain '..'", CFG_MOUNT);
        return false;
    }

    snprintf(sf_target, sizeof(sf_target), "%s", path);
    snprintf(sf_tmp, sizeof(sf_tmp), "%s.up", path);

    /* Written to a temporary name and renamed at the end, so an interrupted
     * upload cannot leave a truncated file that the next boot tries to serve. */
    sf_fp = fopen(sf_tmp, "wb");
    if (!sf_fp) {
        snprintf(err, err_sz, "cannot write to %s", sf_tmp);
        return false;
    }
    ESP_LOGI(TAG, "receiving %s", sf_target);
    return true;
}

bool storage_file_write(const void *data, size_t len, char *err, size_t err_sz)
{
    if (!sf_fp) {
        snprintf(err, err_sz, "no upload in progress");
        return false;
    }
    if (fwrite(data, 1, len, sf_fp) != len) {
        storage_file_abort();
        snprintf(err, err_sz, "the storage partition is full or unwritable");
        return false;
    }
    return true;
}

bool storage_file_end(char *err, size_t err_sz)
{
    if (!sf_fp) {
        snprintf(err, err_sz, "no upload in progress");
        return false;
    }
    bool ok = fclose(sf_fp) == 0;
    sf_fp = NULL;
    if (!ok) {
        remove(sf_tmp);
        snprintf(err, err_sz, "could not finish writing the file");
        return false;
    }
    remove(sf_target);
    if (rename(sf_tmp, sf_target) != 0) {
        remove(sf_tmp);
        snprintf(err, err_sz, "could not replace %s", sf_target);
        return false;
    }
    ESP_LOGI(TAG, "%s replaced", sf_target);
    return true;
}

void storage_file_abort(void)
{
    if (sf_fp) {
        fclose(sf_fp);
        sf_fp = NULL;
        remove(sf_tmp);
    }
}

/* ------------------------------------------------------------------ */
/* Storage partition: whole image                                       */

static const esp_partition_t *si_part;
static size_t si_written;
static size_t si_erased;

/* ---- what the user keeps across an image ---- */

/* Everything in the root of the partition is the user's except the database,
 * which is what the image is for. In www/ it is the other way round: the
 * interface comes with the image, and only the pictures the interface itself
 * stored there are the user's. */
static const char *const KEEP_WWW[] = { "plant.svg", "plant_live.svg" };
#define KEEP_MAX       24
#define KEEP_FILE_MAX  (2 * 1024 * 1024)

typedef struct {
    char     path[96];
    uint8_t *data;
    size_t   len;
} kept_t;

static kept_t s_kept[KEEP_MAX];
static int    s_n_kept;
static bool   s_fs_down;       /* unregistered by begin(), not yet remounted */
static int    s_restored, s_restore_failed;

static void keep_free(void)
{
    for (int i = 0; i < s_n_kept; i++) {
        free(s_kept[i].data);
    }
    memset(s_kept, 0, sizeof(s_kept));
    s_n_kept = 0;
}

static bool keep_one(const char *path, char *err, size_t err_sz)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return true;
    }
    if (s_n_kept >= KEEP_MAX || (size_t)st.st_size > KEEP_FILE_MAX) {
        snprintf(err, err_sz, "cannot carry %s over (%u KiB)", path,
                 (unsigned)(st.st_size / 1024));
        return false;
    }
    kept_t *k = &s_kept[s_n_kept];
    /* PSRAM: a scan result is several hundred kilobytes. */
    k->data = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM);
    FILE *f = k->data ? fopen(path, "rb") : NULL;
    size_t got = f ? fread(k->data, 1, (size_t)st.st_size, f) : 0;
    if (f) {
        fclose(f);
    }
    if (!f || got != (size_t)st.st_size) {
        free(k->data);
        k->data = NULL;
        snprintf(err, err_sz, "could not read %s to carry it over", path);
        return false;
    }
    snprintf(k->path, sizeof(k->path), "%s", path);
    k->len = got;
    s_n_kept++;
    return true;
}

/* Read the user's files into memory. Refuses rather than leaves one behind:
 * an update that silently drops the scan result is the thing being fixed. */
static bool keep_collect(char *err, size_t err_sz)
{
    keep_free();
    DIR *d = opendir(CFG_MOUNT);
    if (!d) {
        snprintf(err, err_sz, "the storage partition is not readable");
        return false;
    }
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (strcmp(e->d_name, "o3edb.bin") == 0 ||
            strcmp(e->d_name, "import.json") == 0 ||
            (n > 3 && strcmp(e->d_name + n - 3, ".up") == 0)) {
            continue;
        }
        char path[96];
        if (snprintf(path, sizeof(path), CFG_MOUNT "/%s", e->d_name) >= (int)sizeof(path)) {
            continue;
        }
        ok = keep_one(path, err, err_sz);
    }
    closedir(d);
    for (size_t i = 0; ok && i < sizeof(KEEP_WWW) / sizeof(KEEP_WWW[0]); i++) {
        char path[96];
        snprintf(path, sizeof(path), CFG_WWW_DIR "/%s", KEEP_WWW[i]);
        ok = keep_one(path, err, err_sz);
    }
    if (!ok) {
        keep_free();
        return false;
    }
    size_t total = 0;
    for (int i = 0; i < s_n_kept; i++) {
        total += s_kept[i].len;
    }
    ESP_LOGI(TAG, "carrying %d file(s) over, %u KiB", s_n_kept, (unsigned)(total / 1024));
    return true;
}

static void keep_restore(void)
{
    s_restored = 0;
    s_restore_failed = 0;
    for (int i = 0; i < s_n_kept; i++) {
        const kept_t *k = &s_kept[i];
        FILE *f = fopen(k->path, "wb");
        bool ok = f != NULL;
        /* In pieces, with a breath in between: littlefs erases a block every
         * 4 KiB, and a few hundred of those in one go starve everything
         * else on the core. */
        for (size_t off = 0; ok && off < k->len; off += 4096) {
            size_t n = k->len - off < 4096 ? k->len - off : 4096;
            ok = fwrite(k->data + off, 1, n, f) == n;
            vTaskDelay(1);
        }
        if (f && fclose(f) != 0) {
            ok = false;
        }
        if (ok) {
            s_restored++;
        } else {
            s_restore_failed++;
            ESP_LOGE(TAG, "could not write %s back", k->path);
        }
    }
    ESP_LOGW(TAG, "carried over: %d file(s), %d failed", s_restored, s_restore_failed);
}

void storage_image_kept(int *kept, int *failed)
{
    *kept = s_restored;
    *failed = s_restore_failed;
}

bool storage_image_begin(size_t total_size, char *err, size_t err_sz)
{
    if (si_part) {
        snprintf(err, err_sz, "an image upload is already running");
        return false;
    }
    si_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!si_part) {
        snprintf(err, err_sz, "storage partition not found");
        return false;
    }
    if (total_size > si_part->size) {
        snprintf(err, err_sz, "image is %u KiB, the partition holds %u KiB",
                 (unsigned)(total_size / 1024), (unsigned)(si_part->size / 1024));
        si_part = NULL;
        return false;
    }

    /* The user's files first, while they can still be read. After a failed
     * attempt the filesystem is already gone and what was read then is all
     * there is -- it stays in memory for exactly this second try. */
    if (!s_fs_down && !keep_collect(err, err_sz)) {
        si_part = NULL;
        return false;
    }

    /* The database is read straight from this partition and littlefs has it
     * mounted; both have to let go before the flash underneath them changes. */
    if (!s_fs_down) {
        o3e_db_close();
        esp_vfs_littlefs_unregister("storage");
        s_fs_down = true;
    }

    si_written = 0;
    si_erased = 0;
    ESP_LOGW(TAG, "overwriting the storage partition (%u KiB)",
             (unsigned)(si_part->size / 1024));
    return true;
}

bool storage_image_write(const void *data, size_t len, char *err, size_t err_sz)
{
    if (!si_part) {
        snprintf(err, err_sz, "no image upload in progress");
        return false;
    }

    /* Erase lazily in 64 KiB blocks as the upload advances: erasing 4 MiB up
     * front would stall the HTTP connection long enough to time out. */
    while (si_erased < si_written + len && si_erased < si_part->size) {
        size_t block = 64 * 1024;
        if (si_erased + block > si_part->size) {
            block = si_part->size - si_erased;
        }
        if (esp_partition_erase_range(si_part, si_erased, block) != ESP_OK) {
            storage_image_abort();
            snprintf(err, err_sz, "erasing the storage partition failed");
            return false;
        }
        si_erased += block;
    }

    if (esp_partition_write(si_part, si_written, data, len) != ESP_OK) {
        storage_image_abort();
        snprintf(err, err_sz, "writing the storage partition failed");
        return false;
    }
    si_written += len;
    return true;
}

bool storage_image_end(char *err, size_t err_sz)
{
    if (!si_part) {
        snprintf(err, err_sz, "no image upload in progress");
        return false;
    }
    /* An image without its erased tail: erase what it left out, so no block
     * of the old filesystem is left where the new one expects empty flash. */
    while (si_erased < si_part->size) {
        size_t block = 64 * 1024;
        if (si_erased + block > si_part->size) {
            block = si_part->size - si_erased;
        }
        if (esp_partition_erase_range(si_part, si_erased, block) != ESP_OK) {
            storage_image_abort();
            snprintf(err, err_sz, "erasing the storage partition failed");
            return false;
        }
        si_erased += block;
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "storage partition rewritten: %u KiB", (unsigned)(si_written / 1024));
    si_part = NULL;

    if (!app_config_fs_remount()) {
        /* Stays down, and the files stay in memory: begin() can be called
         * again with a good image. */
        snprintf(err, err_sz, "the new storage image does not mount; "
                              "do not switch the device off, upload again");
        return false;
    }
    s_fs_down = false;
    keep_restore();
    keep_free();
    return true;
}

void storage_image_abort(void)
{
    si_part = NULL;
    si_written = 0;
    si_erased = 0;
}
