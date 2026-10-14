#include "collect.h"
#include "collect_parse.h"

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
#include "cantrace.h"
#include "hold.h"
#include "mqtt_pub.h"
#include "o3e_codec.h"
#include "o3e_db.h"

static const char *TAG = "collect";

/* One slot per identifier seen, holding the compiled codec and the last
 * decoded value.
 *
 * Both are produced once, when a message arrives, rather than on every request
 * for the live view. Compiling a codec means reading the description from
 * flash and parsing several kilobytes of JSON; doing that for every identifier
 * on every status poll put that work in the HTTP task, where it delays every
 * other request behind it. */
typedef struct {
    uint16_t    did;
    uint16_t    len;
    uint32_t    count;
    uint32_t    last_ms;
    o3e_node_t *codec;
    char       *json;
} slot_t;

static QueueHandle_t     msg_q;
static SemaphoreHandle_t entries_lock;
static slot_t            entries[COLLECT_MAX_DIDS];
static uint16_t          n_entries;
static collect_stats_t   stats;
static volatile bool     running;
static uint16_t          listen_ids[COLLECT_MAX_IDS];
static uint8_t           n_listen;

/* Reassembly state, one per channel, touched only from the receive interrupt.
 * Sharing a single state across channels would interleave their segments. */
static collect_asm_t asm_states[COLLECT_MAX_IDS];

static inline int IRAM_ATTR channel_of(uint32_t id)
{
    for (uint8_t i = 0; i < n_listen; i++) {
        if (listen_ids[i] == id) {
            return i;
        }
    }
    return -1;
}

/* Which identifiers the user selected for MQTT, and their compiled codecs. */
typedef struct {
    uint16_t    did;
    o3e_node_t *codec;
    char        topic[192];
    pub_mode_t  mode;
    uint32_t    min_interval_ms;
    uint32_t    last_pub_ms;
} sel_t;

static sel_t  selected[COLLECT_MAX_DIDS];
static uint16_t n_selected;

/* The datapoints through which the manufacturer's backend steers the storage.
 *
 * Read off a real Vitocharge bus: over 255 seconds the gateway broadcast 2239
 * and 2188 every ten seconds and 2226 at the same cadence, all three constant
 * throughout -- self-consumption mode with the grid connection held at zero.
 * 2225 is the only one of the four the database marks writable, and it did not
 * appear at all, which is what makes it the interesting one: whatever puts the
 * storage on grid power is likely to bring it out of hiding.
 *
 * These are published and reported on change no matter what the user selected
 * for MQTT, because the moment one of them moves is the whole point of running
 * this gateway on that bus. */
typedef struct {
    uint16_t did;
    bool     seen;
    uint16_t len;
    uint8_t  data[24];
} ctrl_t;

static ctrl_t controls[] = {
    { .did = 2188 },   /* PointOfCommonCouplingSetActivePowerTotal */
    { .did = 2225 },   /* ElectricEnergyStorageSetpoint (rw)       */
    { .did = 2226 },   /* ElectricEnergyStorageMaximum             */
    { .did = 2239 },   /* ElectricEnergyStorageControlMode         */
};

#define N_CONTROLS (sizeof(controls) / sizeof(controls[0]))

bool collect_is_control_did(uint16_t did)
{
    for (size_t i = 0; i < N_CONTROLS; i++) {
        if (controls[i].did == did) {
            return true;
        }
    }
    return false;
}

static bool on_frame(uint32_t id, const uint8_t *d, uint8_t dlc)
{
    int ch = channel_of(id);
    if (!msg_q || ch < 0) {
        return false;
    }
    collect_msg_t m;
    if (!collect_feed(&asm_states[ch], d, dlc, &m, &stats.incomplete)) {
        return false;
    }
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(msg_q, &m, &woken) != pdTRUE) {
        /* A message that was reassembled and then thrown away. Distinct from
         * `incomplete`, which never became a message at all -- the two have
         * different causes and only one of them is this device's own fault. */
        stats.dropped++;
    }
    return woken == pdTRUE;
}

static void remember(const collect_msg_t *m, uint32_t now)
{
    xSemaphoreTake(entries_lock, portMAX_DELAY);
    slot_t *e = NULL;
    for (uint16_t i = 0; i < n_entries; i++) {
        if (entries[i].did == m->did) {
            e = &entries[i];
            break;
        }
    }
    if (!e && n_entries < COLLECT_MAX_DIDS) {
        e = &entries[n_entries++];
        memset(e, 0, sizeof(*e));
        e->did = m->did;
    }
    if (e) {
        /* The codec is compiled once per identifier and kept. A changed
         * payload length means a different variant, so it is rebuilt then. */
        if (!e->codec || e->len != m->len) {
            o3e_codec_free(e->codec);
            char *desc = o3e_db_json(m->did, m->len);
            e->codec = desc ? o3e_codec_compile(desc) : NULL;
            free(desc);
            if (!e->codec) {
                e->codec = o3e_codec_raw(m->len, "Raw");
            }
            e->len = m->len;
        }
        free(e->json);
        e->json = e->codec ? o3e_codec_decode_json(e->codec, m->data, m->len) : NULL;
        e->count++;
        e->last_ms = now;
    }
    stats.n_dids = n_entries;
    xSemaphoreGive(entries_lock);
}

/* Publish a control datapoint the first time it is seen and on every change,
 * and freeze the trace if it is armed for this.
 *
 * Only changes are published: the gateway repeats these values every ten
 * seconds, so publishing each repetition would bury the one message that
 * matters under thousands that do not, both in the broker and in whatever
 * graph is watching. Retained, so a subscriber that connects later still
 * learns the current state. */
static void watch_control(const collect_msg_t *m, const char *json)
{
    /* Before the change test, and regardless of it: the manager writes the
     * same value over and over, so nothing here changes, yet each of those
     * writes is exactly what a running hold has to answer. */
    hold_note_foreign(m->did);

    ctrl_t *c = NULL;
    for (size_t i = 0; i < N_CONTROLS; i++) {
        if (controls[i].did == m->did) {
            c = &controls[i];
            break;
        }
    }
    if (!c) {
        return;
    }
    uint16_t n = (m->len > sizeof(c->data)) ? (uint16_t)sizeof(c->data) : m->len;
    if (c->seen && c->len == m->len && memcmp(c->data, m->data, n) == 0) {
        return;
    }
    bool first = !c->seen;
    c->seen = true;
    c->len = m->len;
    memcpy(c->data, m->data, n);

    const o3e_dp_entry_t *e = o3e_db_find(m->did);
    const char *name = e ? o3e_db_name(e) : NULL;

    char hex[2 * sizeof(c->data) + 1];
    for (uint16_t i = 0; i < n; i++) {
        snprintf(hex + 2 * i, 3, "%02x", c->data[i]);
    }
    hex[2 * n] = '\0';

    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    char topic[CFG_TOPIC_MAX + 64];
    if (name) {
        snprintf(topic, sizeof(topic), "%s/control/%s", cfg.base_topic, name);
    } else {
        snprintf(topic, sizeof(topic), "%s/control/%u", cfg.base_topic, m->did);
    }

    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"did\": %u, \"name\": \"%s\", \"raw\": \"%s\", "
             "\"first\": %s, \"value\": %s}",
             m->did, name ? name : "", hex, first ? "true" : "false",
             (json && json[0]) ? json : "null");
    mqtt_pub_raw(topic, payload, true);

    ESP_LOGW(TAG, "control %u (%s) %s: %s",
             m->did, name ? name : "?", first ? "first seen" : "CHANGED", hex);

    if (!first) {
        cantrace_note_control(0, m->did, c->data, (uint8_t)n);
    }
}

static void collect_task(void *arg)
{
    (void)arg;
    collect_msg_t m;
    while (running) {
        if (xQueueReceive(msg_q, &m, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        stats.messages++;
        stats.last_ms = now;
        remember(&m, now);

        /* The decoded value belongs to the slot remember() just refreshed.
         * Only this task ever writes those slots, so reading one back is safe
         * once the lock has been let go -- and letting it go before publishing
         * keeps the web interface's status poll from waiting on the broker. */
        xSemaphoreTake(entries_lock, portMAX_DELAY);
        const char *json = NULL;
        for (uint16_t i = 0; i < n_entries; i++) {
            if (entries[i].did == m.did) {
                json = entries[i].json;
                break;
            }
        }
        xSemaphoreGive(entries_lock);
        watch_control(&m, json);

        xSemaphoreTake(entries_lock, portMAX_DELAY);
        for (uint16_t i = 0; i < n_selected; i++) {
            if (selected[i].did != m.did || !selected[i].codec) {
                continue;
            }
            if (selected[i].last_pub_ms &&
                (uint32_t)(now - selected[i].last_pub_ms) < selected[i].min_interval_ms) {
                break;
            }
            if (mqtt_pub_value(selected[i].topic, selected[i].codec,
                               m.data, m.len, selected[i].mode)) {
                stats.published++;
            }
            selected[i].last_pub_ms = now;
            break;
        }
        xSemaphoreGive(entries_lock);
    }
    vTaskDelete(NULL);
}

void collect_reload(void)
{
    if (!entries_lock) {
        entries_lock = xSemaphoreCreateMutex();
        if (!entries_lock) {
            return;
        }
    }

    char *raw = app_config_read_file(CFG_POINTS_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);

    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);

    xSemaphoreTake(entries_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < n_selected; i++) {
        o3e_codec_free(selected[i].codec);
    }
    memset(selected, 0, sizeof(selected));
    n_selected = 0;

    const cJSON *it;
    cJSON_ArrayForEach(it, root) {
        const char *type = sel_str(it, "type");
        if (!type || strcmp(type, "collect") != 0 || !sel_enabled(it)) {
            continue;
        }
        if (n_selected >= COLLECT_MAX_DIDS) {
            break;
        }
        uint16_t did = sel_u16(it, "did", 0);
        uint16_t dlen = sel_u16(it, "len", 0);
        char *json = o3e_db_json(did, dlen);
        o3e_node_t *codec = json ? o3e_codec_compile(json) : NULL;
        free(json);
        if (!codec && dlen) {
            codec = o3e_codec_raw(dlen, "Raw");
        }
        if (!codec) {
            continue;
        }
        sel_t *s = &selected[n_selected++];
        s->did = did;
        s->codec = codec;
        uint32_t iv = sel_u32(it, "interval", 10);
        s->min_interval_ms = (iv >= 1 ? iv : 10) * 1000;
        const char *mode = sel_str(it, "mode");
        s->mode = (mode && !strcmp(mode, "flat")) ? PUB_MODE_FLAT : PUB_MODE_JSON;

        const char *topic = sel_str(it, "topic");
        /* Sized so base topic plus suffix always fits the slot's topic field;
         * a truncated topic would publish where nobody is subscribed. */
        char suffix[sizeof(s->topic) - CFG_TOPIC_MAX - 2];
        if (topic && topic[0]) {
            snprintf(suffix, sizeof(suffix), "%s", topic);
        } else {
            snprintf(suffix, sizeof(suffix), "collect/%s",
                     codec->id ? codec->id : "Datapoint");
        }
        snprintf(s->topic, sizeof(s->topic), "%s/%s", cfg.base_topic, suffix);
    }
    xSemaphoreGive(entries_lock);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%u broadcast datapoint(s) selected", n_selected);
}

size_t collect_parse_ids(const char *text, uint16_t *out, size_t max)
{
    size_t n = 0;
    const char *p = text;
    while (p && *p && n < max) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *end;
        long v = strtol(p, &end, 0);
        if (end == p) {
            break;
        }
        if (v > 0 && v <= 0x7FF) {
            out[n++] = (uint16_t)v;
        }
        p = end;
    }
    return n;
}

bool collect_start(const uint16_t *ids, size_t n)
{
    if (n == 0 || n > COLLECT_MAX_IDS) {
        return false;
    }
    if (running) {
        collect_stop();
    }
    if (!entries_lock) {
        entries_lock = xSemaphoreCreateMutex();
        if (!entries_lock) {
            return false;
        }
    }
    msg_q = xQueueCreate(8, sizeof(collect_msg_t));
    if (!msg_q) {
        return false;
    }
    n_listen = (uint8_t)n;
    memcpy(listen_ids, ids, n * sizeof(*ids));
    running = true;
    stats.enabled = true;
    stats.n_ids = (uint8_t)n;
    memcpy(stats.can_ids, ids, n * sizeof(*ids));
    memset(asm_states, 0, sizeof(asm_states));

    /* Below the poller: decoding a broadcast must not delay a bus exchange. */
    if (xTaskCreate(collect_task, "collect", 6144, NULL, 3, NULL) != pdPASS) {
        running = false;
        stats.enabled = false;
        vQueueDelete(msg_q);
        msg_q = NULL;
        return false;
    }
    collect_reload();
    /* Listen on exactly the configured identifiers. A range listener
     * (can_port_add_listener()) would register [min(ids), max(ids)] and, per
     * on_rx_done()'s dispatch, swallow every frame in that span before it
     * ever reaches the generic rx_q - including unrelated UDS request/
     * response traffic to any ECU whose address happens to fall numerically
     * between two configured collect IDs. on_frame() would discard such a
     * frame anyway (channel_of() returns -1), but by then it is already lost
     * to the ISO-TP layer, which starves out as a spurious "no response".
     * can_port_add_id_listener() still prefilters by [min(ids), max(ids)]
     * internally, but additionally matches each frame's ID against the exact
     * list before claiming it, so traffic that merely lies in between passes
     * through untouched. */
    can_port_add_id_listener(ids, n, on_frame);
    for (size_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "listening on CAN-ID 0x%03X", ids[i]);
    }
    return true;
}

void collect_stop(void)
{
    if (!running) {
        return;
    }
    can_port_remove_listener(on_frame);
    running = false;
    stats.enabled = false;
    vTaskDelay(pdMS_TO_TICKS(600));
    xSemaphoreTake(entries_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < n_selected; i++) {
        o3e_codec_free(selected[i].codec);
    }
    memset(selected, 0, sizeof(selected));
    n_selected = 0;
    for (uint16_t i = 0; i < n_entries; i++) {
        o3e_codec_free(entries[i].codec);
        free(entries[i].json);
    }
    memset(entries, 0, sizeof(entries));
    n_entries = 0;
    xSemaphoreGive(entries_lock);
    vQueueDelete(msg_q);
    msg_q = NULL;
}

void collect_stats(collect_stats_t *out) { *out = stats; }

size_t collect_entries(collect_entry_t *out, size_t max)
{
    if (!entries_lock) {
        return 0;
    }
    xSemaphoreTake(entries_lock, portMAX_DELAY);
    size_t n = n_entries < max ? n_entries : max;
    for (size_t i = 0; i < n; i++) {
        out[i].did = entries[i].did;
        out[i].len = entries[i].len;
        out[i].count = entries[i].count;
        out[i].last_ms = entries[i].last_ms;
        /* Copied, not borrowed. The decode task frees and replaces both of
         * these on every message that arrives -- eight a second on a
         * Vitocharge bus -- so a pointer handed out under the lock is dangling
         * by the time the caller has finished formatting with it. That is a
         * use-after-free, and it read as the device rebooting whenever the
         * page was opened. em380.c had it right; this did not. */
        const char *nm = entries[i].codec ? entries[i].codec->id : NULL;
        out[i].name = nm ? strdup(nm) : NULL;
        out[i].json = entries[i].json ? strdup(entries[i].json) : NULL;
    }
    xSemaphoreGive(entries_lock);
    return n;
}

void collect_entries_free(collect_entry_t *e, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free((char *)e[i].name);
        free((char *)e[i].json);
        e[i].name = e[i].json = NULL;
    }
}
