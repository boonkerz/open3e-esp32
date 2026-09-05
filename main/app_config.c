#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";

/* cJSON allocates from PSRAM.
 *
 * Its nodes are small -- around 64 bytes -- and CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
 * sends everything below 16 KiB to internal RAM, so a parse tree is built
 * entirely out of the pool the Wi-Fi stack and the web server share. Restoring
 * a backup meant roughly 250 KiB of nodes for the scan result alone, on a chip
 * with about 320 KiB of internal RAM in total: the parse simply failed.
 *
 * Nothing here is latency critical, and memory from heap_caps_malloc is freed
 * with the ordinary free(). */
static void *json_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);   /* boards without PSRAM keep working */
}

static void json_free(void *p)
{
    free(p);
}
static const char *NS = "open3e";
static bool fs_ok;

/* Defaults chosen to match open3e's own so an existing broker setup, Home
 * Assistant template or MQTT subscription keeps working unchanged. */
static const mqtt_cfg_t MQTT_DEFAULTS = {
    .port = 1883,
    .base_topic = "open3e",
    .format = "{didName}",
    .cmnd_topic = "open3e/cmnd",
    .ha_prefix = "homeassistant",
    .ha_discovery = true,
};

bool app_config_init(void)
{
    cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = json_free };
    cJSON_InitHooks(&hooks);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = CFG_MOUNT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs: %u KiB used of %u KiB",
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    fs_ok = true;
    return true;
}

bool app_config_fs_mounted(void) { return fs_ok; }

/* ------------------------------------------------------------------ */
/* NVS helpers                                                          */

static void nvs_get_str_or(nvs_handle_t h, const char *key, char *out,
                           size_t out_sz, const char *dflt)
{
    size_t len = out_sz;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        snprintf(out, out_sz, "%s", dflt ? dflt : "");
    }
}

static uint8_t nvs_get_u8_or(nvs_handle_t h, const char *key, uint8_t dflt)
{
    uint8_t v;
    return nvs_get_u8(h, key, &v) == ESP_OK ? v : dflt;
}

static uint16_t nvs_get_u16_or(nvs_handle_t h, const char *key, uint16_t dflt)
{
    uint16_t v;
    return nvs_get_u16(h, key, &v) == ESP_OK ? v : dflt;
}

/* ------------------------------------------------------------------ */
/* Wi-Fi                                                               */

void wifi_cfg_get(wifi_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        snprintf(out->hostname, sizeof(out->hostname), "open3e");
        snprintf(out->ap_pass, sizeof(out->ap_pass), "open3e-setup");
        return;
    }
    nvs_get_str_or(h, "wifi_ssid", out->ssid, sizeof(out->ssid), "");
    nvs_get_str_or(h, "wifi_pass", out->pass, sizeof(out->pass), "");
    nvs_get_str_or(h, "hostname", out->hostname, sizeof(out->hostname), "open3e");
    nvs_get_str_or(h, "ap_pass", out->ap_pass, sizeof(out->ap_pass), "open3e-setup");
    nvs_close(h);
}

bool wifi_cfg_set(const wifi_cfg_t *in)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_str(h, "wifi_ssid", in->ssid);
    nvs_set_str(h, "wifi_pass", in->pass);
    nvs_set_str(h, "hostname", in->hostname[0] ? in->hostname : "open3e");
    nvs_set_str(h, "ap_pass", in->ap_pass[0] ? in->ap_pass : "open3e-setup");
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool wifi_cfg_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_erase_key(h, "wifi_ssid");
    nvs_erase_key(h, "wifi_pass");
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool wifi_cfg_present(void)
{
    wifi_cfg_t c;
    wifi_cfg_get(&c);
    return c.ssid[0] != '\0';
}

/* ------------------------------------------------------------------ */
/* MQTT                                                                */

void mqtt_cfg_get(mqtt_cfg_t *out)
{
    *out = MQTT_DEFAULTS;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    nvs_get_str_or(h, "mq_host", out->host, sizeof(out->host), "");
    out->port = nvs_get_u16_or(h, "mq_port", MQTT_DEFAULTS.port);
    nvs_get_str_or(h, "mq_user", out->user, sizeof(out->user), "");
    nvs_get_str_or(h, "mq_pass", out->pass, sizeof(out->pass), "");
    nvs_get_str_or(h, "mq_base", out->base_topic, sizeof(out->base_topic),
                   MQTT_DEFAULTS.base_topic);
    nvs_get_str_or(h, "mq_fmt", out->format, sizeof(out->format), MQTT_DEFAULTS.format);
    nvs_get_str_or(h, "mq_cmnd", out->cmnd_topic, sizeof(out->cmnd_topic),
                   MQTT_DEFAULTS.cmnd_topic);
    /* A default only covers a missing key, not a key holding "". A stored
     * empty value would otherwise stay empty forever, and it disables both the
     * command listener and every Home Assistant control -- so heal it on the
     * way out rather than waiting for someone to save the settings again. */
    if (!out->cmnd_topic[0]) {
        /* Both fields are the same width, so the base has to be clipped to
         * leave room for the suffix rather than relying on truncation. */
        snprintf(out->cmnd_topic, sizeof(out->cmnd_topic), "%.*s/cmnd",
                 (int)(sizeof(out->cmnd_topic) - sizeof("/cmnd")),
                 out->base_topic[0] ? out->base_topic : "open3e");
    }
    nvs_get_str_or(h, "ha_prefix", out->ha_prefix, sizeof(out->ha_prefix),
                   MQTT_DEFAULTS.ha_prefix);
    out->enabled = nvs_get_u8_or(h, "mq_on", 0) != 0;
    out->ha_discovery = nvs_get_u8_or(h, "ha_on", 1) != 0;
    nvs_close(h);
}

bool mqtt_cfg_set(const mqtt_cfg_t *in)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_str(h, "mq_host", in->host);
    nvs_set_u16(h, "mq_port", in->port ? in->port : 1883);
    nvs_set_str(h, "mq_user", in->user);
    nvs_set_str(h, "mq_pass", in->pass);
    nvs_set_str(h, "mq_base", in->base_topic[0] ? in->base_topic : "open3e");
    nvs_set_str(h, "mq_fmt", in->format[0] ? in->format : "{didName}");
    /* Every other string here falls back when left blank; this one did not,
     * and an empty command topic disables both the command listener and every
     * Home Assistant control -- silently, because nothing publishes an error
     * for a feature that simply never appears. Derived from the base topic so
     * two gateways on one broker do not share a channel. */
    char cmnd[CFG_TOPIC_MAX + 8];
    if (in->cmnd_topic[0]) {
        snprintf(cmnd, sizeof(cmnd), "%s", in->cmnd_topic);
    } else {
        snprintf(cmnd, sizeof(cmnd), "%s/cmnd",
                 in->base_topic[0] ? in->base_topic : "open3e");
    }
    nvs_set_str(h, "mq_cmnd", cmnd);
    nvs_set_str(h, "ha_prefix", in->ha_prefix[0] ? in->ha_prefix : "homeassistant");
    nvs_set_u8(h, "mq_on", in->enabled ? 1 : 0);
    nvs_set_u8(h, "ha_on", in->ha_discovery ? 1 : 0);
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ------------------------------------------------------------------ */
/* System                                                              */

void sys_cfg_get(sys_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    /* Central European time with DST: open3e's O3EUtc and O3EDateTime codecs
     * render local time, and a German heating installation is the common case. */
    const char *tz_default = "CET-1CEST,M3.5.0,M10.5.0/3";
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        snprintf(out->tz, sizeof(out->tz), "%s", tz_default);
        snprintf(out->collect_canids, sizeof(out->collect_canids), "0x451,0x441");
        return;
    }
    out->write_enabled = nvs_get_u8_or(h, "wr_on", 0) != 0;
    out->raw_write_enabled = nvs_get_u8_or(h, "raw_wr_on", 0) != 0;
    out->em380_enabled = nvs_get_u8_or(h, "em380_on", 0) != 0;
    out->collect_enabled = nvs_get_u8_or(h, "coll_on", 0) != 0;
    nvs_get_str_or(h, "coll_ids", out->collect_canids, sizeof(out->collect_canids),
                   "0x451,0x441");
    nvs_get_str_or(h, "raw_ids", out->raw_canids, sizeof(out->raw_canids), "");
    nvs_get_str_or(h, "tz", out->tz, sizeof(out->tz), tz_default);
    out->grid_ecu = (uint16_t)nvs_get_u16_or(h, "grid_ecu", 0);
    out->grid_watts = (uint16_t)nvs_get_u16_or(h, "grid_w", 2000);
    out->grid_minutes = (uint16_t)nvs_get_u16_or(h, "grid_min", 15);
    nvs_close(h);
}

bool sys_cfg_set(const sys_cfg_t *in)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_u8(h, "wr_on", in->write_enabled ? 1 : 0);
    nvs_set_u8(h, "raw_wr_on", in->raw_write_enabled ? 1 : 0);
    nvs_set_u8(h, "em380_on", in->em380_enabled ? 1 : 0);
    nvs_set_u8(h, "coll_on", in->collect_enabled ? 1 : 0);
    nvs_set_str(h, "coll_ids",
                in->collect_canids[0] ? in->collect_canids : "0x451,0x441");
    nvs_set_str(h, "raw_ids", in->raw_canids);
    nvs_set_str(h, "tz", in->tz);
    nvs_set_u16(h, "grid_ecu", in->grid_ecu);
    nvs_set_u16(h, "grid_w", in->grid_watts ? in->grid_watts : 2000);
    nvs_set_u16(h, "grid_min", in->grid_minutes ? in->grid_minutes : 15);
    bool ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Selection file entries                                               */

bool sel_is_em380(const struct cJSON *entry)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive((const cJSON *)entry, "type");
    return cJSON_IsString(t) && t->valuestring && strcmp(t->valuestring, "em380") == 0;
}

bool sel_is_datapoint(const struct cJSON *entry)
{
    /* A datapoint is anything that is not another known kind and actually
     * carries the two fields the poller needs. Checking for the fields rather
     * than only for the absence of a type keeps a malformed entry from
     * reaching code that would dereference them. */
    if (sel_is_em380(entry)) {
        return false;
    }
    const cJSON *e = cJSON_GetObjectItemCaseSensitive((const cJSON *)entry, "ecu");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive((const cJSON *)entry, "did");
    return cJSON_IsNumber(e) && cJSON_IsNumber(d);
}

bool sel_enabled(const struct cJSON *entry)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)entry, "enabled");
    return !cJSON_IsBool(v) || cJSON_IsTrue(v);
}

uint16_t sel_u16(const struct cJSON *entry, const char *key, uint16_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)entry, key);
    return cJSON_IsNumber(v) ? (uint16_t)v->valuedouble : dflt;
}

uint32_t sel_u32(const struct cJSON *entry, const char *key, uint32_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)entry, key);
    return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : dflt;
}

const char *sel_str(const struct cJSON *entry, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)entry, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

bool sel_bool(const struct cJSON *entry, const char *key, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)entry, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}

/* ------------------------------------------------------------------ */
/* JSON files                                                           */

char *app_config_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (buf) {
        buf[n] = '\0';
    }
    fclose(f);
    return buf;
}

static void temp_name(const char *path, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s.tmp", path);
}

FILE *app_config_begin_write(const char *path)
{
    char tmp[160];
    temp_name(path, tmp, sizeof(tmp));
    return fopen(tmp, "wb");
}

bool app_config_commit_write(const char *path, FILE *f)
{
    char tmp[160];
    temp_name(path, tmp, sizeof(tmp));

    bool ok = f && fclose(f) == 0;
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

void app_config_abort_write(const char *path, FILE *f)
{
    char tmp[160];
    temp_name(path, tmp, sizeof(tmp));
    if (f) {
        fclose(f);
    }
    remove(tmp);
}

void app_config_fput_json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (*p < 0x20) {
                fprintf(f, "\\u%04x", *p);
            } else {
                fputc((char)*p, f);
            }
        }
    }
    fputc('"', f);
}

bool app_config_write_file(const char *path, const char *data, size_t len)
{
    /* Write to a temporary file and rename, so a power cut mid-save cannot
     * leave a half-written selection that the next boot fails to parse. */
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}
