#include "raw_relay.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "can_port.h"
#include "mqtt_pub.h"

static const char *TAG = "rawrelay";

typedef struct {
    uint16_t can_id;
    uint8_t  data[8];
    uint8_t  len;
} raw_frame_t;

static uint16_t          listen_ids[RAW_RELAY_MAX_IDS];
static uint8_t           n_listen;
static QueueHandle_t     frame_q;
static raw_relay_stats_t stats;
static volatile bool     running;

static bool wanted(uint16_t id)
{
    for (uint8_t i = 0; i < n_listen; i++) {
        if (listen_ids[i] == id) {
            return true;
        }
    }
    return false;
}

/* Interrupt context: copy the frame into a queue and report whether that woke
 * the relay task. Yielding here instead would cut the TWAI driver's own
 * interrupt handling short - see em380.c's on_frame() for the same rule. */
static bool on_frame(uint32_t id, const uint8_t *data, uint8_t len)
{
    if (!frame_q || !wanted((uint16_t)id)) {
        return false;
    }
    raw_frame_t f = { .can_id = (uint16_t)id, .len = len > 8 ? 8 : len };
    memcpy(f.data, data, f.len);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(frame_q, &f, &woken);
    return woken == pdTRUE;
}

static void raw_relay_task(void *arg)
{
    (void)arg;
    raw_frame_t f;
    while (running) {
        if (xQueueReceive(frame_q, &f, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }
        stats.frames++;

        mqtt_cfg_t cfg;
        mqtt_cfg_get(&cfg);
        char topic[CFG_TOPIC_MAX + 16];
        snprintf(topic, sizeof(topic), "%s/raw/%03x", cfg.base_topic, f.can_id);

        char hex[17];
        for (uint8_t i = 0; i < f.len; i++) {
            snprintf(hex + i * 2, 3, "%02x", f.data[i]);
        }
        hex[f.len * 2] = '\0';

        /* Real wall-clock time when SNTP has synced, 0 otherwise - a consumer
         * decoding these itself only uses this to order/deduplicate frames,
         * not as a source of truth by itself. */
        char payload[80];
        snprintf(payload, sizeof(payload), "{\"dlc\": %u, \"data\": \"%s\", \"ts\": %llu}",
                 f.len, hex, (unsigned long long)time(NULL) * 1000ULL);

        if (mqtt_pub_raw(topic, payload, false)) {
            stats.published++;
        }
    }
    vTaskDelete(NULL);
}

bool raw_relay_start(const uint16_t *ids, size_t n)
{
    if (n == 0 || n > RAW_RELAY_MAX_IDS) {
        return false;
    }
    if (running) {
        raw_relay_stop();
    }
    frame_q = xQueueCreate(16, sizeof(raw_frame_t));
    if (!frame_q) {
        return false;
    }
    n_listen = (uint8_t)n;
    memcpy(listen_ids, ids, n * sizeof(*ids));
    memset(&stats, 0, sizeof(stats));
    stats.enabled = true;
    stats.n_ids = (uint8_t)n;
    memcpy(stats.can_ids, ids, n * sizeof(*ids));
    running = true;

    /* Below the poller and the scan: relaying a broadcast must never delay an
     * ISO-TP exchange. */
    if (xTaskCreate(raw_relay_task, "rawrelay", 4096, NULL, 3, NULL) != pdPASS) {
        running = false;
        stats.enabled = false;
        vQueueDelete(frame_q);
        frame_q = NULL;
        return false;
    }

    /* Exact-ID listener, not a plain [min, max] range: unlike collect.c's
     * collect_start(), the configured IDs here are picked by whoever calls
     * this (ioBroker.e3oncan in practice) and are not known in advance to
     * avoid straddling some ECU's own request/response addresses. A plain
     * range listener would silently steal that ECU's UDS traffic away from
     * the ISO-TP path for every ID in the gap, not just the ones actually
     * wanted - can_port_add_id_listener() lets the gaps fall through
     * instead. Return value not checked, same as collect.c's
     * collect_start(): a full listener table only logs a warning, it is not
     * something this module can recover from differently. */
    can_port_add_id_listener(ids, n, on_frame);
    ESP_LOGI(TAG, "relaying %u CAN-ID(s) raw over MQTT", (unsigned)n);
    return true;
}

void raw_relay_stop(void)
{
    if (!running) {
        return;
    }
    can_port_remove_listener(on_frame);
    running = false;
    stats.enabled = false;
    vTaskDelay(pdMS_TO_TICKS(600));
    vQueueDelete(frame_q);
    frame_q = NULL;
}

void raw_relay_stats(raw_relay_stats_t *out) { *out = stats; }
