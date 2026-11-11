#include "mqtt_pub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "app_config.h"
#include "can_port.h"
#include "contact.h"
#include "ha_disco.h"
#include "poller.h"
#include "mqtt_cmnd.h"
#include "o3e_flatten.h"
#include "o3e_json.h"

static const char *TAG = "mqtt";

static void publish_leaf(void *user, const char *topic, const char *value);

static esp_mqtt_client_handle_t client;
static mqtt_stats_t stats;
static char lwt_topic[CFG_TOPIC_MAX + 8];

/* Restarting the client tears a task down and waits for it. Doing that inside
 * the HTTP handler that just saved the settings makes the browser wait on it
 * too, so any stall there looks like the device has died. The restart runs on
 * a task of its own and the settings response goes out immediately. */
static TaskHandle_t ctl_task_h;

/* Work handed to the control task. Both items are too heavy for the contexts
 * that request them: a restart tears down a task, and announcing walks the
 * whole selection compiling codecs -- neither belongs in an HTTP handler or in
 * the MQTT client's own event callback. */
#define CTL_RESTART   (1u << 0)
#define CTL_ANNOUNCE  (1u << 1)

static bool ctl_notify(uint32_t bits);

/* ------------------------------------------------------------------ */
/* Topic formatting                                                     */

/* open3e builds topics with Python's str.format(). Rather than a general
 * format engine we recognise the placeholders its README documents; anything
 * else is copied through so a typo is visible in the topic instead of being
 * silently dropped. */
static bool match_placeholder(const char *p, const char *name, size_t *adv,
                              char *spec, size_t spec_sz)
{
    size_t nlen = strlen(name);
    if (strncmp(p, name, nlen) != 0) {
        return false;
    }
    const char *q = p + nlen;
    spec[0] = '\0';
    if (*q == ':') {
        q++;
        size_t i = 0;
        while (*q && *q != '}' && i + 1 < spec_sz) {
            spec[i++] = *q++;
        }
        spec[i] = '\0';
    }
    if (*q != '}') {
        return false;
    }
    *adv = (size_t)(q - p) + 1;
    return true;
}

void mqtt_pub_topic(char *out, size_t out_sz, uint16_t ecu, uint16_t did,
                    const char *did_name, const char *device,
                    const char *suffix_override)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);

    /* A per-datapoint override replaces the formatted part, not the base. */
    if (suffix_override && suffix_override[0]) {
        snprintf(out, out_sz, "%s/%s", cfg.base_topic, suffix_override);
        return;
    }

    size_t o = (size_t)snprintf(out, out_sz, "%s/", cfg.base_topic);
    const char *p = cfg.format;
    char spec[16];

    while (*p && o + 1 < out_sz) {
        if (*p != '{') {
            out[o++] = *p++;
            continue;
        }
        p++;
        size_t adv;
        char sub[96];
        if (match_placeholder(p, "didName", &adv, spec, sizeof(spec))) {
            snprintf(sub, sizeof(sub), "%s", did_name ? did_name : "");
        } else if (match_placeholder(p, "didNumber", &adv, spec, sizeof(spec))) {
            /* The documented spec is {didNumber:04d}; treat any d-spec as a
             * zero-padded width. */
            int width = (spec[0] == '0') ? atoi(spec) : 0;
            snprintf(sub, sizeof(sub), "%0*u", width, did);
        } else if (match_placeholder(p, "ecuAddr", &adv, spec, sizeof(spec))) {
            if (strchr(spec, 'X')) {
                snprintf(sub, sizeof(sub), "%03X", ecu);
            } else if (strchr(spec, 'x')) {
                snprintf(sub, sizeof(sub), "%03x", ecu);
            } else {
                snprintf(sub, sizeof(sub), "%u", ecu);   /* open3e's plain int */
            }
        } else if (match_placeholder(p, "device", &adv, spec, sizeof(spec))) {
            snprintf(sub, sizeof(sub), "%s", device ? device : "");
        } else {
            out[o++] = '{';
            continue;
        }
        p += adv;
        for (const char *s = sub; *s && o + 1 < out_sz; s++) {
            out[o++] = *s;
        }
    }
    out[o < out_sz ? o : out_sz - 1] = '\0';
}

/* ------------------------------------------------------------------ */

/* Bridges o3e_flatten's callback to the broker; `user` carries the retain
 * flag, or NULL for a plain publish. */
static void publish_leaf(void *user, const char *topic, const char *value)
{
    mqtt_pub_raw(topic, value, user != NULL);
}

static void clear_leaf(void *user, const char *topic, const char *value)
{
    (void)user;
    (void)value;
    mqtt_pub_raw(topic, "", true);
}

bool mqtt_pub_raw(const char *topic, const char *payload, bool retain)
{
    if (!client || !stats.connected) {
        return false;
    }
    int id = esp_mqtt_client_publish(client, topic, payload, 0, 0, retain ? 1 : 0);
    if (id < 0) {
        stats.errors++;
        return false;
    }
    stats.published++;
    return true;
}

bool mqtt_pub_value(const char *topic, const o3e_node_t *node,
                    const uint8_t *payload, size_t len, pub_mode_t mode,
                    bool retain)
{
    static int retain_flag;
    if (mode == PUB_MODE_FLAT) {
        uint32_t n = o3e_flatten(node, payload, len, topic, publish_leaf,
                                 retain ? &retain_flag : NULL);
        return n > 0;
    }

    char *json = o3e_codec_decode_json(node, payload, len);
    if (!json) {
        return false;
    }
    bool ok = mqtt_pub_raw(topic, json, retain);
    free(json);
    return ok;
}

void mqtt_pub_clear(const char *topic, const o3e_node_t *node,
                    const uint8_t *last, size_t last_len, pub_mode_t mode)
{
    if (!client || !stats.connected) {
        return;
    }
    if (mode != PUB_MODE_FLAT) {
        mqtt_pub_raw(topic, "", true);
        return;
    }
    uint8_t *zero = NULL;
    if (!last) {
        if (!node || !node->len) {
            return;
        }
        zero = calloc(node->len, 1);
        if (!zero) {
            return;
        }
        last = zero;
        last_len = node->len;
    }
    o3e_flatten(node, last, last_len, topic, clear_leaf, NULL);
    free(zero);
}

/* ------------------------------------------------------------------ */
/* Status push                                                          */

/* Retained <base>/status, refreshed whenever CAN or MQTT health changes so a
 * remote client (e.g. ioBroker.e3oncan's gateway transport) sees the current
 * state on connect and on any real change afterwards, instead of having to
 * poll /api/status. LWT already covers "the firmware is completely gone";
 * this covers "the firmware is up but the bus or broker link is unwell". */
static TaskHandle_t status_task_h;

static void status_push_check(bool force)
{
    static char last_state[16] = "";
    static bool last_mqtt;
    static uint16_t last_tec, last_rec;
    static bool have_last;

    can_stats_t cs;
    can_port_stats(&cs);
    bool mqtt_ok = stats.connected;
    const char *state = cs.state ? cs.state : "unknown";

    if (!force && have_last && strcmp(last_state, state) == 0 &&
        last_mqtt == mqtt_ok && last_tec == cs.tx_err_count &&
        last_rec == cs.rx_err_count) {
        return;
    }

    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    char topic[CFG_TOPIC_MAX + 8];
    snprintf(topic, sizeof(topic), "%s/status", cfg.base_topic);

    char json[96];
    snprintf(json, sizeof(json),
             "{\"can\": \"%s\", \"mqtt\": %s, \"tec\": %u, \"rec\": %u}",
             state, mqtt_ok ? "true" : "false", cs.tx_err_count, cs.rx_err_count);

    if (mqtt_pub_raw(topic, json, true)) {
        snprintf(last_state, sizeof(last_state), "%s", state);
        last_mqtt = mqtt_ok;
        last_tec = cs.tx_err_count;
        last_rec = cs.rx_err_count;
        have_last = true;
    }
}

static void status_push_task(void *arg)
{
    (void)arg;
    for (;;) {
        status_push_check(false);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ------------------------------------------------------------------ */

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        stats.connected = true;
        stats.reconnects++;
        ESP_LOGI(TAG, "connected");
        /* open3e publishes a retained online/offline flag on <base>/LWT. */
        esp_mqtt_client_publish(client, lwt_topic, "online", 0, 0, 1);
        mqtt_cfg_t cfg;
        mqtt_cfg_get(&cfg);
        if (cfg.cmnd_topic[0]) {
            esp_mqtt_client_subscribe(client, cfg.cmnd_topic, 0);
        }
        /* The relay outputs, <base>/output/<name>/set. A wildcard, so a
         * renamed output needs no resubscribe; subscribed even with no output
         * configured, because one subscription is cheaper than tracking. */
        char set_topic[CFG_TOPIC_MAX + 16];
        snprintf(set_topic, sizeof(set_topic), "%s/output/+/set", cfg.base_topic);
        esp_mqtt_client_subscribe(client, set_topic, 0);
        /* Handed to the control task rather than done here: this callback runs
         * on the client's own task, and blocking it stops the keepalive. */
        ctl_notify(CTL_ANNOUNCE);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        stats.connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_ERROR:
        stats.errors++;
        break;
    case MQTT_EVENT_DATA:
        /* Command handling lives in cmnd.c to keep this file about transport. */
        mqtt_cmnd_dispatch(e->topic, e->topic_len, e->data, e->data_len);
        break;
    default:
        break;
    }
}

bool mqtt_pub_start(void)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    if (!cfg.enabled || !cfg.host[0]) {
        ESP_LOGI(TAG, "disabled or no broker configured");
        return false;
    }

    char uri[CFG_STR_MAX + 32];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", cfg.host, cfg.port);
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/LWT", cfg.base_topic);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        .credentials.username = cfg.user[0] ? cfg.user : NULL,
        .credentials.authentication.password = cfg.pass[0] ? cfg.pass : NULL,
        .session.last_will = {
            .topic = lwt_topic,
            .msg = "offline",
            .qos = 0,
            .retain = 1,
        },
        .session.keepalive = 30,
    };

    client = esp_mqtt_client_init(&mc);
    if (!client) {
        return false;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    return esp_mqtt_client_start(client) == ESP_OK;
}

void mqtt_pub_stop(void)
{
    if (!client) {
        return;
    }
    if (stats.connected) {
        esp_mqtt_client_publish(client, lwt_topic, "offline", 0, 0, 1);
    }
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = NULL;
    stats.connected = false;
}

static void mqtt_ctl_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t what = 0;
        xTaskNotifyWait(0, UINT32_MAX, &what, portMAX_DELAY);

        if (what & CTL_ANNOUNCE) {
            /* The broker has just become reachable. Home Assistant needs its
             * discovery topics again -- they are retained but a fresh broker
             * or a restarted one has none -- and the datapoints whose first
             * readings were dropped while disconnected should not wait out a
             * whole interval. */
            ha_disco_publish_all();
            /* Retained, so this only matters for a broker that has lost its
             * retained set -- but that is exactly the case where a doorbell
             * would otherwise read "unknown" until the next visitor. */
            contact_publish();
            /* This walk is the deepest thing this task does, and it grew a
             * control entity per writable leaf. It overflowed a 4 KiB stack
             * once, and the only symptom was that everything after it went
             * missing -- so the margin is now reported rather than assumed. */
            UBaseType_t left = uxTaskGetStackHighWaterMark(NULL);
            if (left < 1024) {
                ESP_LOGW(TAG, "control task stack down to %u bytes", (unsigned)left);
            } else {
                ESP_LOGD(TAG, "control task stack margin %u bytes", (unsigned)left);
            }
            poller_refresh();
            /* Retained topics may need refreshing after a broker or client
             * restart, same reasoning as the HA discovery publish above. */
            status_push_check(true);
            if (!status_task_h) {
                xTaskCreate(status_push_task, "mqttstatus", 3072, NULL, 3, &status_task_h);
            }
        }
        if (what & CTL_RESTART) {
            ESP_LOGI(TAG, "applying new settings");
            mqtt_pub_stop();
            mqtt_pub_start();
        }
    }
}

static bool ctl_notify(uint32_t bits)
{
    if (!ctl_task_h &&
        xTaskCreate(mqtt_ctl_task, "mqttctl", 8192, NULL, 4, &ctl_task_h) != pdPASS) {
        return false;
    }
    xTaskNotify(ctl_task_h, bits, eSetBits);
    return true;
}

void mqtt_pub_restart(void)
{
    if (!ctl_notify(CTL_RESTART)) {
        /* No task available: do it inline rather than not at all. */
        mqtt_pub_stop();
        mqtt_pub_start();
    }
}

void mqtt_pub_stats(mqtt_stats_t *out) { *out = stats; }
bool mqtt_pub_connected(void) { return stats.connected; }
bool mqtt_pub_running(void) { return client != NULL; }
