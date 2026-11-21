#include "e3_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "can_port.h"
#include "o3e_codec.h"
#include "o3e_db.h"
#include "cJSON.h"
#include "o3e_json.h"
#include "poller.h"

static const char *TAG = "scan";

static scan_status_t     st;
/* Kept out of scan_status_t so a status poll does not copy several kilobytes
 * of device records it never looks at. */
static scan_ecu_t        ecus[SCAN_MAX_ECUS];
static SemaphoreHandle_t st_lock;
static volatile bool     abort_req;
static TaskHandle_t      scan_task_h;

/* Bitmap of responding DIDs per ECU, 4001 bits rounded up. */
#define DID_BITS ((SCAN_DID_LAST + 1 + 7) / 8)
static uint8_t *found_bits[SCAN_MAX_ECUS];

/* Response lengths matter: a DID's layout can depend on them (see the variant
 * table in the database), so the scan records what each ECU actually returned. */
static uint16_t *resp_len[SCAN_MAX_ECUS];

static void set_bit(uint8_t *bits, uint16_t did) { bits[did >> 3] |= (uint8_t)(1u << (did & 7)); }
static bool get_bit(const uint8_t *bits, uint16_t did) { return bits[did >> 3] & (1u << (did & 7)); }

void e3_scan_status(scan_status_t *out)
{
    if (!st_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(st_lock, portMAX_DELAY);
    *out = st;
    xSemaphoreGive(st_lock);
}

bool e3_scan_running(void)
{
    scan_status_t s;
    e3_scan_status(&s);
    return s.phase == SCAN_ECUS || s.phase == SCAN_DIDS;
}

void e3_scan_abort(void) { abort_req = true; }

/* Copy a decoded field into a fixed buffer. Enum fields decode to
 * {"ID": n, "Text": "..."}, scalars to a plain value, so both shapes are
 * handled here rather than at every call site. */
static void take_field(const cJSON *root, const char *key, char *out, size_t out_sz)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsObject(v)) {
        const cJSON *txt = cJSON_GetObjectItemCaseSensitive(v, "Text");
        if (cJSON_IsString(txt)) {
            snprintf(out, out_sz, "%s", txt->valuestring);
        }
    } else if (cJSON_IsString(v)) {
        snprintf(out, out_sz, "%s", v->valuestring);
    }
}

/* Trim trailing spaces and NULs; the E3 pads its text fields with both. */
static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

/* DID 256 (BusIdentification) is the ECU fingerprint open3e probes with: bus
 * type, what kind of controller this is, its software and hardware versions
 * and the VIN.
 *
 * Decoding it through the real codec rather than reading bytes at fixed
 * offsets matters: the record has a length variant, and hand-rolled offsets
 * would silently produce nonsense on a device that returns the other one. */
static void describe_ecu(scan_ecu_t *e, const uint8_t *payload, size_t len)
{
    /* open3e's default key for an unnamed device is its hex address. */
    snprintf(e->name, sizeof(e->name), "0x%03x", e->addr);
    snprintf(e->prop, sizeof(e->prop), "unknown");

    char *json = o3e_db_json(256, (uint16_t)len);
    o3e_node_t *node = json ? o3e_codec_compile(json) : NULL;
    free(json);
    if (!node) {
        return;
    }
    char *decoded = o3e_codec_decode_json(node, payload, len);
    o3e_codec_free(node);
    if (!decoded) {
        return;
    }
    cJSON *root = cJSON_Parse(decoded);
    free(decoded);
    if (!root) {
        return;
    }

    take_field(root, "DeviceProperty", e->prop, sizeof(e->prop));
    take_field(root, "DeviceFunction", e->function, sizeof(e->function));
    take_field(root, "BusType", e->bus_type, sizeof(e->bus_type));
    take_field(root, "SW-Version", e->sw, sizeof(e->sw));
    take_field(root, "HW-Version", e->hw, sizeof(e->hw));
    take_field(root, "VIN", e->vin, sizeof(e->vin));
    rtrim(e->vin);
    cJSON_Delete(root);
}

/* While somebody else is programming a control unit the bus refuses every
 * request (bus_guard.h). A scan that carried on would record all of them as
 * "not there", so it waits instead; aborting still works. */
static void wait_for_bus(void)
{
    while (!abort_req &&
           bus_guard_blocked((uint32_t)(esp_timer_get_time() / 1000))) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* DID 377 carries the Viessmann identification number, which is what actually
 * names the product ("Vitocal 250-A" rather than "HPMUMASTER"). Not every
 * device answers it, so a failure here is not an error. */
static void read_viessmann_id(scan_ecu_t *e)
{
    uint8_t buf[64];
    size_t n = 0;
    wait_for_bus();
    if (can_read_did(e->addr, 377, buf, sizeof(buf), &n, UDS_SCAN_P2_MS).err != UDS_OK) {
        return;
    }
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < sizeof(e->vid); i++) {
        if (buf[i] >= 0x20 && buf[i] < 0x7F) {
            e->vid[o++] = (char)buf[i];
        }
    }
    e->vid[o] = '\0';
    rtrim(e->vid);
}

static uint16_t cob_lo = SCAN_COB_FIRST;
static uint16_t cob_hi = SCAN_COB_LAST;

static void scan_ecus(void)
{
    /* DID 256 is a 36-byte record; 256 bytes covers any variant of it. */
    uint8_t buf[256];

    xSemaphoreTake(st_lock, portMAX_DELAY);
    st.phase = SCAN_ECUS;
    st.total = (uint32_t)(cob_hi - cob_lo) + 1;
    st.probed = 0;
    st.n_ecus = 0;
    snprintf(st.message, sizeof(st.message), "probing COB-IDs 0x%03X..0x%03X",
             cob_lo, cob_hi);
    xSemaphoreGive(st_lock);

    for (uint16_t tx = cob_lo; tx <= cob_hi && !abort_req; tx++) {
        /* An ECU at 0x680 answers on 0x690, so 0x690 must not be probed as a
         * request address of its own -- open3e skips these too. */
        bool is_reply_addr = false;
        xSemaphoreTake(st_lock, portMAX_DELAY);
        for (uint8_t i = 0; i < st.n_ecus; i++) {
            if (UDS_RX_OF(ecus[i].addr) == tx) {
                is_reply_addr = true;
            }
        }
        st.cur_did = tx;
        st.probed++;
        st.last_progress_ms = (uint32_t)(esp_timer_get_time() / 1000);
        xSemaphoreGive(st_lock);
        if (is_reply_addr) {
            continue;
        }

        /* Clearing the buffer keeps a previous device's record from being read
         * back as this one's if a response ever comes up short. */
        memset(buf, 0, sizeof(buf));
        size_t n = 0;
        wait_for_bus();
        uds_result_t r = can_read_did(tx, 256, buf, sizeof(buf), &n, UDS_SCAN_P2_MS);
        if (r.err != UDS_OK) {
            continue;
        }
        /* BusIdentification is 36 bytes; anything that cannot even carry the
         * four identification bytes is not a device answering for itself. */
        if (n < 4) {
            ESP_LOGW(TAG, "0x%03X answered DID 256 with only %u byte(s), ignoring",
                     tx, (unsigned)n);
            continue;
        }

        xSemaphoreTake(st_lock, portMAX_DELAY);
        if (st.n_ecus < SCAN_MAX_ECUS) {
            scan_ecu_t *e = &ecus[st.n_ecus++];
            memset(e, 0, sizeof(*e));
            e->addr = tx;
            describe_ecu(e, buf, n);
        }
        xSemaphoreGive(st_lock);

        /* Outside the lock: this is another bus round trip. */
        if (st.n_ecus) {
            scan_ecu_t *e = &ecus[st.n_ecus - 1];
            read_viessmann_id(e);
            ESP_LOGI(TAG, "ECU 0x%03X: %s / %s, SW %s, VIN %s, ident %s",
                     tx, e->prop, e->function, e->sw, e->vin, e->vid);
        }
    }
}

/* The ISO-TP receive buffer is 4 KiB and would dominate the scan task's stack,
 * so it lives on the heap and is shared across the whole sweep. */
static uint8_t *scan_buf;

/* The per-ECU result maps go to PSRAM explicitly.
 *
 * They are 8.5 KiB each, which is below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL,
 * so a plain calloc() puts them in internal RAM -- 66 KiB for eight ECUs, 133
 * for sixteen, taken from the same pool the Wi-Fi stack and the web server
 * need. They are written once per datapoint and read once at the end, which is
 * exactly what PSRAM is for. */
static void *scan_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(n, size);   /* boards without PSRAM still work */
}

static void scan_dids_of(uint8_t idx, scan_mode_t mode)
{
    size_t n;
    uint16_t addr;
    xSemaphoreTake(st_lock, portMAX_DELAY);
    addr = ecus[idx].addr;
    st.cur_ecu = idx;
    xSemaphoreGive(st_lock);

    uint32_t since_log = 0;
    for (uint16_t did = SCAN_DID_FIRST; did <= SCAN_DID_LAST && !abort_req; did++) {
        if (mode == SCAN_MODE_KNOWN && !o3e_db_find(did)) {
            continue;
        }
        if (++since_log >= 200) {
            since_log = 0;
            can_stats_t cs;
            can_port_stats(&cs);
            ESP_LOGI(TAG, "0x%03X at DID %u: %u found, bus %s (TEC %u/REC %u), "
                     "heap %u KiB internal",
                     addr, did, ecus[idx].n_dids, cs.state ? cs.state : "?",
                     cs.tx_err_count, cs.rx_err_count,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        }

        wait_for_bus();
        uds_result_t r = can_read_did(addr, did, scan_buf, ISOTP_MAX_PAYLOAD, &n,
                                      UDS_SCAN_P2_MS);

        xSemaphoreTake(st_lock, portMAX_DELAY);
        st.cur_did = did;
        st.probed++;
        st.last_progress_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (r.err == UDS_OK) {
            set_bit(found_bits[idx], did);
            resp_len[idx][did] = (uint16_t)n;
            ecus[idx].n_dids++;
        }
        xSemaphoreGive(st_lock);
    }
}

/* A rescan must not throw away names the user gave the devices, so the old
 * result is consulted for a name at the same address before it is replaced. */
static void restore_names(void)
{
    char *raw = app_config_read_file(CFG_SYSTEM_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!root) {
        return;
    }
    const cJSON *dev;
    cJSON_ArrayForEach(dev, cJSON_GetObjectItem(root, "devices")) {
        const cJSON *addr = cJSON_GetObjectItem(dev, "addr");
        const cJSON *name = cJSON_GetObjectItem(dev, "name");
        if (!cJSON_IsNumber(addr) || !cJSON_IsString(name)) {
            continue;
        }
        xSemaphoreTake(st_lock, portMAX_DELAY);
        for (uint8_t i = 0; i < st.n_ecus; i++) {
            if (ecus[i].addr == (uint16_t)addr->valuedouble) {
                snprintf(ecus[i].name, sizeof(ecus[i].name), "%s", name->valuestring);
            }
        }
        xSemaphoreGive(st_lock);
    }
    cJSON_Delete(root);
}

/* Serialise the result in the shape the web UI and poller expect. Kept close
 * to open3e's devices.json so the file is recognisable to anyone who has seen
 * one, with the identity fields and the discovered DID list added.
 *
 * Written straight to the file rather than assembled in memory: with thousands
 * of datapoints across several ECUs the result runs to hundreds of kilobytes,
 * and a growable buffer would take that out of the heap in a series of
 * doubling reallocations -- the smaller of which come from internal RAM, the
 * scarce pool the network stack shares. This runs in constant memory. */
static bool write_result(void)
{
    scan_status_t s;
    e3_scan_status(&s);

    FILE *f = app_config_begin_write(CFG_SYSTEM_PATH);
    if (!f) {
        ESP_LOGE(TAG, "cannot open the result file for writing");
        return false;
    }

    fputs("{\"dbVersion\": ", f);
    app_config_fput_json_str(f, o3e_db_version());
    fprintf(f, ", \"mode\": \"%s\", \"devices\": [",
            s.mode == SCAN_MODE_FULL ? "full" : "known");

    for (uint8_t i = 0; i < s.n_ecus; i++) {
        const scan_ecu_t *e = &ecus[i];
        if (i) {
            fputs(", ", f);
        }
        fprintf(f, "{\"addr\": %u, \"addrHex\": \"0x%03X\", \"name\": ",
                e->addr, e->addr);
        app_config_fput_json_str(f, e->name);
        fputs(", \"prop\": ", f);
        app_config_fput_json_str(f, e->prop);
        fputs(", \"function\": ", f);
        app_config_fput_json_str(f, e->function);
        fputs(", \"busType\": ", f);
        app_config_fput_json_str(f, e->bus_type);
        fputs(", \"sw\": ", f);
        app_config_fput_json_str(f, e->sw);
        fputs(", \"hw\": ", f);
        app_config_fput_json_str(f, e->hw);
        fputs(", \"vin\": ", f);
        app_config_fput_json_str(f, e->vin);
        fputs(", \"ident\": ", f);
        app_config_fput_json_str(f, e->vid);

        /* Pairs of [did, responseLength], not objects: the name and a "known"
         * flag are both already answered by the database, and repeating them
         * per datapoint pushed the file past the free space on the partition. */
        fputs(", \"dids\": [", f);
        bool first = true;
        for (uint16_t did = SCAN_DID_FIRST; did <= SCAN_DID_LAST; did++) {
            if (!found_bits[i] || !get_bit(found_bits[i], did)) {
                continue;
            }
            fprintf(f, "%s[%u,%u]", first ? "" : ",", did, resp_len[i][did]);
            first = false;
        }
        fputs("]}", f);
    }
    fputs("]}", f);

    if (ferror(f)) {
        app_config_abort_write(CFG_SYSTEM_PATH, f);
        ESP_LOGE(TAG, "writing the result failed - storage partition full?");
        return false;
    }
    return app_config_commit_write(CFG_SYSTEM_PATH, f);
}

static void free_bitmaps(void)
{
    free(scan_buf);
    scan_buf = NULL;
    for (int i = 0; i < SCAN_MAX_ECUS; i++) {
        free(found_bits[i]);
        found_bits[i] = NULL;
        free(resp_len[i]);
        resp_len[i] = NULL;
    }
}

static void scan_task(void *arg)
{
    scan_mode_t mode = (scan_mode_t)(intptr_t)arg;

    xSemaphoreTake(st_lock, portMAX_DELAY);
    st.mode = mode;
    st.started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(st_lock);

    poller_pause(true);
    scan_ecus();

    scan_status_t s;
    e3_scan_status(&s);
    if (abort_req || s.n_ecus == 0) {
        xSemaphoreTake(st_lock, portMAX_DELAY);
        st.phase = abort_req ? SCAN_IDLE : SCAN_FAILED;
        snprintf(st.message, sizeof(st.message), abort_req
                 ? "aborted" : "no ECU answered - check wiring, termination and bitrate");
        xSemaphoreGive(st_lock);
        poller_pause(false);
        scan_task_h = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Allocate the per-ECU result maps now that the ECU count is known. */
    size_t per_ecu = 0;
    scan_buf = malloc(ISOTP_MAX_PAYLOAD);
    if (!scan_buf) {
        xSemaphoreTake(st_lock, portMAX_DELAY);
        st.phase = SCAN_FAILED;
        snprintf(st.message, sizeof(st.message), "out of memory");
        xSemaphoreGive(st_lock);
        poller_pause(false);
        scan_task_h = NULL;
        vTaskDelete(NULL);
        return;
    }
    for (uint8_t i = 0; i < s.n_ecus; i++) {
        found_bits[i] = scan_calloc(1, DID_BITS);
        resp_len[i] = scan_calloc(SCAN_DID_LAST + 1, sizeof(uint16_t));
        if (!found_bits[i] || !resp_len[i]) {
            free_bitmaps();
            xSemaphoreTake(st_lock, portMAX_DELAY);
            st.phase = SCAN_FAILED;
            snprintf(st.message, sizeof(st.message),
                     "out of memory for %u ECUs (%u KiB internal, %u KiB PSRAM free)",
                     s.n_ecus,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            xSemaphoreGive(st_lock);
            poller_pause(false);
            scan_task_h = NULL;
            vTaskDelete(NULL);
            return;
        }
    }
    per_ecu = (mode == SCAN_MODE_KNOWN) ? o3e_db_count()
                                        : (SCAN_DID_LAST - SCAN_DID_FIRST + 1);

    restore_names();

    ESP_LOGI(TAG, "scanning %u ECU(s); heap: %u KiB internal, %u KiB PSRAM",
             s.n_ecus,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    xSemaphoreTake(st_lock, portMAX_DELAY);
    st.phase = SCAN_DIDS;
    st.probed = 0;
    st.total = (uint32_t)per_ecu * s.n_ecus;
    snprintf(st.message, sizeof(st.message), "reading datapoints of %u ECU(s)", s.n_ecus);
    xSemaphoreGive(st_lock);

    for (uint8_t i = 0; i < s.n_ecus && !abort_req; i++) {
        scan_dids_of(i, mode);
        /* Persist after each ECU: a full sweep takes minutes and a reboot
         * partway through should not throw away what was already found. */
        bool saved = write_result();
        ESP_LOGI(TAG, "ECU %u/%u done: %u datapoints, save %s; "
                 "heap %u KiB internal / %u KiB PSRAM",
                 i + 1, s.n_ecus, ecus[i].n_dids, saved ? "ok" : "FAILED",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }

    bool ok = write_result();
    free_bitmaps();

    xSemaphoreTake(st_lock, portMAX_DELAY);
    st.phase = abort_req ? SCAN_IDLE : (ok ? SCAN_DONE : SCAN_FAILED);
    uint32_t secs = ((uint32_t)(esp_timer_get_time() / 1000) - st.started_ms) / 1000;
    if (abort_req) {
        snprintf(st.message, sizeof(st.message), "aborted");
    } else if (ok) {
        snprintf(st.message, sizeof(st.message), "finished in %us", (unsigned)secs);
    } else {
        /* Almost always the storage partition: the result file lives next to a
         * 1.6 MB database on a 4 MB partition, and it is written via a
         * temporary copy, so it needs twice its own size free. */
        snprintf(st.message, sizeof(st.message),
                 "could not save the result - storage partition full?");
    }
    xSemaphoreGive(st_lock);

    ESP_LOGI(TAG, "scan finished (%s): %s",
             st.phase == SCAN_DONE ? "ok" : st.phase == SCAN_FAILED ? "failed" : "aborted",
             st.message);
    /* The selection may now refer to datapoints this scan added or removed. */
    poller_reload();
    poller_pause(false);
    scan_task_h = NULL;
    vTaskDelete(NULL);
}

bool e3_scan_start(scan_mode_t mode, uint16_t first, uint16_t last)
{
    if (!st_lock) {
        st_lock = xSemaphoreCreateMutex();
        if (!st_lock) {
            return false;
        }
    }
    if (e3_scan_running()) {
        return false;
    }
    abort_req = false;

    cob_lo = first ? first : SCAN_COB_FIRST;
    cob_hi = last ? last : SCAN_COB_LAST;
    if (cob_hi < cob_lo) {
        cob_hi = cob_lo;
    }
    if (cob_hi > SCAN_COB_MAX) {
        cob_hi = SCAN_COB_MAX;
    }

    xSemaphoreTake(st_lock, portMAX_DELAY);
    memset(&st, 0, sizeof(st));
    st.cob_first = cob_lo;
    st.cob_last = cob_hi;
    memset(ecus, 0, sizeof(ecus));
    st.phase = SCAN_ECUS;
    st.mode = mode;
    xSemaphoreGive(st_lock);

    /* 6 KiB: the result serialiser builds the JSON on the heap, but the DID
     * loop keeps a 4 KiB ISO-TP buffer on the stack. */
    return xTaskCreate(scan_task, "scan", 6144, (void *)(intptr_t)mode, 4,
                       &scan_task_h) == pdPASS;
}

char *e3_scan_result_json(void)
{
    return app_config_read_file(CFG_SYSTEM_PATH);
}

bool e3_scan_rename(uint16_t addr, const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    /* Patch the stored result rather than the in-memory scan state: the state
     * is gone after a reboot, the file is what everything else reads. */
    char *raw = app_config_read_file(CFG_SYSTEM_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!root) {
        return false;
    }

    bool found = false;
    cJSON *dev;
    cJSON_ArrayForEach(dev, cJSON_GetObjectItem(root, "devices")) {
        const cJSON *a = cJSON_GetObjectItem(dev, "addr");
        if (cJSON_IsNumber(a) && (uint16_t)a->valuedouble == addr) {
            cJSON_ReplaceItemInObject(dev, "name", cJSON_CreateString(name));
            found = true;
            break;
        }
    }

    bool ok = false;
    if (found) {
        char *out = cJSON_PrintUnformatted(root);
        if (out) {
            ok = app_config_write_file(CFG_SYSTEM_PATH, out, strlen(out));
            free(out);
        }
        /* Keep the live state in step so a status poll does not show the old
         * name until the next scan. */
        if (ok && st_lock) {
            xSemaphoreTake(st_lock, portMAX_DELAY);
            for (uint8_t i = 0; i < st.n_ecus; i++) {
                if (ecus[i].addr == addr) {
                    snprintf(ecus[i].name, sizeof(ecus[i].name), "%s", name);
                }
            }
            xSemaphoreGive(st_lock);
        }
    }
    cJSON_Delete(root);
    return ok;
}
