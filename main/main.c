/* open3e CAN gateway for the Waveshare ESP32-S3-RS485-CAN board.
 *
 * Reads Viessmann E3 controllers (Vitocal, Vitodens, VX3) over CAN and
 * publishes them to MQTT, replacing a Raspberry Pi running open3e with a board
 * that fits in the boiler room and runs off the same 24 V supply.
 *
 * Boot order matters: configuration and the datapoint database first, then the
 * CAN bus, then the network.  The web UI needs the database to render anything
 * useful, and the poller needs both before it can publish.
 */
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "can_port.h"
#include "collect.h"
#include "cantrace.h"
#include "em380.h"
#include "httpd_api.h"
#include "mqtt_pub.h"
#include "net_prov.h"
#include "o3e_db.h"
#include "ota.h"
#include "poller.h"
#include "raw_relay.h"

static const char *TAG = "main";

/* Runs on the tracer's own notification task, not in the receive interrupt. */
static void trace_trigger_to_mqtt(const cantrace_event_t *e)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);

    char topic[CFG_TOPIC_MAX + 16];
    snprintf(topic, sizeof(topic), "%s/trace", cfg.base_topic);

    char payload[224];
    int o;
    if (e->kind == CANTRACE_EV_NEW_ID) {
        o = snprintf(payload, sizeof(payload),
                     "{\"event\": \"newId\", \"canId\": \"0x%03X\", "
                     "\"atMs\": %u, \"data\": \"",
                     e->can_id, (unsigned)(e->us / 1000));
    } else if (e->kind == CANTRACE_EV_BYTE_CHANGE) {
        o = snprintf(payload, sizeof(payload),
                     "{\"event\": \"byteChange\", \"canId\": \"0x%03X\", "
                     "\"byte\": %u, \"was\": %u, \"now\": %u, "
                     "\"atMs\": %u, \"data\": \"",
                     e->can_id, e->byte_index, e->was, e->now,
                     (unsigned)(e->us / 1000));
    } else if (e->kind == CANTRACE_EV_CONTROL) {
        o = snprintf(payload, sizeof(payload),
                     "{\"event\": \"control\", \"did\": %u, "
                     "\"atMs\": %u, \"data\": \"",
                     e->did, (unsigned)(e->us / 1000));
    } else {
        o = snprintf(payload, sizeof(payload),
                     "{\"event\": \"write\", \"canId\": \"0x%03X\", \"did\": %u, "
                     "\"atMs\": %u, \"data\": \"",
                     e->can_id, e->did, (unsigned)(e->us / 1000));
    }
    for (uint8_t i = 0; i < e->dlc && o < (int)sizeof(payload) - 4; i++) {
        o += snprintf(payload + o, sizeof(payload) - o, "%02X", e->data[i]);
    }
    snprintf(payload + o, sizeof(payload) - o, "\"}");

    ESP_LOGW(TAG, "trace trigger on 0x%03X (%s)", e->can_id,
             e->kind == CANTRACE_EV_NEW_ID ? "new identifier"
             : e->kind == CANTRACE_EV_BYTE_CHANGE ? "byte changed" : "UDS write");
    mqtt_pub_raw(topic, payload, false);
}

void app_main(void)
{
    ESP_LOGI(TAG, "open3e gateway starting");

    if (!app_config_init()) {
        /* Without NVS and the filesystem there is nothing to serve and nothing
         * to remember; keep going so the log is reachable over USB, but say so
         * loudly. */
        ESP_LOGE(TAG, "configuration storage unavailable - reflash the storage partition");
    }

    sys_cfg_t sys;
    sys_cfg_get(&sys);
    /* open3e's O3EUtc and O3EDateTime codecs render local time, so the zone has
     * to be set before any datapoint is decoded. */
    setenv("TZ", sys.tz, 1);
    tzset();

    if (o3e_db_open(CFG_DB_PATH)) {
        ESP_LOGI(TAG, "datapoint database %s, %u datapoints",
                 o3e_db_version(), (unsigned)o3e_db_count());
    } else {
        ESP_LOGE(TAG, "no datapoint database at %s - run 'idf.py storage-flash'",
                 CFG_DB_PATH);
    }

    if (!can_port_start()) {
        ESP_LOGE(TAG, "CAN interface did not come up");
    }

    net_prov_start();
    httpd_api_start();

    /* In setup mode there is no network to publish to and no point polling the
     * bus; the device is there to be configured. */
    if (!net_prov_is_setup_mode()) {
        mqtt_pub_start();
        poller_start();
        /* Passive: this only installs a receive filter, it never transmits. */
        if (sys.em380_enabled) {
            em380_start();
        }
        if (sys.collect_enabled) {
            uint16_t ids[COLLECT_MAX_IDS];
            size_t n = collect_parse_ids(sys.collect_canids, ids, COLLECT_MAX_IDS);
            if (n) {
                collect_start(ids, n);
            }
        }
        if (sys.raw_canids[0]) {
            uint16_t ids[RAW_RELAY_MAX_IDS];
            size_t n = collect_parse_ids(sys.raw_canids, ids, RAW_RELAY_MAX_IDS);
            if (n) {
                raw_relay_start(ids, n);
            }
        }
    }

    /* An unattended capture is only useful if it says when it fired: the
     * trigger it waits for can be hours away and comes from outside. */
    cantrace_on_trigger(trace_trigger_to_mqtt);

    /* If this boot is a freshly flashed image, it is on trial: the bootloader
     * reverts to the previous one unless the health check confirms it. */
    ota_arm_health_check();

    ESP_LOGI(TAG, "ready");
}
