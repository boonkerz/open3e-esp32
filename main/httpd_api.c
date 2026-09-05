#include "httpd_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "can_port.h"
#include "cantrace.h"
#include "collect.h"
#include "crashlog.h"
#include "e3_scan.h"
#include "em380.h"
#include "hold.h"
#include "ha_disco.h"
#include "mqtt_pub.h"
#include "net_prov.h"
#include "o3e_codec.h"
#include "o3e_db.h"
#include "o3e_json.h"
#include "ota.h"
#include "poller.h"
#include "raw_relay.h"
#include "sysinfo.h"

static const char *TAG = "http";
static httpd_handle_t server;

/* Restart after the handler has returned.
 *
 * Calling esp_restart() from inside a handler cuts the response off before the
 * server has finished writing it and closed the connection, so the browser
 * waits for an answer that never arrives -- the request simply hangs. Handing
 * the reboot to a timer lets the handler return, the response reach the client
 * and the socket close first. */
static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "restarting on request");
    esp_restart();
}

static void restart_soon(uint32_t delay_ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t ta = { .callback = restart_cb, .name = "restart" };
        if (esp_timer_create(&ta, &t) != ESP_OK) {
            return;
        }
    }
    esp_timer_start_once(t, (uint64_t)delay_ms * 1000);
}

#define MAX_BODY 16384

/* Bumped whenever /api/rawread or /api/rawwrite's request/response shape
 * changes, so a caller can tell a firmware too old to have them apart from
 * one that simply answered "no" (see docs/raw-gateway-api.md upstream). */
#define RAW_API_VERSION 1

/* ------------------------------------------------------------------ */
/* helpers                                                              */

/* Send a string literal as a chunk.
 *
 * Passing the length by hand invites exactly the mistake that made this
 * necessary: "{\"n\":{" was counted as five characters, so the opening brace of
 * the inner object never went out and every response was invalid JSON. The
 * browser then silently had no datapoint names and blamed the database. */
static esp_err_t send_chunk_str(httpd_req_t *r, const char *s)
{
    size_t n = strlen(s);
    /* A zero-length chunk is how the protocol says "response finished". Sent
     * by accident in the middle, it ends the body early and every later chunk
     * fails -- so an empty string is nothing, not a terminator. */
    return n ? httpd_resp_send_chunk(r, s, n) : ESP_OK;
}

/* Collects output and sends it in a few large pieces instead of many small
 * ones.
 *
 * The collect answer is some fifteen kilobytes across a hundred and thirty
 * fragments. Each fragment is its own send on a socket whose buffer is under
 * six kilobytes, and a send that cannot complete is an error the handler has
 * to abandon the response on. Buffering to a page at a time keeps the memory
 * bounded -- the point of streaming in the first place -- while asking far
 * less of the connection. */
typedef struct {
    httpd_req_t *req;
    char         buf[1024];
    size_t       len;
    esp_err_t    err;
} chunker_t;

static void chunker_init(chunker_t *c, httpd_req_t *r)
{
    c->req = r;
    c->len = 0;
    c->err = ESP_OK;
}

static esp_err_t chunker_flush(chunker_t *c)
{
    if (c->err == ESP_OK && c->len) {
        c->err = httpd_resp_send_chunk(c->req, c->buf, c->len);
        if (c->err != ESP_OK) {
            ESP_LOGW(TAG, "chunk of %u bytes failed: %s",
                     (unsigned)c->len, esp_err_to_name(c->err));
        }
    }
    c->len = 0;
    return c->err;
}

static esp_err_t chunker_add(chunker_t *c, const char *s)
{
    while (c->err == ESP_OK && *s) {
        size_t room = sizeof(c->buf) - c->len;
        size_t n = strlen(s);
        if (n > room) {
            n = room;
        }
        memcpy(c->buf + c->len, s, n);
        c->len += n;
        s += n;
        if (c->len == sizeof(c->buf)) {
            chunker_flush(c);
        }
    }
    return c->err;
}

static esp_err_t send_json(httpd_req_t *r, const char *json)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, json);
}

static esp_err_t send_err(httpd_req_t *r, int code, const char *msg)
{
    o3e_buf_t b;
    o3e_buf_init(&b);
    o3e_buf_adds(&b, "{\"error\": ");
    o3e_buf_add_json_str(&b, msg);
    o3e_buf_addc(&b, '}');

    char status[32];
    snprintf(status, sizeof(status), "%d %s", code,
             code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
             code == 409 ? "Conflict" : "Internal Server Error");
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, b.buf ? b.buf : "{\"error\":\"?\"}");
    o3e_buf_free(&b);
    return e;
}

/* Read the whole request body. Returns NULL and answers the client on error. */
static char *read_body(httpd_req_t *r)
{
    if (r->content_len <= 0 || r->content_len > MAX_BODY) {
        send_err(r, 400, "request body missing or too large");
        return NULL;
    }
    char *buf = malloc((size_t)r->content_len + 1);
    if (!buf) {
        send_err(r, 500, "out of memory");
        return NULL;
    }
    int got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, buf + got, (size_t)(r->content_len - got));
        if (n <= 0) {
            free(buf);
            send_err(r, 400, "could not read the request body");
            return NULL;
        }
        got += n;
    }
    buf[got] = '\0';
    return buf;
}

static bool query_u16(httpd_req_t *r, const char *key, uint16_t *out)
{
    char q[128], val[16];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(q, key, val, sizeof(val)) != ESP_OK) {
        return false;
    }
    *out = (uint16_t)strtol(val, NULL, 0);
    return true;
}

static bool query_str(httpd_req_t *r, const char *key, char *out, size_t out_sz)
{
    char q[128];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(q, key, out, out_sz) == ESP_OK;
}

/* Parse a hex string (upper- or lowercase) into bytes. False on an odd
 * length, a non-hex digit, or more bytes than `cap` holds -- `*out_len` is
 * only meaningful when this returns true. */
static bool hex_to_bytes(const char *hex, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t hexlen = strlen(hex);
    if (hexlen % 2 != 0 || hexlen / 2 > cap) {
        return false;
    }
    for (size_t i = 0; i < hexlen / 2; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo)) {
            return false;
        }
        char byte_str[3] = { hi, lo, '\0' };
        out[i] = (uint8_t)strtol(byte_str, NULL, 16);
    }
    *out_len = hexlen / 2;
    return true;
}

/* ------------------------------------------------------------------ */
/* status                                                               */

static esp_err_t h_status(httpd_req_t *r)
{
    net_status_t net;
    can_stats_t can;
    mqtt_stats_t mq;
    poll_stats_t poll;
    scan_status_t scan;
    sys_cfg_t sys;

    net_prov_status(&net);
    can_port_stats(&can);
    mqtt_pub_stats(&mq);
    int ha_sensors = 0, ha_controls = 0;
    ha_disco_counts(&ha_sensors, &ha_controls);
    grid_hold_status_t gh;
    grid_hold_status(&gh);
    storage_hold_status_t sh;
    storage_hold_status(&sh);
    poller_stats(&poll);
    e3_scan_status(&scan);
    sys_cfg_get(&sys);

    o3e_buf_t b;
    o3e_buf_init(&b);
    char t[512];

    ota_info_t ota;
    ota_info(&ota);

    /* Local wall-clock time, so a trace can be lined up against anything
     * outside this device. Empty until SNTP has answered. */
    char clock_str[32] = "";
    if (net_time_valid()) {
        time_t now = 0;
        time(&now);
        struct tm tm_local;
        localtime_r(&now, &tm_local);
        strftime(clock_str, sizeof(clock_str), "%Y-%m-%d %H:%M:%S", &tm_local);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    /* The ELF checksum is what actually identifies a build: the version string
     * only changes when someone remembers to change it, this changes whenever
     * a single byte of code does. Eight characters is plenty to compare
     * against what the local build reports. */
    char elf_sha[17] = "";
    for (int i = 0; i < 8; i++) {
        snprintf(elf_sha + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }
    snprintf(t, sizeof(t),
             "{\"firmware\": \"%s\", \"buildDate\": \"%s %s\", \"elfSha\": \"%s\", "
             "\"idfVersion\": \"%s\", "
             "\"dbVersion\": \"%s\", \"dbLoaded\": %s, \"dbCount\": %u, "
             "\"uptimeS\": %llu, "
             "\"heapFree\": %u, \"heapMin\": %u, \"writeEnabled\": %s, "
             "\"rawWriteEnabled\": %s, \"rawApiVersion\": %u, "
             "\"partition\": \"%s\", \"pendingVerify\": %s, "
             "\"clock\": \"%s\", \"clockValid\": %s, ",
             app->version, app->date, app->time, elf_sha, app->idf_ver,
             o3e_db_version(), o3e_db_is_open() ? "true" : "false",
             (unsigned)o3e_db_count(),
             (unsigned long long)(esp_timer_get_time() / 1000000),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             sys.write_enabled ? "true" : "false",
             sys.raw_write_enabled ? "true" : "false", RAW_API_VERSION,
             ota.running, ota.pending_verify ? "true" : "false",
             clock_str, net_time_valid() ? "true" : "false");
    o3e_buf_adds(&b, t);

    snprintf(t, sizeof(t),
             "\"net\": {\"state\": \"%s\", \"rssi\": %d, \"setupMode\": %s, "
             "\"ssid\": ",
             net.state == NET_STA_CONNECTED ? "connected"
               : net.state == NET_STA_CONNECTING ? "connecting"
               : net.state == NET_AP_SETUP ? "setup" : "booting",
             net.rssi, net_prov_is_setup_mode() ? "true" : "false");
    o3e_buf_adds(&b, t);
    /* An SSID is arbitrary user data and can contain quotes. */
    o3e_buf_add_json_str(&b, net.ssid);
    o3e_buf_adds(&b, ", \"ip\": ");
    o3e_buf_add_json_str(&b, net.ip);
    o3e_buf_adds(&b, ", \"apSsid\": ");
    o3e_buf_add_json_str(&b, net.ap_ssid);
    o3e_buf_adds(&b, "}, ");

    snprintf(t, sizeof(t),
             "\"can\": {\"state\": \"%s\", \"txFailed\": %u, \"rxMissed\": %u, "
             "\"busErrors\": %u, \"txErrCount\": %u, \"rxErrCount\": %u, "
             "\"recoveries\": %u, \"holder\": \"%s\", \"heldMs\": %u}, ",
             can.state ? can.state : "stopped", (unsigned)can.tx_failed,
             (unsigned)can.rx_missed, (unsigned)can.bus_errors,
             (unsigned)can.tx_err_count, (unsigned)can.rx_err_count,
             (unsigned)can.recovery_count, can.holder, (unsigned)can.held_ms);
    o3e_buf_adds(&b, t);

    snprintf(t, sizeof(t),
             "\"mqtt\": {\"connected\": %s, \"published\": %u, \"errors\": %u, "
             "\"haSensors\": %d, \"haControls\": %d}, "
             "\"grid\": {\"active\": %s, \"watts\": %d, \"remainingS\": %u, "
             "\"writes\": %u, \"failures\": %u, \"storage\": \"%s\", "
             "\"storageRemainingS\": %u}, ",
             mq.connected ? "true" : "false", (unsigned)mq.published,
             (unsigned)mq.errors, ha_sensors, ha_controls,
             gh.active ? "true" : "false", gh.watts, (unsigned)gh.remaining_s,
             (unsigned)gh.writes, (unsigned)gh.failures,
             storage_mode_name(sh.mode), (unsigned)sh.remaining_s);
    o3e_buf_adds(&b, t);

    snprintf(t, sizeof(t),
             "\"poll\": {\"points\": %u, \"polls\": %u, \"failures\": %u, "
             "\"published\": %u}, ",
             poll.active_points, (unsigned)poll.polls, (unsigned)poll.failures,
             (unsigned)poll.published);
    o3e_buf_adds(&b, t);

    snprintf(t, sizeof(t),
             "\"scan\": {\"phase\": \"%s\", \"probed\": %u, \"total\": %u, "
             "\"ecus\": %u, \"curDid\": %u, \"ecuLimitHit\": %s, "
             "\"ecuLimit\": %u, \"stalledMs\": %u, "
             "\"cobFirst\": %u, \"cobLast\": %u, \"message\": ",
             scan.phase == SCAN_ECUS ? "ecus" : scan.phase == SCAN_DIDS ? "dids"
               : scan.phase == SCAN_DONE ? "done"
               : scan.phase == SCAN_FAILED ? "failed" : "idle",
             (unsigned)scan.probed, (unsigned)scan.total, scan.n_ecus, scan.cur_did,
             scan.ecu_limit_hit ? "true" : "false", SCAN_MAX_ECUS,
             (unsigned)(scan.last_progress_ms
                        ? (uint32_t)(esp_timer_get_time() / 1000) - scan.last_progress_ms
                        : 0),
             scan.cob_first, scan.cob_last);
    o3e_buf_adds(&b, t);
    o3e_buf_add_json_str(&b, scan.message);
    o3e_buf_adds(&b, "}}");

    esp_err_t e = send_json(r, b.buf ? b.buf : "{}");
    o3e_buf_free(&b);
    return e;
}

/* Per-task CPU share and stack headroom. Polled by the status page, so the
 * shares describe the interval between two polls. */
static esp_err_t h_sysinfo(httpd_req_t *r)
{
    sysinfo_t info;
    sysinfo_task_t tasks[SYSINFO_MAX_TASKS];
    size_t n = sysinfo_read(&info, tasks, SYSINFO_MAX_TASKS);

    o3e_buf_t b;
    o3e_buf_init(&b);
    char t[256];

    snprintf(t, sizeof(t),
             "{\"cpuAvailable\": %s, \"windowMs\": %u, \"tasks\": %u, "
             "\"heap\": {\"internalFree\": %u, \"internalMin\": %u, "
             "\"internalLargest\": %u, \"psramFree\": %u, \"psramMin\": %u}, "
             "\"taskList\": [",
             info.cpu_available ? "true" : "false", (unsigned)info.window_ms,
             (unsigned)info.n_tasks, (unsigned)info.heap_int_free,
             (unsigned)info.heap_int_min, (unsigned)info.heap_int_largest,
             (unsigned)info.heap_psram_free, (unsigned)info.heap_psram_min);
    o3e_buf_adds(&b, t);

    for (size_t i = 0; i < n; i++) {
        o3e_buf_adds(&b, i ? ", {\"name\": " : "{\"name\": ");
        o3e_buf_add_json_str(&b, tasks[i].name);
        snprintf(t, sizeof(t),
                 ", \"cpu\": %u, \"stackFree\": %u, \"prio\": %u, "
                 "\"core\": %d, \"state\": \"%c\"}",
                 tasks[i].cpu_permille, (unsigned)tasks[i].stack_free,
                 tasks[i].priority, tasks[i].core, tasks[i].state);
        o3e_buf_adds(&b, t);
    }
    o3e_buf_adds(&b, "]}");

    esp_err_t e = send_json(r, b.buf ? b.buf : "{}");
    o3e_buf_free(&b);
    return e;
}

/* ------------------------------------------------------------------ */
/* Wi-Fi setup                                                          */

static esp_err_t h_wifi_scan(httpd_req_t *r)
{
    char *json = net_prov_scan_json();
    esp_err_t e = send_json(r, json ? json : "[]");
    free(json);
    return e;
}

static esp_err_t h_wifi_save(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) {
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        return send_err(r, 400, "not valid JSON");
    }

    wifi_cfg_t cfg;
    wifi_cfg_get(&cfg);
    const cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(j, "pass");
    const cJSON *host = cJSON_GetObjectItem(j, "hostname");
    const cJSON *appw = cJSON_GetObjectItem(j, "apPass");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        cJSON_Delete(j);
        return send_err(r, 400, "an SSID is required");
    }
    snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", ssid->valuestring);
    if (cJSON_IsString(pass)) {
        snprintf(cfg.pass, sizeof(cfg.pass), "%s", pass->valuestring);
    }
    if (cJSON_IsString(host) && host->valuestring[0]) {
        snprintf(cfg.hostname, sizeof(cfg.hostname), "%s", host->valuestring);
    }
    if (cJSON_IsString(appw) && strlen(appw->valuestring) >= 8) {
        snprintf(cfg.ap_pass, sizeof(cfg.ap_pass), "%s", appw->valuestring);
    }
    cJSON_Delete(j);

    if (!wifi_cfg_set(&cfg)) {
        return send_err(r, 500, "could not save the settings");
    }
    send_json(r, "{\"ok\": true, \"restarting\": true}");
    restart_soon(800);
    return ESP_OK;
}

static esp_err_t h_wifi_forget(httpd_req_t *r)
{
    wifi_cfg_clear();
    send_json(r, "{\"ok\": true, \"restarting\": true}");
    restart_soon(800);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* scan and datapoints                                                  */

static esp_err_t h_scan_start(httpd_req_t *r)
{
    char *body = read_body(r);
    scan_mode_t mode = SCAN_MODE_KNOWN;
    uint16_t first = 0, last = 0;
    if (body) {
        cJSON *j = cJSON_Parse(body);
        if (j) {
            const char *m = sel_str(j, "mode");
            if (m && strcmp(m, "full") == 0) {
                mode = SCAN_MODE_FULL;
            }
            first = sel_u16(j, "cobFirst", 0);
            last = sel_u16(j, "cobLast", 0);
        }
        cJSON_Delete(j);
        free(body);
    }
    if (!e3_scan_start(mode, first, last)) {
        return send_err(r, 409, "a scan is already running");
    }
    return send_json(r, "{\"ok\": true}");
}

static esp_err_t h_scan_abort(httpd_req_t *r)
{
    e3_scan_abort();
    return send_json(r, "{\"ok\": true}");
}

static esp_err_t h_system(httpd_req_t *r)
{
    char *json = e3_scan_result_json();
    esp_err_t e = send_json(r, json ? json : "{\"devices\": []}");
    free(json);
    return e;
}

static esp_err_t h_devices_put(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) {
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(j)) {
        cJSON_Delete(j);
        return send_err(r, 400, "expected a JSON array of {addr, name}");
    }

    int renamed = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, j) {
        const cJSON *addr = cJSON_GetObjectItem(it, "addr");
        const cJSON *name = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsNumber(addr) && cJSON_IsString(name) &&
            e3_scan_rename((uint16_t)addr->valuedouble, name->valuestring)) {
            renamed++;
        }
    }
    cJSON_Delete(j);

    /* Names feed the {device} placeholder, so the topics have to be rebuilt. */
    if (renamed) {
        poller_reload();
    }
    return send_json(r, "{\"ok\": true}");
}

/* Codec description for one DID, streamed straight from the database so the UI
 * can show field names, units, descriptions and the access flag. */
static esp_err_t h_datapoint(httpd_req_t *r)
{
    uint16_t did = 0, len = 0;
    if (!query_u16(r, "did", &did)) {
        return send_err(r, 400, "did parameter is required");
    }
    query_u16(r, "len", &len);

    char *json = o3e_db_json(did, len);
    if (!json) {
        return send_err(r, 404, "unknown DID");
    }
    esp_err_t e = send_json(r, json);
    free(json);
    return e;
}

/* The energy meter frames the database knows about, with whatever the meter
 * last broadcast for each. Frames never seen come back without a value, which
 * is how the UI tells "no meter on this bus" from "meter present". */
/* Bus diagnosis: a loopback self-test followed by a listen-only sweep across
 * the common bit rates. Together they say whether a silent bus is the
 * controller, the wiring, or the bit rate. */
static esp_err_t h_candiag(httpd_req_t *r)
{
    /* The sweep takes the bus for several seconds; polling has to stand
     * aside the same way it does for a scan. */
    bool was_paused = poller_is_paused();
    poller_pause(true);

    char err[192] = "";
    bool loop_ok = can_diag_loopback(err, sizeof(err));

    static const uint32_t rates[] = { 250000, 125000, 500000, 1000000 };
    can_diag_t res[sizeof(rates) / sizeof(rates[0])];
    memset(res, 0, sizeof(res));
    char lerr[192] = "";
    bool listened = can_diag_listen(rates, sizeof(rates) / sizeof(rates[0]),
                                    2000, res, lerr, sizeof(lerr));

    if (!was_paused) {
        poller_pause(false);
    }

    o3e_buf_t b;
    o3e_buf_init(&b);
    char t[160];
    snprintf(t, sizeof(t), "{\"loopback\": %s, \"loopbackError\": ",
             loop_ok ? "true" : "false");
    o3e_buf_adds(&b, t);
    o3e_buf_add_json_str(&b, loop_ok ? "" : err);
    snprintf(t, sizeof(t), ", \"listenOk\": %s, \"listenError\": ",
             listened ? "true" : "false");
    o3e_buf_adds(&b, t);
    o3e_buf_add_json_str(&b, listened ? "" : lerr);
    o3e_buf_adds(&b, ", \"rates\": [");

    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        snprintf(t, sizeof(t), "%s{\"bitrate\": %u, \"frames\": %u, \"ids\": [",
                 i ? ", " : "", (unsigned)res[i].bitrate, (unsigned)res[i].frames);
        o3e_buf_adds(&b, t);
        for (uint8_t k = 0; k < res[i].n_ids; k++) {
            snprintf(t, sizeof(t), "%s\"0x%03X\"", k ? ", " : "",
                     (unsigned)res[i].ids[k]);
            o3e_buf_adds(&b, t);
        }
        o3e_buf_adds(&b, "]}");
    }
    o3e_buf_adds(&b, "]}");

    esp_err_t e = send_json(r, b.buf ? b.buf : "{}");
    o3e_buf_free(&b);
    return e;
}

static esp_err_t h_em380(httpd_req_t *r)
{
    em380_stats_t st;
    em380_stats(&st);

    o3e_buf_t b;
    o3e_buf_init(&b);
    char t[192];

    snprintf(t, sizeof(t),
             "{\"enabled\": %s, \"seen\": %s, \"frames\": %u, \"published\": %u, "
             "\"canFirst\": %u, \"canLast\": %u, \"frameList\": [",
             st.enabled ? "true" : "false", st.seen ? "true" : "false",
             (unsigned)st.frames, (unsigned)st.published,
             EM380_CAN_FIRST, EM380_CAN_LAST);
    o3e_buf_adds(&b, t);

    for (size_t i = 0; i < o3e_db_em_count(); i++) {
        const o3e_em_entry_t *e = o3e_db_em_at(i);
        if (!e) {
            continue;
        }
        char *desc = o3e_db_em_json(e->can_id);
        o3e_node_t *node = desc ? o3e_codec_compile(desc) : NULL;
        free(desc);

        snprintf(t, sizeof(t), "%s{\"canId\": %u, \"canIdHex\": \"0x%03X\", "
                 "\"len\": %u, \"address\": %u, \"name\": ",
                 i ? ", " : "", e->can_id, e->can_id, e->dlen,
                 /* Even IDs belong to the meter at CAN address 97, odd to 98. */
                 (e->can_id % 2) ? 98 : 97);
        o3e_buf_adds(&b, t);
        o3e_buf_add_json_str(&b, node && node->id ? node->id : "");
        o3e_codec_free(node);

        char *val = em380_last_json(e->can_id);
        if (val) {
            o3e_buf_adds(&b, ", \"value\": ");
            o3e_buf_adds(&b, val);
            free(val);
        }
        o3e_buf_addc(&b, '}');
    }
    o3e_buf_adds(&b, "]}");

    esp_err_t e = send_json(r, b.buf ? b.buf : "{}");
    o3e_buf_free(&b);
    return e;
}

/* did -> name and German label, for every datapoint the database knows.
 *
 * The scan result stores only numbers; names are resolved here, once, instead
 * of being repeated for every datapoint of every ECU. Streamed straight from
 * the resident index, so it costs no heap. */
static esp_err_t h_names(httpd_req_t *r)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    send_chunk_str(r, "{\"n\":{");

    char part[192];
    for (size_t i = 0; i < o3e_db_count(); i++) {
        const o3e_dp_entry_t *e = o3e_db_at(i);
        const char *name = o3e_db_name(e);
        if (!e || !name) {
            continue;
        }
        int n = snprintf(part, sizeof(part), "%s\"%u\":\"%s\"",
                         i ? "," : "", e->did, name);
        if (n > 0 && httpd_resp_send_chunk(r, part, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    send_chunk_str(r, "},\"de\":{");
    bool first = true;
    for (size_t i = 0; i < o3e_db_count(); i++) {
        const o3e_dp_entry_t *e = o3e_db_at(i);
        const char *de = o3e_db_name_de(i);
        if (!e || !de || !de[0]) {
            continue;
        }
        int n = snprintf(part, sizeof(part), "%s\"%u\":\"%s\"",
                         first ? "" : ",", e->did, de);
        first = false;
        if (n > 0 && httpd_resp_send_chunk(r, part, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    send_chunk_str(r, "}}");
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* What the broadcast channel has announced, newest activity first. Each entry
 * is an ordinary open3e datapoint, so it is named and decoded like any other. */
static esp_err_t h_collect(httpd_req_t *r)
{
    collect_stats_t st;
    collect_stats(&st);

    /* Static rather than a fresh 1.5 KiB of internal RAM on every poll of the
     * page: the HTTP server runs one worker, so there is no second caller, and
     * internal RAM is the scarce kind here. A failed allocation used to turn
     * into a 500 that the page rendered as a dash. */
    static collect_entry_t e[COLLECT_MAX_DIDS];
    size_t n = collect_entries(e, COLLECT_MAX_DIDS);

    /* Streamed, not assembled.
     *
     * On a Vitocharge bus this answer is around fifteen kilobytes: 43
     * datapoints, one of them decoding to a value of 1.5 KiB on its own. A
     * doubling buffer reaches that through 1, 2, 4, 8, 16 KiB, and the last
     * step needs the old and the new alive at once -- some 24 KiB, all of it
     * internal RAM, because allocations below 16 KiB never go to PSRAM here.
     * The request simply died. Sent in pieces there is no large buffer at all,
     * and the peak is the longest single value. */
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");

    char t[192];
    snprintf(t, sizeof(t),
             "{\"enabled\": %s, \"messages\": %u, \"incomplete\": %u, "
             "\"published\": %u, \"canIds\": [",
             st.enabled ? "true" : "false", (unsigned)st.messages,
             (unsigned)st.incomplete, (unsigned)st.published);
    send_chunk_str(r, t);
    for (uint8_t k = 0; k < st.n_ids; k++) {
        snprintf(t, sizeof(t), "%s\"0x%03X\"", k ? "," : "", st.can_ids[k]);
        send_chunk_str(r, t);
    }
    send_chunk_str(r, "], \"dids\": [");

    esp_err_t rc = ESP_OK;
    for (size_t i = 0; i < n && rc == ESP_OK; i++) {
        /* Name and value were produced when the message arrived; this handler
         * only formats them. */
        snprintf(t, sizeof(t), "%s{\"did\": %u, \"len\": %u, \"count\": %u, \"name\": \"%s\"",
                 i ? ", " : "", e[i].did, e[i].len, (unsigned)e[i].count,
                 e[i].name ? e[i].name : "");
        rc = send_chunk_str(r, t);
        if (rc == ESP_OK && e[i].json) {
            rc = send_chunk_str(r, ", \"value\": ");
            if (rc == ESP_OK) {
                rc = send_chunk_str(r, e[i].json);
            }
        }
        if (rc == ESP_OK) {
            rc = send_chunk_str(r, "}");
        }
    }
    collect_entries_free(e, n);
    if (rc != ESP_OK) {
        return ESP_FAIL;
    }
    send_chunk_str(r, "]}");
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* Why the device last restarted, and where it crashed if it did. */
static esp_err_t h_crash(httpd_req_t *r)
{
    if (r->method == HTTP_DELETE) {
        return crashlog_clear() ? send_json(r, "{\"ok\": true}")
                                : send_err(r, 500, "cannot erase the core dump");
    }
    char out[768];
    crashlog_json(out, sizeof(out));
    return send_json(r, out);
}

static esp_err_t h_points_get(httpd_req_t *r)
{
    char *json = app_config_read_file(CFG_POINTS_PATH);
    esp_err_t e = send_json(r, json ? json : "[]");
    free(json);
    return e;
}

static esp_err_t h_points_put(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) {
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    if (!cJSON_IsArray(j)) {
        cJSON_Delete(j);
        free(body);
        return send_err(r, 400, "expected a JSON array of datapoints");
    }
    cJSON_Delete(j);

    /* Clear the old discovery topics before the selection changes, so entities
     * that were just removed do not linger in Home Assistant. */
    ha_disco_clear_all();

    bool ok = app_config_write_file(CFG_POINTS_PATH, body, strlen(body));
    free(body);
    if (!ok) {
        return send_err(r, 500, "could not save the selection");
    }
    poller_reload();
    return send_json(r, "{\"ok\": true}");
}

/* ------------------------------------------------------------------ */
/* manual read / write                                                  */

static esp_err_t h_read(httpd_req_t *r)
{
    uint16_t ecu = 0, did = 0;
    if (!query_u16(r, "ecu", &ecu) || !query_u16(r, "did", &did)) {
        return send_err(r, 400, "ecu and did parameters are required");
    }
    char err[160] = "";
    char *json = poller_read_now(ecu, did, err, sizeof(err));
    if (!json) {
        return send_err(r, 400, err);
    }
    o3e_buf_t b;
    o3e_buf_init(&b);
    o3e_buf_adds(&b, "{\"value\": ");
    o3e_buf_adds(&b, json);
    o3e_buf_addc(&b, '}');
    free(json);
    esp_err_t e = send_json(r, b.buf ? b.buf : "{}");
    o3e_buf_free(&b);
    return e;
}

/* Hold the grid setpoint. The caps live in grid_hold.c, not here: a limit
 * that only the web interface enforces is not a limit. */
static esp_err_t h_grid(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) {
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        return send_err(r, 400, "not valid JSON");
    }
    if (sel_bool(j, "stop", false)) {
        cJSON_Delete(j);
        grid_hold_stop();
        return send_json(r, "{\"ok\": true}");
    }
    /* The storage's own limits ride on the same endpoint: same target, same
       deadline, same way of ending. */
    const cJSON *jsm = cJSON_GetObjectItem(j, "storage");
    if (cJSON_IsString(jsm)) {
        storage_mode_t mode;
        if (!storage_mode_parse(jsm->valuestring, &mode)) {
            cJSON_Delete(j);
            return send_err(r, 400, "unknown storage mode");
        }
        uint16_t secu = sel_u16(j, "ecu", 0);
        uint32_t ssec = sel_u32(j, "seconds", 0);
        cJSON_Delete(j);
        char serr[128] = "";
        if (!storage_hold_start(secu, mode, ssec ? ssec : 900, serr, sizeof(serr))) {
            return send_err(r, 400, serr[0] ? serr : "cannot set the storage mode");
        }
        return send_json(r, "{\"ok\": true}");
    }

    const cJSON *jw = cJSON_GetObjectItem(j, "watts");
    if (!cJSON_IsNumber(jw)) {
        cJSON_Delete(j);
        return send_err(r, 400, "watts is required");
    }
    uint16_t ecu = sel_u16(j, "ecu", 0);
    int16_t watts = (int16_t)jw->valuedouble;
    uint32_t seconds = sel_u32(j, "seconds", 0);
    cJSON_Delete(j);
    if (!ecu) {
        return send_err(r, 400, "ecu is required");
    }

    char err[128] = "";
    if (!grid_hold_start(ecu, watts, seconds, err, sizeof(err))) {
        return send_err(r, 400, err[0] ? err : "cannot start the hold");
    }
    /* Remember what was asked for, so the Home Assistant switch and this form
     * mean the same thing. Stored positive: the sign belongs to the datapoint,
     * not to the operator. */
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    sys.grid_ecu = ecu;
    sys.grid_watts = (uint16_t)(watts < 0 ? -watts : watts);
    sys.grid_minutes = (uint16_t)((seconds + 59) / 60);
    sys_cfg_set(&sys);
    hold_publish();
    return send_json(r, "{\"ok\": true}");
}

static esp_err_t h_write(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) {
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        return send_err(r, 400, "not valid JSON");
    }
    const cJSON *jecu = cJSON_GetObjectItem(j, "ecu");
    const cJSON *jdid = cJSON_GetObjectItem(j, "did");
    const cJSON *jval = cJSON_GetObjectItem(j, "value");
    if (!cJSON_IsNumber(jecu) || !cJSON_IsNumber(jdid) || !jval) {
        cJSON_Delete(j);
        return send_err(r, 400, "ecu, did and value are required");
    }
    char *value = cJSON_PrintUnformatted(jval);
    uint16_t ecu = (uint16_t)jecu->valuedouble;
    uint16_t did = (uint16_t)jdid->valuedouble;
    /* Read before the tree goes: everything below outlives it. Only the web
       interface may force, and only when the request asks -- a person ticking
       a labelled box is a decision, an automation sending the same JSON every
       minute is not. */
    bool force = sel_bool(j, "force", false);
    cJSON_Delete(j);

    char err[192] = "";
    bool ok = value && poller_write_now(ecu, did, value, force, err, sizeof(err));
    free(value);
    if (!ok) {
        return send_err(r, 400, err[0] ? err : "write failed");
    }
    return send_json(r, "{\"ok\": true}");
}

/* ------------------------------------------------------------------ */
/* raw read / write                                                     */
/*
 * The counterparts to h_read()/h_write() above that skip the open3e codec
 * entirely -- see docs/raw-gateway-api.md. Deliberately calling can_read_did()
 * /can_write_did() directly rather than going through poller_read_now()/
 * poller_write_now(): those also enforce "DID known to our database" and
 * "marked rw in our database", which is exactly the coupling a caller with
 * its own datapoint definitions (e.g. ioBroker.e3oncan) needs to not have.
 */

#define RAWREAD_MAX_BATCH 10

static esp_err_t h_rawread(httpd_req_t *r)
{
    char ecu_str[16], did_str[128];
    if (!query_str(r, "ecu", ecu_str, sizeof(ecu_str)) ||
        !query_str(r, "did", did_str, sizeof(did_str))) {
        return send_err(r, 400, "ecu and did parameters are required");
    }
    uint16_t ecu = (uint16_t)strtol(ecu_str, NULL, 0);

    uint8_t *buf = malloc(ISOTP_MAX_PAYLOAD);
    if (!buf) {
        return send_err(r, 500, "out of memory");
    }

    o3e_buf_t b;
    o3e_buf_init(&b);
    char t[32];
    snprintf(t, sizeof(t), "0x%03x", ecu);
    o3e_buf_adds(&b, "{\"ecu\": ");
    o3e_buf_add_json_str(&b, t);
    o3e_buf_adds(&b, ", \"results\": [");

    int count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(did_str, ",", &save); tok && count < RAWREAD_MAX_BATCH;
         tok = strtok_r(NULL, ",", &save), count++) {
        uint16_t did = (uint16_t)strtol(tok, NULL, 0);
        size_t n = 0;
        uds_result_t res = can_read_did(ecu, did, buf, ISOTP_MAX_PAYLOAD, &n, UDS_P2_MS);

        snprintf(t, sizeof(t), "%u", did);
        o3e_buf_adds(&b, count ? ", {\"did\": " : "{\"did\": ");
        o3e_buf_adds(&b, t);
        if (res.err == UDS_OK) {
            o3e_buf_adds(&b, ", \"data\": \"");
            for (size_t i = 0; i < n; i++) {
                char hx[3];
                snprintf(hx, sizeof(hx), "%02x", buf[i]);
                o3e_buf_adds(&b, hx);
            }
            snprintf(t, sizeof(t), "\", \"len\": %u}", (unsigned)n);
            o3e_buf_adds(&b, t);
        } else {
            o3e_buf_adds(&b, ", \"error\": ");
            o3e_buf_add_json_str(&b, uds_strerror(res));
            o3e_buf_addc(&b, '}');
        }
    }
    free(buf);
    o3e_buf_adds(&b, "]}");

    esp_err_t e = send_json(r, b.buf ? b.buf : "{}");
    o3e_buf_free(&b);
    return e;
}

static esp_err_t h_rawwrite(httpd_req_t *r)
{
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    if (!sys.raw_write_enabled) {
        return send_err(r, 403, "raw writing is disabled in the system settings");
    }

    char *body = read_body(r);
    if (!body) {
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        return send_err(r, 400, "not valid JSON");
    }
    const cJSON *jecu = cJSON_GetObjectItem(j, "ecu");
    const cJSON *jdid = cJSON_GetObjectItem(j, "did");
    const cJSON *jsvc = cJSON_GetObjectItem(j, "svc");
    const cJSON *jdata = cJSON_GetObjectItem(j, "data");
    if ((!cJSON_IsNumber(jecu) && !cJSON_IsString(jecu)) || !cJSON_IsNumber(jdid) ||
        !cJSON_IsString(jsvc) || !cJSON_IsString(jdata)) {
        cJSON_Delete(j);
        return send_err(r, 400, "ecu, did, svc and data are required");
    }
    /* Service 0x77 is deliberately not implemented -- see uds.h. */
    if (strcasecmp(jsvc->valuestring, "0x2E") != 0) {
        cJSON_Delete(j);
        return send_err(r, 400, "svc must be \"0x2E\" (0x77 is not supported yet)");
    }
    uint16_t ecu = cJSON_IsNumber(jecu) ? (uint16_t)jecu->valuedouble
                                        : (uint16_t)strtol(jecu->valuestring, NULL, 0);
    uint16_t did = (uint16_t)jdid->valuedouble;

    uint8_t payload[UDS_MAX_WRITE];
    size_t len = 0;
    bool parsed = hex_to_bytes(jdata->valuestring, payload, sizeof(payload), &len);
    cJSON_Delete(j);
    if (!parsed) {
        return send_err(r, 400, "data must be an even-length hex string");
    }

    uds_result_t res = can_write_did(ecu, did, payload, len, UDS_P2_MS);
    if (res.err != UDS_OK) {
        return send_err(r, 400, uds_strerror(res));
    }
    ESP_LOGI(TAG, "raw wrote 0x%03X.%u (%u bytes)", ecu, did, (unsigned)len);
    return send_json(r, "{\"ok\": true}");
}

/* ------------------------------------------------------------------ */
/* CAN trace                                                             */

static esp_err_t h_trace_ctl(httpd_req_t *r)
{
    char *body = read_body(r);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);

    const char *action = j ? sel_str(j, "action") : NULL;
    uint32_t frames = j ? sel_u32(j, "frames", CANTRACE_DEFAULT_FRAMES) : CANTRACE_DEFAULT_FRAMES;
    uint16_t lo = j ? sel_u16(j, "idLow", 0) : 0;
    uint16_t hi = j ? sel_u16(j, "idHigh", 0x7FF) : 0x7FF;
    bool start = action && strcmp(action, "start") == 0;
    const char *trig = j ? sel_str(j, "trigger") : NULL;
    cantrace_trigger_t tmode = CANTRACE_TRIG_NONE;
    if (trig && strcmp(trig, "write") == 0) {
        tmode = CANTRACE_TRIG_WRITE;
    } else if (trig && strcmp(trig, "novel") == 0) {
        tmode = CANTRACE_TRIG_NOVEL;
    } else if (trig && strcmp(trig, "control") == 0) {
        tmode = CANTRACE_TRIG_CONTROL;
    }
    uint32_t post = j ? sel_u32(j, "postFrames", 0) : 0;
    uint32_t learn = j ? sel_u32(j, "learnS", 300) : 300;
    bool skip_own = j ? sel_bool(j, "excludeOwn", true) : true;
    cJSON_Delete(j);

    if (start) {
        if (!cantrace_start(frames, lo, hi, tmode, post, learn, skip_own)) {
            return send_err(r, 500, "not enough memory for the requested ring size");
        }
    } else {
        cantrace_stop();
    }
    return send_json(r, "{\"ok\": true}");
}

static esp_err_t h_trace_status(httpd_req_t *r)
{
    cantrace_stats_t st;
    cantrace_stats(&st);

    char t[512];
    int o = snprintf(t, sizeof(t),
             "{\"running\": %s, \"captured\": %u, \"stored\": %u, "
             "\"capacity\": %u, \"dropped\": %u, \"elapsedMs\": %u, "
             "\"idLow\": %u, \"idHigh\": %u, \"trigger\": \"%s\", "
             "\"triggered\": %s, \"postRemaining\": %u",
             st.running ? "true" : "false", (unsigned)st.captured,
             (unsigned)st.stored, (unsigned)st.capacity, (unsigned)st.dropped,
             (unsigned)st.elapsed_ms, st.filter_lo, st.filter_hi,
             st.trigger == CANTRACE_TRIG_WRITE ? "write"
             : st.trigger == CANTRACE_TRIG_NOVEL ? "novel"
             : st.trigger == CANTRACE_TRIG_CONTROL ? "control" : "none",
             st.triggered ? "true" : "false", (unsigned)st.post_remaining);
    uint32_t learn_left = 0;
    bool learning = cantrace_learning(&learn_left);
    o += snprintf(t + o, sizeof(t) - o,
                  ", \"learning\": %s, \"learnLeftS\": %u, "
                  "\"excludeOwn\": %s, \"skippedOwn\": %u",
                  learning ? "true" : "false", (unsigned)learn_left,
                  st.exclude_own ? "true" : "false", (unsigned)st.skipped_own);
    if (st.event.fired) {
        o += snprintf(t + o, sizeof(t) - o,
                      ", \"event\": {\"kind\": \"%s\", \"us\": %u, \"canId\": %u, "
                      "\"did\": %u, \"byte\": %u, \"was\": %u, \"now\": %u, \"data\": \"",
                      st.event.kind == CANTRACE_EV_NEW_ID ? "newId"
                      : st.event.kind == CANTRACE_EV_BYTE_CHANGE ? "byteChange" : "write",
                      (unsigned)st.event.us, st.event.can_id, st.event.did,
                      st.event.byte_index, st.event.was, st.event.now);
        for (uint8_t i = 0; i < st.event.dlc && o < (int)sizeof(t) - 4; i++) {
            o += snprintf(t + o, sizeof(t) - o, "%02X", st.event.data[i]);
        }
        o += snprintf(t + o, sizeof(t) - o, "\"}");
    }
    snprintf(t + o, sizeof(t) - o, "}");
    return send_json(r, t);
}

/* Frames as JSON, for the live view. `from` continues a previous read. */
static esp_err_t h_trace_frames(httpd_req_t *r)
{
    uint16_t from = 0, want = 200;
    query_u16(r, "from", &from);
    query_u16(r, "count", &want);
    if (want > 500) {
        want = 500;
    }

    cantrace_frame_t *buf = malloc(sizeof(*buf) * want);
    if (!buf) {
        return send_err(r, 500, "out of memory");
    }
    size_t n = cantrace_read(from, buf, want);

    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    send_chunk_str(r, "[");
    for (size_t i = 0; i < n; i++) {
        char t[96];
        int o = snprintf(t, sizeof(t), "%s[%u,%u,%u,\"", i ? "," : "",
                         (unsigned)buf[i].us, buf[i].id,
                         (buf[i].flags & CANTRACE_FLAG_TX) ? 1 : 0);
        for (uint8_t k = 0; k < buf[i].dlc; k++) {
            o += snprintf(t + o, sizeof(t) - o, "%02X", buf[i].data[k]);
        }
        o += snprintf(t + o, sizeof(t) - o, "\"]");
        httpd_resp_send_chunk(r, t, o);
    }
    send_chunk_str(r, "]");
    free(buf);
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* The per-identifier map: which identifiers this bus carries, how busy each
 * is, and which payload bytes actually vary. Worth reading before hunting for
 * anything specific. */
static esp_err_t h_trace_ids(httpd_req_t *r)
{
    cantrace_id_stat_t *ids = malloc(sizeof(*ids) * CANTRACE_MAX_IDS);
    if (!ids) {
        return send_err(r, 500, "out of memory");
    }
    size_t n = cantrace_ids(ids, CANTRACE_MAX_IDS);

    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    send_chunk_str(r, "[");
    for (size_t i = 0; i < n; i++) {
        char t[200];
        int o = snprintf(t, sizeof(t),
                         "%s{\"id\": %u, \"count\": %u, \"varying\": %u, "
                         "\"firstUs\": %u, \"lastUs\": %u, \"last\": \"",
                         i ? "," : "", ids[i].id, (unsigned)ids[i].count,
                         ids[i].varying, (unsigned)ids[i].first_us,
                         (unsigned)ids[i].last_us);
        for (uint8_t k = 0; k < ids[i].dlc; k++) {
            o += snprintf(t + o, sizeof(t) - o, "%02X", ids[i].last[k]);
        }
        o += snprintf(t + o, sizeof(t) - o, "\"}");
        httpd_resp_send_chunk(r, t, o);
    }
    send_chunk_str(r, "]");
    free(ids);
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* candump log format, as `candump -l` writes it and canplayer, open3e's
 * candump2msgbus.py and Wireshark all read:
 *   (1699999999.123456) can0 680#2201000000000000
 * Downloading rather than viewing means the analysis can continue in whatever
 * tool is already set up for it. */
static esp_err_t h_trace_dump(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Content-Disposition",
                       "attachment; filename=\"open3e-can.log\"");

    cantrace_frame_t chunk[64];
    size_t from = 0, n;
    while ((n = cantrace_read(from, chunk, 64)) > 0) {
        for (size_t i = 0; i < n; i++) {
            char line[96];
            int o = snprintf(line, sizeof(line), "(%u.%06u) can0 %03X#",
                             (unsigned)(chunk[i].us / 1000000),
                             (unsigned)(chunk[i].us % 1000000), chunk[i].id);
            for (uint8_t k = 0; k < chunk[i].dlc; k++) {
                o += snprintf(line + o, sizeof(line) - o, "%02X", chunk[i].data[k]);
            }
            o += snprintf(line + o, sizeof(line) - o, "\n");
            if (httpd_resp_send_chunk(r, line, o) != ESP_OK) {
                return ESP_FAIL;
            }
        }
        from += n;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Backup and restore                                                    */

/* Stream a file's contents into the response, or `dflt` when it is missing. */
static void copy_str(const cJSON *o, const char *key, char *dst, size_t dst_sz);

static bool stream_file_inline(httpd_req_t *r, const char *path, const char *dflt)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_chunk(r, dflt, strlen(dflt)) == ESP_OK;
    }
    char chunk[512];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        ok = httpd_resp_send_chunk(r, chunk, n) == ESP_OK;
    }
    fclose(f);
    return ok;
}

/* Everything needed to put a replacement device into the same state.
 *
 * Credentials are included: a backup that cannot restore the Wi-Fi password
 * leaves the device unreachable after a restore, which defeats the point. The
 * UI says so plainly next to the download.
 *
 * The scan result is optional because it is the bulky part -- hundreds of
 * kilobytes -- but also the expensive one to recreate, so it defaults to
 * included. Both files are streamed rather than assembled in memory. */
static esp_err_t h_export(httpd_req_t *r)
{
    uint16_t with_scan = 1;
    query_u16(r, "scan", &with_scan);

    wifi_cfg_t wifi;
    mqtt_cfg_t mq;
    sys_cfg_t sys;
    wifi_cfg_get(&wifi);
    mqtt_cfg_get(&mq);
    sys_cfg_get(&sys);

    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Content-Disposition",
                       "attachment; filename=\"open3e-settings.json\"");

    o3e_buf_t b;
    o3e_buf_init(&b);
    char t[128];

    const esp_app_desc_t *app = esp_app_get_description();
    o3e_buf_adds(&b, "{\"format\": 1, \"firmware\": ");
    o3e_buf_add_json_str(&b, app->version);
    o3e_buf_adds(&b, ", \"dbVersion\": ");
    o3e_buf_add_json_str(&b, o3e_db_version());

    o3e_buf_adds(&b, ", \"wifi\": {\"ssid\": ");
    o3e_buf_add_json_str(&b, wifi.ssid);
    o3e_buf_adds(&b, ", \"pass\": ");
    o3e_buf_add_json_str(&b, wifi.pass);
    o3e_buf_adds(&b, ", \"hostname\": ");
    o3e_buf_add_json_str(&b, wifi.hostname);
    o3e_buf_adds(&b, ", \"apPass\": ");
    o3e_buf_add_json_str(&b, wifi.ap_pass);

    o3e_buf_adds(&b, "}, \"mqtt\": {\"enabled\": ");
    o3e_buf_adds(&b, mq.enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"host\": ");
    o3e_buf_add_json_str(&b, mq.host);
    snprintf(t, sizeof(t), ", \"port\": %u, \"user\": ", mq.port);
    o3e_buf_adds(&b, t);
    o3e_buf_add_json_str(&b, mq.user);
    o3e_buf_adds(&b, ", \"pass\": ");
    o3e_buf_add_json_str(&b, mq.pass);
    o3e_buf_adds(&b, ", \"baseTopic\": ");
    o3e_buf_add_json_str(&b, mq.base_topic);
    o3e_buf_adds(&b, ", \"format\": ");
    o3e_buf_add_json_str(&b, mq.format);
    o3e_buf_adds(&b, ", \"cmndTopic\": ");
    o3e_buf_add_json_str(&b, mq.cmnd_topic);
    o3e_buf_adds(&b, ", \"haDiscovery\": ");
    o3e_buf_adds(&b, mq.ha_discovery ? "true" : "false");
    o3e_buf_adds(&b, ", \"haPrefix\": ");
    o3e_buf_add_json_str(&b, mq.ha_prefix);

    o3e_buf_adds(&b, "}, \"system\": {\"writeEnabled\": ");
    o3e_buf_adds(&b, sys.write_enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"rawWriteEnabled\": ");
    o3e_buf_adds(&b, sys.raw_write_enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"em380Enabled\": ");
    o3e_buf_adds(&b, sys.em380_enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"collectEnabled\": ");
    o3e_buf_adds(&b, sys.collect_enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"collectCanIds\": ");
    o3e_buf_add_json_str(&b, sys.collect_canids);
    o3e_buf_adds(&b, ", \"rawCanIds\": ");
    o3e_buf_add_json_str(&b, sys.raw_canids);
    o3e_buf_adds(&b, ", \"tz\": ");
    o3e_buf_add_json_str(&b, sys.tz);
    o3e_buf_adds(&b, "}, \"points\": ");

    if (b.oom || !b.buf ||
        httpd_resp_send_chunk(r, b.buf, b.len) != ESP_OK) {
        o3e_buf_free(&b);
        return ESP_FAIL;
    }
    o3e_buf_free(&b);

    if (!stream_file_inline(r, CFG_POINTS_PATH, "[]")) {
        return ESP_FAIL;
    }
    if (with_scan) {
        send_chunk_str(r, ", \"scan\": ");
        if (!stream_file_inline(r, CFG_SYSTEM_PATH, "null")) {
            return ESP_FAIL;
        }
    }
    send_chunk_str(r, "}");
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* Restore. The body can be several hundred kilobytes, so it goes to a file
 * first and is parsed from there rather than held twice in memory. */
#define IMPORT_MAX (1024 * 1024)
#define IMPORT_TMP CFG_MOUNT "/import.json"

static bool import_apply(const cJSON *root, char *err, size_t err_sz);

static esp_err_t h_import(httpd_req_t *r)
{
    if (r->content_len <= 0 || r->content_len > IMPORT_MAX) {
        return send_err(r, 400, "backup file missing or too large");
    }

    FILE *f = fopen(IMPORT_TMP, "wb");
    if (!f) {
        return send_err(r, 500, "cannot buffer the upload");
    }
    char chunk[1024];
    int received = 0;
    while (received < r->content_len) {
        int n = httpd_req_recv(r, chunk, sizeof(chunk));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0 || fwrite(chunk, 1, (size_t)n, f) != (size_t)n) {
            fclose(f);
            remove(IMPORT_TMP);
            return send_err(r, 400, "the upload was interrupted");
        }
        received += n;
    }
    fclose(f);

    char *raw = app_config_read_file(IMPORT_TMP);
    remove(IMPORT_TMP);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!root) {
        ESP_LOGE(TAG, "could not parse the backup (%u bytes); "
                 "heap: %u KiB internal, %u KiB PSRAM",
                 (unsigned)received,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        return send_err(r, 400,
                        "the backup could not be read - not valid JSON, or too "
                        "little memory to parse it");
    }

    char err[192] = "";
    bool ok = import_apply(root, err, sizeof(err));
    cJSON_Delete(root);
    if (!ok) {
        return send_err(r, 400, err);
    }

    send_json(r, "{\"ok\": true, \"restarting\": true}");

    restart_soon(800);
    return ESP_OK;
}

static bool write_section(const cJSON *node, const char *path)
{
    char *text = cJSON_PrintUnformatted(node);
    if (!text) {
        return false;
    }
    bool ok = app_config_write_file(path, text, strlen(text));
    free(text);
    return ok;
}

static bool import_apply(const cJSON *root, char *err, size_t err_sz)
{
    const cJSON *fmt = cJSON_GetObjectItem(root, "format");
    if (!cJSON_IsNumber(fmt) || (int)fmt->valuedouble != 1) {
        snprintf(err, err_sz, "unknown backup format");
        return false;
    }

    const cJSON *jw = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(jw)) {
        wifi_cfg_t w;
        wifi_cfg_get(&w);
        copy_str(jw, "ssid", w.ssid, sizeof(w.ssid));
        copy_str(jw, "pass", w.pass, sizeof(w.pass));
        copy_str(jw, "hostname", w.hostname, sizeof(w.hostname));
        copy_str(jw, "apPass", w.ap_pass, sizeof(w.ap_pass));
        wifi_cfg_set(&w);
    }

    const cJSON *jm = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(jm)) {
        mqtt_cfg_t m;
        mqtt_cfg_get(&m);
        const cJSON *v;
        if (cJSON_IsBool(v = cJSON_GetObjectItem(jm, "enabled"))) {
            m.enabled = cJSON_IsTrue(v);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(jm, "port"))) {
            m.port = (uint16_t)v->valuedouble;
        }
        if (cJSON_IsBool(v = cJSON_GetObjectItem(jm, "haDiscovery"))) {
            m.ha_discovery = cJSON_IsTrue(v);
        }
        copy_str(jm, "host", m.host, sizeof(m.host));
        copy_str(jm, "user", m.user, sizeof(m.user));
        copy_str(jm, "pass", m.pass, sizeof(m.pass));
        copy_str(jm, "baseTopic", m.base_topic, sizeof(m.base_topic));
        copy_str(jm, "format", m.format, sizeof(m.format));
        copy_str(jm, "cmndTopic", m.cmnd_topic, sizeof(m.cmnd_topic));
        copy_str(jm, "haPrefix", m.ha_prefix, sizeof(m.ha_prefix));
        mqtt_cfg_set(&m);
    }

    const cJSON *js = cJSON_GetObjectItem(root, "system");
    if (cJSON_IsObject(js)) {
        sys_cfg_t sc;
        sys_cfg_get(&sc);
        const cJSON *v;
        if (cJSON_IsBool(v = cJSON_GetObjectItem(js, "writeEnabled"))) {
            sc.write_enabled = cJSON_IsTrue(v);
        }
        if (cJSON_IsBool(v = cJSON_GetObjectItem(js, "rawWriteEnabled"))) {
            sc.raw_write_enabled = cJSON_IsTrue(v);
        }
        if (cJSON_IsBool(v = cJSON_GetObjectItem(js, "em380Enabled"))) {
            sc.em380_enabled = cJSON_IsTrue(v);
        }
        copy_str(js, "tz", sc.tz, sizeof(sc.tz));
        sys_cfg_set(&sc);
    }

    const cJSON *jp = cJSON_GetObjectItem(root, "points");
    if (cJSON_IsArray(jp) && !write_section(jp, CFG_POINTS_PATH)) {
        snprintf(err, err_sz, "could not write the datapoint selection");
        return false;
    }
    const cJSON *jsc = cJSON_GetObjectItem(root, "scan");
    if (cJSON_IsObject(jsc) && !write_section(jsc, CFG_SYSTEM_PATH)) {
        snprintf(err, err_sz, "could not write the scan result");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* settings                                                             */

static esp_err_t h_settings_get(httpd_req_t *r)
{
    mqtt_cfg_t mq;
    sys_cfg_t sys;
    wifi_cfg_t wifi;
    mqtt_cfg_get(&mq);
    sys_cfg_get(&sys);
    wifi_cfg_get(&wifi);

    o3e_buf_t b;
    o3e_buf_init(&b);
    char t[64];

    /* Every user-supplied string goes through the JSON escaper: a topic or
     * hostname containing a quote or backslash would otherwise produce a
     * response the browser cannot parse. Passwords are deliberately not
     * returned; the UI shows a placeholder and only sends a value when the
     * user actually types a new one. */
    o3e_buf_adds(&b, "{\"mqtt\": {\"enabled\": ");
    o3e_buf_adds(&b, mq.enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"host\": ");
    o3e_buf_add_json_str(&b, mq.host);
    snprintf(t, sizeof(t), ", \"port\": %u, \"user\": ", mq.port);
    o3e_buf_adds(&b, t);
    o3e_buf_add_json_str(&b, mq.user);
    o3e_buf_adds(&b, ", \"hasPass\": ");
    o3e_buf_adds(&b, mq.pass[0] ? "true" : "false");
    o3e_buf_adds(&b, ", \"baseTopic\": ");
    o3e_buf_add_json_str(&b, mq.base_topic);
    o3e_buf_adds(&b, ", \"format\": ");
    o3e_buf_add_json_str(&b, mq.format);
    o3e_buf_adds(&b, ", \"cmndTopic\": ");
    o3e_buf_add_json_str(&b, mq.cmnd_topic);
    o3e_buf_adds(&b, ", \"haDiscovery\": ");
    o3e_buf_adds(&b, mq.ha_discovery ? "true" : "false");
    o3e_buf_adds(&b, ", \"haPrefix\": ");
    o3e_buf_add_json_str(&b, mq.ha_prefix);

    o3e_buf_adds(&b, "}, \"system\": {\"writeEnabled\": ");
    o3e_buf_adds(&b, sys.write_enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"rawWriteEnabled\": ");
    o3e_buf_adds(&b, sys.raw_write_enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"em380Enabled\": ");
    o3e_buf_adds(&b, sys.em380_enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"collectEnabled\": ");
    o3e_buf_adds(&b, sys.collect_enabled ? "true" : "false");
    o3e_buf_adds(&b, ", \"collectCanIds\": ");
    o3e_buf_add_json_str(&b, sys.collect_canids);
    o3e_buf_adds(&b, ", \"rawCanIds\": ");
    o3e_buf_add_json_str(&b, sys.raw_canids);
    o3e_buf_adds(&b, ", \"tz\": ");
    o3e_buf_add_json_str(&b, sys.tz);
    o3e_buf_adds(&b, ", \"hostname\": ");
    o3e_buf_add_json_str(&b, wifi.hostname);
    o3e_buf_adds(&b, "}}");

    esp_err_t e = send_json(r, b.buf ? b.buf : "{}");
    o3e_buf_free(&b);
    return e;
}

static void copy_str(const cJSON *o, const char *key, char *dst, size_t dst_sz)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (cJSON_IsString(v)) {
        snprintf(dst, dst_sz, "%s", v->valuestring);
    }
}

static esp_err_t h_settings_put(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) {
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        return send_err(r, 400, "not valid JSON");
    }

    const cJSON *jm = cJSON_GetObjectItem(j, "mqtt");
    if (cJSON_IsObject(jm)) {
        mqtt_cfg_t mq;
        mqtt_cfg_get(&mq);
        const cJSON *v;
        if (cJSON_IsBool(v = cJSON_GetObjectItem(jm, "enabled"))) {
            mq.enabled = cJSON_IsTrue(v);
        }
        copy_str(jm, "host", mq.host, sizeof(mq.host));
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(jm, "port"))) {
            mq.port = (uint16_t)v->valuedouble;
        }
        copy_str(jm, "user", mq.user, sizeof(mq.user));
        /* An absent password keeps the stored one. */
        copy_str(jm, "pass", mq.pass, sizeof(mq.pass));
        copy_str(jm, "baseTopic", mq.base_topic, sizeof(mq.base_topic));
        copy_str(jm, "format", mq.format, sizeof(mq.format));
        copy_str(jm, "cmndTopic", mq.cmnd_topic, sizeof(mq.cmnd_topic));
        copy_str(jm, "haPrefix", mq.ha_prefix, sizeof(mq.ha_prefix));
        bool ha_was = mq.ha_discovery;
        if (cJSON_IsBool(v = cJSON_GetObjectItem(jm, "haDiscovery"))) {
            mq.ha_discovery = cJSON_IsTrue(v);
        }
        if (ha_was && !mq.ha_discovery) {
            ha_disco_clear_all();
        }
        mqtt_cfg_set(&mq);
        mqtt_pub_restart();
    }

    const cJSON *js = cJSON_GetObjectItem(j, "system");
    if (cJSON_IsObject(js)) {
        sys_cfg_t sys;
        sys_cfg_get(&sys);
        const cJSON *v;
        if (cJSON_IsBool(v = cJSON_GetObjectItem(js, "writeEnabled"))) {
            sys.write_enabled = cJSON_IsTrue(v);
        }
        if (cJSON_IsBool(v = cJSON_GetObjectItem(js, "rawWriteEnabled"))) {
            sys.raw_write_enabled = cJSON_IsTrue(v);
        }
        bool em_was = sys.em380_enabled;
        if (cJSON_IsBool(v = cJSON_GetObjectItem(js, "em380Enabled"))) {
            sys.em380_enabled = cJSON_IsTrue(v);
        }
        bool coll_was = sys.collect_enabled;
        char coll_ids_was[CFG_STR_MAX];
        snprintf(coll_ids_was, sizeof(coll_ids_was), "%s", sys.collect_canids);
        if (cJSON_IsBool(v = cJSON_GetObjectItem(js, "collectEnabled"))) {
            sys.collect_enabled = cJSON_IsTrue(v);
        }
        copy_str(js, "collectCanIds", sys.collect_canids, sizeof(sys.collect_canids));
        char raw_ids_was[CFG_RAW_IDS_MAX];
        snprintf(raw_ids_was, sizeof(raw_ids_was), "%s", sys.raw_canids);
        copy_str(js, "rawCanIds", sys.raw_canids, sizeof(sys.raw_canids));
        copy_str(js, "tz", sys.tz, sizeof(sys.tz));
        sys_cfg_set(&sys);
        /* Takes effect immediately: enabling only installs a receive filter. */
        if (em_was != sys.em380_enabled) {
            if (sys.em380_enabled) {
                em380_start();
            } else {
                em380_stop();
            }
        }
        if (coll_was != sys.collect_enabled ||
            strcmp(coll_ids_was, sys.collect_canids) != 0) {
            collect_stop();
            if (sys.collect_enabled) {
                uint16_t ids[COLLECT_MAX_IDS];
                size_t n = collect_parse_ids(sys.collect_canids, ids, COLLECT_MAX_IDS);
                if (n) {
                    collect_start(ids, n);
                }
            }
        }
        if (strcmp(raw_ids_was, sys.raw_canids) != 0) {
            raw_relay_stop();
            if (sys.raw_canids[0]) {
                uint16_t ids[RAW_RELAY_MAX_IDS];
                size_t n = collect_parse_ids(sys.raw_canids, ids, RAW_RELAY_MAX_IDS);
                if (n) {
                    raw_relay_start(ids, n);
                }
            }
        }
        setenv("TZ", sys.tz, 1);
        tzset();
    }
    cJSON_Delete(j);
    return send_json(r, "{\"ok\": true}");
}

/* Shared body pump for the three uploads. Streaming is not an optimisation
 * here: a firmware image is over a megabyte and there is nowhere to buffer it. */
static esp_err_t stream_upload(httpd_req_t *r,
                               bool (*chunk)(const void *, size_t, char *, size_t),
                               void (*abort_fn)(void),
                               char *err, size_t err_sz)
{
    char *buf = malloc(4096);
    if (!buf) {
        abort_fn();
        return send_err(r, 500, "out of memory");
    }
    int received = 0;
    while (received < r->content_len) {
        int n = httpd_req_recv(r, buf, 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            abort_fn();
            return send_err(r, 400, "the upload was interrupted");
        }
        if (!chunk(buf, (size_t)n, err, err_sz)) {
            free(buf);
            return send_err(r, 400, err[0] ? err : "writing the upload failed");
        }
        received += n;
    }
    free(buf);
    return ESP_OK;
}

/* Replace one file on the storage partition. This is the everyday path while
 * the interface is still changing: app.js is a few KiB, the whole partition
 * image is four megabytes. */
static esp_err_t h_fs_put(httpd_req_t *r)
{
    char q[256], name[160];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "path", name, sizeof(name)) != ESP_OK) {
        return send_err(r, 400, "path parameter is required");
    }

    char err[160] = "";
    if (!storage_file_begin(name, err, sizeof(err))) {
        return send_err(r, 400, err);
    }
    esp_err_t e = stream_upload(r, storage_file_write, storage_file_abort,
                                err, sizeof(err));
    if (e != ESP_OK) {
        return e;
    }
    if (!storage_file_end(err, sizeof(err))) {
        return send_err(r, 400, err);
    }
    return send_json(r, "{\"ok\": true}");
}

/* Replace the whole storage partition. Needed when the datapoint database
 * changes; it takes the saved selection with it, so the UI warns first. */
static esp_err_t h_storage_put(httpd_req_t *r)
{
    char err[160] = "";
    if (!storage_image_begin((size_t)r->content_len, err, sizeof(err))) {
        return send_err(r, 400, err);
    }
    esp_err_t e = stream_upload(r, storage_image_write, storage_image_abort,
                                err, sizeof(err));
    if (e != ESP_OK) {
        return e;
    }
    if (!storage_image_end(err, sizeof(err))) {
        return send_err(r, 400, err);
    }
    /* The filesystem is unmounted at this point, so nothing else works until
     * the device comes back up. */
    send_json(r, "{\"ok\": true, \"restarting\": true}");
    restart_soon(800);
    return ESP_OK;
}

/* Firmware upload. The body is the raw .bin, streamed straight into the spare
 * app partition -- buffering a megabyte first would not fit in RAM. */
static esp_err_t h_ota(httpd_req_t *r)
{
    char err[160] = "";
    if (!ota_begin((size_t)r->content_len, err, sizeof(err))) {
        return send_err(r, 400, err[0] ? err : "could not start the update");
    }
    esp_err_t e = stream_upload(r, ota_write, ota_abort, err, sizeof(err));
    if (e != ESP_OK) {
        return e;
    }
    if (!ota_end(err, sizeof(err))) {
        return send_err(r, 400, err[0] ? err : "the update could not be completed");
    }

    send_json(r, "{\"ok\": true, \"restarting\": true}");

    restart_soon(800);
    return ESP_OK;
}

/* Reset endpoints. Each clears one thing, so a bad datapoint selection can be
 * discarded without also losing the Wi-Fi credentials that make the device
 * reachable. */
static esp_err_t h_reset(httpd_req_t *r)
{
    char *body = read_body(r);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *what = j ? cJSON_GetObjectItem(j, "what") : NULL;
    if (!cJSON_IsString(what)) {
        cJSON_Delete(j);
        return send_err(r, 400, "what must be one of: points, scan, mqtt, all");
    }
    char scope[24];
    snprintf(scope, sizeof(scope), "%s", what->valuestring);
    cJSON_Delete(j);

    bool restart = false;
    if (!strcmp(scope, "points")) {
        /* Clear the discovery topics before the selection goes, otherwise the
         * entities linger in Home Assistant with nothing feeding them. */
        ha_disco_clear_all();
        remove(CFG_POINTS_PATH);
        poller_reload();
    } else if (!strcmp(scope, "scan")) {
        ha_disco_clear_all();
        remove(CFG_SYSTEM_PATH);
        remove(CFG_POINTS_PATH);
        poller_reload();
    } else if (!strcmp(scope, "mqtt")) {
        mqtt_cfg_t mq;
        memset(&mq, 0, sizeof(mq));
        mqtt_cfg_set(&mq);
        mqtt_pub_restart();
    } else if (!strcmp(scope, "all")) {
        ha_disco_clear_all();
        remove(CFG_POINTS_PATH);
        remove(CFG_SYSTEM_PATH);
        mqtt_cfg_t mq;
        memset(&mq, 0, sizeof(mq));
        mqtt_cfg_set(&mq);
        sys_cfg_t sys;
        memset(&sys, 0, sizeof(sys));
        sys_cfg_set(&sys);
        /* Wi-Fi is deliberately kept: "reset everything" should not strand the
         * device on a network nobody can reach it on. Use "WLAN vergessen" for
         * that, which says what it does. */
        restart = true;
    } else {
        return send_err(r, 400, "what must be one of: points, scan, mqtt, all");
    }

    ESP_LOGW(TAG, "reset: %s", scope);
    send_json(r, restart ? "{\"ok\": true, \"restarting\": true}" : "{\"ok\": true}");
    if (restart) {
        restart_soon(800);
    }
    return ESP_OK;
}

static esp_err_t h_restart(httpd_req_t *r)
{
    send_json(r, "{\"ok\": true}");
    restart_soon(800);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* static files and the captive portal                                  */

static const char *mime_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "text/plain";
    }
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

static esp_err_t serve_file(httpd_req_t *r, const char *uri)
{
    char path[160];
    snprintf(path, sizeof(path), "%s%s", CFG_WWW_DIR,
             (!uri[0] || !strcmp(uri, "/")) ? "/index.html" : uri);

    /* Assets are stored pre-compressed to save flash and transfer time; try
     * the .gz first and fall back for anything stored plain. */
    char gz[176];
    snprintf(gz, sizeof(gz), "%s.gz", path);
    bool gzipped = true;
    FILE *f = fopen(gz, "rb");
    if (!f) {
        gzipped = false;
        f = fopen(path, "rb");
    }
    if (!f) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(r, mime_of(path));
    if (gzipped) {
        httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    }
    /* Revalidate rather than cache: the interface is replaced through the web
     * UI itself, and a browser holding on to the previous app.js makes an
     * update look like it did not take. The files are a few KiB. */
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");

    char chunk[1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(r, chunk, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* Answers everything not matched by the API. In setup mode this doubles as the
 * captive portal: the probe URLs Android, iOS and Windows fetch get a redirect
 * to the setup page, which is what makes the sign-in sheet appear by itself. */
static esp_err_t h_fallback(httpd_req_t *r)
{
    if (net_prov_is_setup_mode()) {
        static const char *probes[] = {
            "/generate_204", "/gen_204", "/hotspot-detect.html",
            "/library/test/success.html", "/ncsi.txt", "/connecttest.txt",
            "/canonical.html", "/success.txt",
        };
        for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            if (strcmp(r->uri, probes[i]) == 0) {
                httpd_resp_set_status(r, "302 Found");
                httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
                return httpd_resp_send(r, NULL, 0);
            }
        }
    }
    if (serve_file(r, r->uri) == ESP_OK) {
        return ESP_OK;
    }
    /* A single-page UI routes client-side, so an unknown path is the app, not
     * a 404 -- unless it is an API path, where a 404 is the honest answer. */
    if (strncmp(r->uri, "/api/", 5) == 0) {
        return send_err(r, 404, "no such endpoint");
    }
    return serve_file(r, "/index.html") == ESP_OK
         ? ESP_OK
         : send_err(r, 404, "web interface not installed on the storage partition");
}

/* ------------------------------------------------------------------ */

/* Kept at file scope so the server's handler limit is derived from the table
 * rather than tracked by hand. Getting that wrong is silent: registration of
 * the surplus handler fails, and since the wildcard is registered last it was
 * the web interface itself that disappeared. */
static const httpd_uri_t routes[] = {
    { "/api/status",      HTTP_GET,  h_status,       NULL },
    { "/api/sysinfo",     HTTP_GET,  h_sysinfo,      NULL },
    { "/api/wifi/scan",   HTTP_GET,  h_wifi_scan,    NULL },
    { "/api/wifi",        HTTP_POST, h_wifi_save,    NULL },
    { "/api/wifi/forget", HTTP_POST, h_wifi_forget,  NULL },
    { "/api/scan",        HTTP_POST, h_scan_start,   NULL },
    { "/api/scan/abort",  HTTP_POST, h_scan_abort,   NULL },
    { "/api/system",      HTTP_GET,  h_system,       NULL },
    { "/api/devices",     HTTP_PUT,  h_devices_put,  NULL },
    { "/api/datapoint",   HTTP_GET,  h_datapoint,    NULL },
    { "/api/names",       HTTP_GET,  h_names,        NULL },
    { "/api/candiag",     HTTP_POST, h_candiag,      NULL },
    { "/api/em380",       HTTP_GET,  h_em380,        NULL },
    /* Not "/api/collect": that path is what Google Analytics uses, so ad
       blockers refuse it outright. The request never left the browser --
       ERR_BLOCKED_BY_CLIENT -- while the same URL answered fine from curl,
       which cost a long hunt through memory, sockets and crash dumps. */
    { "/api/broadcast",   HTTP_GET,  h_collect,      NULL },
    { "/api/points",      HTTP_GET,  h_points_get,   NULL },
    { "/api/points",      HTTP_PUT,  h_points_put,   NULL },
    { "/api/read",        HTTP_GET,  h_read,         NULL },
    { "/api/write",       HTTP_POST, h_write,        NULL },
    { "/api/grid",        HTTP_POST, h_grid,         NULL },
    { "/api/crash",       HTTP_GET,  h_crash,        NULL },
    { "/api/crash",       HTTP_DELETE, h_crash,      NULL },
    { "/api/rawread",     HTTP_GET,  h_rawread,      NULL },
    { "/api/rawwrite",    HTTP_POST, h_rawwrite,     NULL },
    { "/api/trace",       HTTP_POST, h_trace_ctl,    NULL },
    { "/api/trace",       HTTP_GET,  h_trace_status, NULL },
    { "/api/trace/frames", HTTP_GET, h_trace_frames, NULL },
    { "/api/trace/ids",   HTTP_GET,  h_trace_ids,    NULL },
    { "/api/trace/dump",  HTTP_GET,  h_trace_dump,   NULL },
    { "/api/export",      HTTP_GET,  h_export,       NULL },
    { "/api/import",      HTTP_POST, h_import,       NULL },
    { "/api/settings",    HTTP_GET,  h_settings_get, NULL },
    { "/api/settings",    HTTP_PUT,  h_settings_put, NULL },
    { "/api/ota",         HTTP_POST, h_ota,          NULL },
    { "/api/fs",          HTTP_POST, h_fs_put,       NULL },
    { "/api/storage",     HTTP_POST, h_storage_put,  NULL },
    { "/api/reset",       HTTP_POST, h_reset,        NULL },
    { "/api/restart",     HTTP_POST, h_restart,      NULL },
    /* Must be last: it matches everything the routes above did not. */
    { "/*",               HTTP_GET,  h_fallback,     NULL },
};

#define N_ROUTES (sizeof(routes) / sizeof(routes[0]))

/* Checked here rather than discovered on the device: exceeding the budget does
 * not degrade anything, it stops the server starting, and a gateway with no web
 * interface can only be recovered over a cable. */
#define HTTPD_CLIENT_SOCKETS 12
_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= HTTPD_CLIENT_SOCKETS + 3,
               "LWIP_MAX_SOCKETS must leave room for the HTTP server's own three");

bool httpd_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = N_ROUTES;
    cfg.stack_size = 8192;
    /* Chrome opens up to six connections to one host and keeps them alive, so
     * the browser alone can reach the default limit of seven; the next request
     * then closes the oldest, which lands on whichever answer took longest to
     * produce -- the broadcast channel's fifteen kilobytes. It failed in a
     * browser and succeeded from curl, because curl asks one thing at a time.
     *
     * This counts client connections only; three more are the server's own,
     * and the sum may not exceed LWIP_MAX_SOCKETS. That is raised to sixteen
     * in sdkconfig.defaults -- setting this alone stops the server starting at
     * all, which is a worse failure than the one it was meant to fix. */
    cfg.max_open_sockets = HTTPD_CLIENT_SOCKETS;
    /* Still on: running out should cost the oldest idle connection, not the
     * ability to answer at all. */
    cfg.lru_purge_enable = true;
    /* A firmware upload is over a megabyte across a shared 2.4 GHz link; the
     * default 5 s socket timeout gives up long before it finishes. */
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "could not start the HTTP server");
        return false;
    }

    for (size_t i = 0; i < N_ROUTES; i++) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) {
            /* Never silent again: losing the wildcard takes the whole
             * interface down and looks like a corrupt filesystem. */
            ESP_LOGE(TAG, "could not register %s: %s", routes[i].uri,
                     esp_err_to_name(e));
            httpd_stop(server);
            server = NULL;
            return false;
        }
    }
    ESP_LOGI(TAG, "HTTP server listening on port 80, %u routes", (unsigned)N_ROUTES);
    return true;
}

void httpd_api_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}
