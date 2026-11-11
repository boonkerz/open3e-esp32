#include "em380.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "can_port.h"
#include "mqtt_pub.h"
#include "o3e_codec.h"
#include "o3e_db.h"

static const char *TAG = "e380";

#define EM380_N_IDS (EM380_CAN_LAST - EM380_CAN_FIRST + 1)

typedef struct {
    uint16_t can_id;
    uint8_t  data[8];
    uint8_t  len;
} em_frame_t;

typedef struct {
    o3e_node_t *codec;
    char        topic[192];
    pub_mode_t  mode;
    bool        enabled;
    uint32_t    min_interval_ms;
    uint32_t    last_pub_ms;
    uint8_t     last[8];
    uint8_t     last_len;
    bool        have_last;
    uint32_t    published;
    /* Decoded once when the frame arrives. The live view used to compile a
     * codec per frame on every status poll, which put a flash read and a JSON
     * parse per frame into the HTTP task. */
    char       *json;
} em_slot_t;

static em_slot_t     slots[EM380_N_IDS];
/* Guards the compiled codec trees in `slots`.
 *
 * Three contexts touch them: this module's decoding task, em380_reload() from
 * whichever task saved the selection (including the scan task, which reloads
 * when it finishes), and the web UI's live view. Rebuilding frees the trees
 * the decoding task may be walking at that moment, so the rebuild and every
 * use of a tree have to exclude each other. */
static SemaphoreHandle_t slots_lock;
static QueueHandle_t frame_q;
static em380_stats_t stats;
static volatile bool running;

static inline int slot_of(uint16_t can_id)
{
    if (can_id < EM380_CAN_FIRST || can_id > EM380_CAN_LAST) {
        return -1;
    }
    return can_id - EM380_CAN_FIRST;
}

/* Interrupt context: copy the frame into a queue and report whether that woke
 * the decoding task. Yielding here instead would cut the TWAI driver's own
 * interrupt handling short. */
static bool on_frame(uint32_t id, const uint8_t *data, uint8_t len)
{
    if (!frame_q) {
        return false;
    }
    em_frame_t f = { .can_id = (uint16_t)id, .len = len > 8 ? 8 : len };
    memcpy(f.data, data, f.len);

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(frame_q, &f, &woken) != pdTRUE) {
        stats.dropped++;
    }
    return woken == pdTRUE;
}

static void free_slots(void)
{
    for (int i = 0; i < EM380_N_IDS; i++) {
        o3e_codec_free(slots[i].codec);
        slots[i].codec = NULL;
        slots[i].enabled = false;
        free(slots[i].json);
        slots[i].json = NULL;
    }
}

void em380_reload(void)
{
    if (!slots_lock) {
        slots_lock = xSemaphoreCreateMutex();
        if (!slots_lock) {
            return;
        }
    }

    char *raw = app_config_read_file(CFG_POINTS_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        xSemaphoreTake(slots_lock, portMAX_DELAY);
        free_slots();
        xSemaphoreGive(slots_lock);
        return;
    }

    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);

    xSemaphoreTake(slots_lock, portMAX_DELAY);
    free_slots();

    int active = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, root) {
        /* Energy-meter entries are marked with a type so they can share the
         * selection file with the polled datapoints. */
        if (!sel_is_em380(it) || !sel_enabled(it)) {
            continue;
        }
        uint16_t can_id = sel_u16(it, "canId", 0);
        int s = slot_of(can_id);
        if (s < 0) {
            continue;
        }

        char *json = o3e_db_em_json(can_id);
        slots[s].codec = json ? o3e_codec_compile(json) : NULL;
        free(json);
        if (!slots[s].codec) {
            ESP_LOGW(TAG, "no codec for CAN-ID 0x%03X", can_id);
            continue;
        }

        const char *mode = sel_str(it, "mode");
        slots[s].mode = (mode && !strcmp(mode, "flat")) ? PUB_MODE_FLAT : PUB_MODE_JSON;

        /* The meter broadcasts several times a second; publishing every frame
         * would swamp the broker for no gain. The interval is a floor, not a
         * schedule -- nothing is ever sent that was not received. */
        const cJSON *jiv = cJSON_GetObjectItem(it, "interval");
        slots[s].min_interval_ms = cJSON_IsNumber(jiv) && jiv->valuedouble >= 1
                                 ? (uint32_t)(jiv->valuedouble * 1000) : 10000;

        const cJSON *jtopic = cJSON_GetObjectItem(it, "topic");
        /* Sized so base topic plus suffix always fits the slot's topic field;
         * a truncated topic would publish somewhere nobody is subscribed. */
        char suffix[sizeof(slots[s].topic) - CFG_TOPIC_MAX - 2];
        if (cJSON_IsString(jtopic) && jtopic->valuestring[0]) {
            snprintf(suffix, sizeof(suffix), "%s", jtopic->valuestring);
        } else {
            /* Distinct from the polled datapoints: the same name can exist in
             * both namespaces, and a meter reading is not a heat pump reading. */
            snprintf(suffix, sizeof(suffix), "E380/%s",
                     slots[s].codec->id ? slots[s].codec->id : "Frame");
        }
        snprintf(slots[s].topic, sizeof(slots[s].topic), "%s/%s", cfg.base_topic, suffix);

        slots[s].enabled = true;
        active++;
    }
    xSemaphoreGive(slots_lock);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%d broadcast frame(s) active", active);
}

static void em380_task(void *arg)
{
    (void)arg;
    em_frame_t f;
    while (running) {
        if (xQueueReceive(frame_q, &f, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }
        int s = slot_of(f.can_id);
        if (s < 0) {
            continue;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        stats.frames++;
        stats.seen = true;
        stats.last_seen_ms = now;
        stats.ids_seen |= (uint16_t)(1u << s);

        if (!slots_lock) {
            continue;
        }
        xSemaphoreTake(slots_lock, portMAX_DELAY);

        memcpy(slots[s].last, f.data, f.len);
        slots[s].last_len = f.len;
        slots[s].have_last = true;

        /* Decode here, once per frame, rather than per request for the live
         * view. The codec for an unselected frame is compiled on first sight
         * and kept. */
        if (!slots[s].codec) {
            char *desc = o3e_db_em_json(f.can_id);
            slots[s].codec = desc ? o3e_codec_compile(desc) : NULL;
            free(desc);
        }
        if (slots[s].codec) {
            free(slots[s].json);
            slots[s].json = o3e_codec_decode_json(slots[s].codec, f.data, f.len);
        }

        bool due = slots[s].enabled && slots[s].codec &&
                   (slots[s].last_pub_ms == 0 ||
                    (uint32_t)(now - slots[s].last_pub_ms) >= slots[s].min_interval_ms);
        if (due) {
            if (mqtt_pub_value(slots[s].topic, slots[s].codec, f.data, f.len,
                               slots[s].mode, false)) {
                slots[s].published++;
                stats.published++;
            }
            slots[s].last_pub_ms = now;
        }
        xSemaphoreGive(slots_lock);
    }
    vTaskDelete(NULL);
}

bool em380_start(void)
{
    if (running) {
        return true;
    }
    if (o3e_db_em_count() == 0) {
        ESP_LOGW(TAG, "database carries no energy-meter frames");
        return false;
    }

    if (!slots_lock) {
        slots_lock = xSemaphoreCreateMutex();
        if (!slots_lock) {
            return false;
        }
    }
    frame_q = xQueueCreate(32, sizeof(em_frame_t));
    if (!frame_q) {
        return false;
    }
    running = true;
    stats.enabled = true;
    /* Below the poller and the scan: decoding a broadcast must never delay an
     * ISO-TP exchange, whose inter-frame timing is the tighter constraint. */
    if (xTaskCreate(em380_task, "e380", 4096, NULL, 3, NULL) != pdPASS) {
        running = false;
        stats.enabled = false;
        vQueueDelete(frame_q);
        frame_q = NULL;
        return false;
    }

    em380_reload();
    can_port_add_listener(EM380_CAN_FIRST, EM380_CAN_LAST, on_frame);
    ESP_LOGI(TAG, "listening on CAN-IDs 0x%03X..0x%03X", EM380_CAN_FIRST, EM380_CAN_LAST);
    return true;
}

void em380_stop(void)
{
    if (!running) {
        return;
    }
    can_port_remove_listener(on_frame);
    running = false;
    stats.enabled = false;
    vTaskDelay(pdMS_TO_TICKS(600));
    xSemaphoreTake(slots_lock, portMAX_DELAY);
    free_slots();
    xSemaphoreGive(slots_lock);
    vQueueDelete(frame_q);
    frame_q = NULL;
}

void em380_stats(em380_stats_t *out) { *out = stats; }

char *em380_last_json(uint16_t can_id)
{
    int s = slot_of(can_id);
    if (s < 0 || !slots_lock) {
        return NULL;
    }
    xSemaphoreTake(slots_lock, portMAX_DELAY);
    /* A copy, because the caller formats it after releasing the lock and the
     * decoding task may replace the stored one at any moment. */
    char *out = slots[s].json ? strdup(slots[s].json) : NULL;
    xSemaphoreGive(slots_lock);
    return out;
}
