#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota.h"
#include "update_pkg.h"

static const char *TAG = "update";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;                 /* one update or check at a time */
static update_status_t s_st;
static int64_t s_backup_us = -1;

/* The package as it arrives. */
static struct {
    uint8_t      head[UPDATE_PKG_HEADER];
    size_t       head_n;
    bool         have_pkg;
    update_pkg_t pkg;
    size_t       fw_got, st_got;
    uint32_t     fw_crc, st_crc;
    uint8_t     *st;                /* storage image, PSRAM */
    bool         ota_open;
} rx;

const char *update_state_name(update_state_t s)
{
    static const char *const n[] = { "idle", "checking", "receiving",
                                     "installing", "done", "failed" };
    return n[s];
}

void update_note_backup(void)
{
    s_backup_us = esp_timer_get_time();
}

static int32_t backup_age_s(void)
{
    return s_backup_us < 0 ? -1 : (int32_t)((esp_timer_get_time() - s_backup_us) / 1000000);
}

void update_status(update_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
    out->backup_age_s = backup_age_s();
}

static bool claim(char *err, size_t err_sz)
{
    bool ok;
    portENTER_CRITICAL(&s_lock);
    ok = !s_busy && s_st.state != UPDATE_DONE;
    if (ok) {
        s_busy = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!ok) {
        snprintf(err, err_sz, "an update is already running");
    }
    return ok;
}

static void release(void)
{
    s_busy = false;
}

static void fail(const char *why)
{
    ESP_LOGE(TAG, "%s", why);
    portENTER_CRITICAL(&s_lock);
    s_st.state = UPDATE_FAILED;
    snprintf(s_st.error, sizeof(s_st.error), "%s", why);
    portEXIT_CRITICAL(&s_lock);
}

static bool backup_ok(char *err, size_t err_sz)
{
    int32_t age = backup_age_s();
    if (age < 0 || age > UPDATE_BACKUP_VALID_S) {
        snprintf(err, err_sz, "download a backup first");
        return false;
    }
    return true;
}

static void rx_reset(void)
{
    if (rx.ota_open) {
        ota_abort();
    }
    free(rx.st);
    memset(&rx, 0, sizeof(rx));
}

/* ------------------------------------------------------------------ */
/* Receiving                                                            */

static bool start(size_t total, bool online, char *err, size_t err_sz)
{
    rx_reset();
    portENTER_CRITICAL(&s_lock);
    s_st.state = UPDATE_RECEIVING;
    s_st.online = online;
    s_st.total = (uint32_t)total;
    s_st.received = 0;
    s_st.version[0] = '\0';
    s_st.error[0] = '\0';
    s_st.kept = s_st.keep_failed = 0;
    portEXIT_CRITICAL(&s_lock);
    if (total <= UPDATE_PKG_HEADER) {
        snprintf(err, err_sz, "this file is too short to be an update package");
        return false;
    }
    return true;
}

bool update_begin(size_t total, char *err, size_t err_sz)
{
    if (!backup_ok(err, err_sz) || !claim(err, err_sz)) {
        return false;
    }
    if (!start(total, false, err, err_sz)) {
        fail(err);
        release();
        return false;
    }
    return true;
}

static bool header_complete(char *err, size_t err_sz)
{
    const char *why = update_pkg_header(rx.head, rx.head_n, s_st.total, &rx.pkg);
    if (why) {
        snprintf(err, err_sz, "%s", why);
        return false;
    }
    const esp_partition_t *sp = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!sp || rx.pkg.st_full != sp->size) {
        snprintf(err, err_sz, "the package was built for a different partition layout");
        return false;
    }
    rx.st = heap_caps_malloc(rx.pkg.st_len, MALLOC_CAP_SPIRAM);
    if (!rx.st) {
        snprintf(err, err_sz, "not enough memory to hold the update (%u KiB)",
                 (unsigned)(rx.pkg.st_len / 1024));
        return false;
    }
    if (!ota_begin(rx.pkg.fw_len, err, err_sz)) {
        return false;
    }
    rx.ota_open = true;
    rx.have_pkg = true;
    portENTER_CRITICAL(&s_lock);
    memcpy(s_st.version, rx.pkg.version, sizeof(s_st.version));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "package %s: firmware %u KiB, storage %u KiB", rx.pkg.version,
             (unsigned)(rx.pkg.fw_len / 1024), (unsigned)(rx.pkg.st_len / 1024));
    return true;
}

static bool feed(const uint8_t *p, size_t len, char *err, size_t err_sz)
{
    while (len) {
        size_t n;
        if (!rx.have_pkg) {
            n = UPDATE_PKG_HEADER - rx.head_n;
            n = n < len ? n : len;
            memcpy(rx.head + rx.head_n, p, n);
            rx.head_n += n;
            if (rx.head_n == UPDATE_PKG_HEADER && !header_complete(err, err_sz)) {
                return false;
            }
        } else if (rx.fw_got < rx.pkg.fw_len) {
            n = rx.pkg.fw_len - rx.fw_got;
            n = n < len ? n : len;
            if (!ota_write(p, n, err, err_sz)) {
                rx.ota_open = false;      /* ota_write aborts on failure */
                return false;
            }
            rx.fw_crc = update_crc32(rx.fw_crc, p, n);
            rx.fw_got += n;
        } else {
            n = rx.pkg.st_len - rx.st_got;
            n = n < len ? n : len;
            if (n == 0) {
                snprintf(err, err_sz, "more data than the package announced");
                return false;
            }
            memcpy(rx.st + rx.st_got, p, n);
            rx.st_crc = update_crc32(rx.st_crc, p, n);
            rx.st_got += n;
        }
        p += n;
        len -= n;
        s_st.received += (uint32_t)n;
    }
    return true;
}

bool update_feed(const void *data, size_t len, char *err, size_t err_sz)
{
    if (!s_busy || s_st.state != UPDATE_RECEIVING) {
        snprintf(err, err_sz, "no update in progress");
        return false;
    }
    if (!feed(data, len, err, err_sz)) {
        rx_reset();
        fail(err);
        release();
        return false;
    }
    return true;
}

void update_abort(void)
{
    if (s_busy && s_st.state == UPDATE_RECEIVING) {
        rx_reset();
        fail("the transfer broke off; nothing was changed");
        release();
    }
}

/* ------------------------------------------------------------------ */
/* Installing                                                           */

static bool write_storage(char *err, size_t err_sz)
{
    if (!storage_image_begin(rx.pkg.st_len, err, err_sz)) {
        return false;
    }
    for (size_t off = 0; off < rx.pkg.st_len; off += 4096) {
        size_t n = rx.pkg.st_len - off < 4096 ? rx.pkg.st_len - off : 4096;
        if (!storage_image_write(rx.st + off, n, err, err_sz)) {
            return false;
        }
        /* Flash writes stall the core; let the bus tasks in between. */
        if ((off & 0xFFFF) == 0) {
            vTaskDelay(1);
        }
    }
    return storage_image_end(err, err_sz);
}

bool update_finish(char *err, size_t err_sz)
{
    if (!s_busy || s_st.state != UPDATE_RECEIVING) {
        snprintf(err, err_sz, "no update in progress");
        return false;
    }
    const char *why = NULL;
    if (!rx.have_pkg || rx.fw_got != rx.pkg.fw_len || rx.st_got != rx.pkg.st_len) {
        why = "the package is incomplete";
    } else if (rx.fw_crc != rx.pkg.fw_crc) {
        why = "the firmware in the package is damaged";
    } else if (rx.st_crc != rx.pkg.st_crc) {
        why = "the storage image in the package is damaged";
    }
    if (why) {
        snprintf(err, err_sz, "%s; nothing was changed", why);
        rx_reset();
        fail(err);
        release();
        return false;
    }
    rx.ota_open = false;
    if (!ota_validate(err, err_sz)) {
        rx_reset();
        fail(err);
        release();
        return false;
    }

    /* From here on the running system is being changed. The image is in
     * memory, so a write that fails is simply done again -- giving up would
     * leave a device without a filesystem. */
    s_st.state = UPDATE_INSTALLING;
    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        ok = write_storage(err, err_sz);
        if (!ok) {
            ESP_LOGE(TAG, "storage attempt %d: %s", attempt, err);
            storage_image_abort();
        }
    }
    if (!ok) {
        /* Released, so the upload can be repeated: the user's files are
         * still in memory (ota.c), and a restart now would find a partition
         * that does not mount. */
        char msg[160];
        snprintf(msg, sizeof(msg), "writing the storage partition failed (%.50s); "
                 "do not switch the device off, start the update again", err);
        snprintf(err, err_sz, "%s", msg);
        rx_reset();
        fail(msg);
        release();
        return false;
    }
    storage_image_kept(&s_st.kept, &s_st.keep_failed);
    free(rx.st);
    rx.st = NULL;

    if (!ota_activate(err, err_sz)) {
        fail(err);
        release();
        return false;
    }
    s_st.state = UPDATE_DONE;
    ESP_LOGW(TAG, "update to %s installed, %d file(s) carried over", s_st.version, s_st.kept);
    return true;
}

/* ------------------------------------------------------------------ */
/* Online                                                               */

static char s_channel[8];
static bool s_install;

static esp_http_client_handle_t http_open(const char *url, int64_t *len,
                                          char *err, size_t err_sz)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .user_agent = "open3e-gateway",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(err, err_sz, "out of memory");
        return NULL;
    }
    esp_err_t e = esp_http_client_open(c, 0);
    if (e != ESP_OK) {
        snprintf(err, err_sz, "the update server is not reachable (%s)", esp_err_to_name(e));
        esp_http_client_cleanup(c);
        return NULL;
    }
    *len = esp_http_client_fetch_headers(c);
    int code = esp_http_client_get_status_code(c);
    if (code != 200) {
        snprintf(err, err_sz, code == 404 ? "the server has no update for this channel"
                                          : "the update server answered HTTP %d", code);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

static void http_done(esp_http_client_handle_t c)
{
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

static bool do_check(char *err, size_t err_sz)
{
    char url[128];
    snprintf(url, sizeof(url), UPDATE_SITE "/bin/%s/update.json", s_channel);
    int64_t len = 0;
    esp_http_client_handle_t c = http_open(url, &len, err, err_sz);
    if (!c) {
        return false;
    }
    char body[512];
    int got = 0;
    for (;;) {
        int n = esp_http_client_read(c, body + got, (int)sizeof(body) - 1 - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }
    http_done(c);
    body[got] = '\0';
    cJSON *j = cJSON_Parse(body);
    const cJSON *ver = j ? cJSON_GetObjectItem(j, "version") : NULL;
    const cJSON *size = j ? cJSON_GetObjectItem(j, "size") : NULL;
    bool ok = cJSON_IsString(ver) && cJSON_IsNumber(size);
    if (ok) {
        portENTER_CRITICAL(&s_lock);
        snprintf(s_st.avail_channel, sizeof(s_st.avail_channel), "%s", s_channel);
        snprintf(s_st.avail_version, sizeof(s_st.avail_version), "%s", ver->valuestring);
        s_st.avail_size = (uint32_t)size->valuedouble;
        portEXIT_CRITICAL(&s_lock);
    } else {
        snprintf(err, err_sz, "the update server sent something unreadable");
    }
    cJSON_Delete(j);
    return ok;
}

static bool do_install(char *err, size_t err_sz)
{
    char url[128];
    snprintf(url, sizeof(url), UPDATE_SITE "/bin/%s/open3e-update.bin", s_channel);
    int64_t len = 0;
    esp_http_client_handle_t c = http_open(url, &len, err, err_sz);
    if (!c) {
        return false;
    }
    if (len <= 0 || len > 16 * 1024 * 1024) {
        snprintf(err, err_sz, "the update server did not say how large the package is");
        http_done(c);
        return false;
    }
    if (!start((size_t)len, true, err, err_sz)) {
        http_done(c);
        return false;
    }
    char *buf = malloc(4096);
    bool ok = buf != NULL;
    if (!ok) {
        snprintf(err, err_sz, "out of memory");
    }
    while (ok && s_st.received < (uint32_t)len) {
        int n = esp_http_client_read(c, buf, 4096);
        if (n <= 0) {
            snprintf(err, err_sz, "the download broke off; nothing was changed");
            ok = false;
        } else {
            ok = feed((const uint8_t *)buf, (size_t)n, err, err_sz);
        }
    }
    free(buf);
    http_done(c);
    if (!ok) {
        rx_reset();
        return false;
    }
    /* finish() does its own failure bookkeeping, including release(). */
    return update_finish(err, err_sz);
}

static void online_task(void *arg)
{
    (void)arg;
    char err[160] = "";
    if (s_install) {
        if (do_install(err, sizeof(err))) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }
        if (s_st.state != UPDATE_FAILED) {
            fail(err);
            release();
        }
    } else {
        if (do_check(err, sizeof(err))) {
            s_st.state = UPDATE_IDLE;
        } else {
            fail(err);
        }
        release();
    }
    vTaskDelete(NULL);
}

static bool online_start(const char *channel, bool install, char *err, size_t err_sz)
{
    if (strcmp(channel, "release") != 0 && strcmp(channel, "dev") != 0) {
        snprintf(err, err_sz, "channel must be release or dev");
        return false;
    }
    if (install && !backup_ok(err, err_sz)) {
        return false;
    }
    if (!claim(err, err_sz)) {
        return false;
    }
    snprintf(s_channel, sizeof(s_channel), "%s", channel);
    s_install = install;
    portENTER_CRITICAL(&s_lock);
    s_st.state = install ? UPDATE_RECEIVING : UPDATE_CHECKING;
    s_st.online = true;
    s_st.error[0] = '\0';
    s_st.received = s_st.total = 0;
    portEXIT_CRITICAL(&s_lock);
    /* TLS handshake on this stack. */
    if (xTaskCreate(online_task, "update", 10240, NULL, 4, NULL) != pdPASS) {
        fail("out of memory");
        release();
        snprintf(err, err_sz, "out of memory");
        return false;
    }
    return true;
}

bool update_online_check(const char *channel, char *err, size_t err_sz)
{
    return online_start(channel, false, err, err_sz);
}

bool update_online_install(const char *channel, char *err, size_t err_sz)
{
    return online_start(channel, true, err, err_sz);
}
