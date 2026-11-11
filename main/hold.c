#include "hold.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "cJSON.h"
#include "mqtt_pub.h"
#include "poller.h"

static const char *TAG = "hold";

/* Stop rather than hammer: if the storage has begun refusing the write, the
 * next fifty attempts will not go better, and a hold that cannot take effect
 * should end loudly instead of running as a lie on the status page. */
#define HOLD_MAX_FAILS 5

/* One per datapoint held, each with its own deadline, because the two are
 * independent: charging from the grid while the storage is otherwise idle is a
 * sensible combination, and so is neither. */
enum { SLOT_GRID = 0, SLOT_STORAGE, N_SLOTS };

typedef struct {
    bool     active;
    uint16_t ecu;
    uint16_t did;
    uint8_t  bytes[12];
    uint8_t  len;
    int64_t  deadline_us;
    uint32_t writes;
    uint32_t fails;
    char     last_error[96];
} slot_t;

static SemaphoreHandle_t lock;
static TaskHandle_t      task_h;
static slot_t            slots[N_SLOTS];
static int16_t           grid_watts;
static storage_mode_t    storage_mode;

/* The charge loop; all under `lock` except where noted. */
static uint16_t          charge_w;          /* 0: the grid setpoint is held as given */
static int16_t           charge_grid_w;
static int16_t           charge_storage_w;
static uint32_t          charge_adjusts;
static bool              charge_readings;
static int64_t           charge_next_us;    /* hold task only */

static void put_i32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* The datapoints are RawCodecs, so the value goes in as a hex string -- quotes
 * and all, because that is a JSON string. */
static void as_json(char *out, size_t out_sz, const uint8_t *b, uint8_t len)
{
    size_t o = 0;
    out[o++] = '"';
    for (uint8_t i = 0; i < len && o + 3 < out_sz; i++) {
        o += (size_t)snprintf(out + o, out_sz - o, "%02x", b[i]);
    }
    snprintf(out + o, out_sz - o, "\"");
}

static bool ensure_task(char *err, size_t err_sz);

/* Grid setpoint bytes for `w` watts, the shape grid_hold_start() writes. */
static void grid_bytes(uint8_t *b, int16_t w)
{
    b[0] = (uint8_t)((uint16_t)w & 0xFF);
    b[1] = (uint8_t)(((uint16_t)w >> 8) & 0xFF);
    put_i32(b + 2, HOLD_VALIDITY);
}

/* One decoded reading as a whole number of watts. `field` picks a member of
 * a complex datapoint, NULL takes a bare scalar. */
static bool read_watts(uint16_t ecu, uint16_t did, const char *field, int16_t *out)
{
    char err[64];
    char *json = poller_read_now(ecu, did, err, sizeof(err));
    if (!json) {
        ESP_LOGW(TAG, "0x%03X.%u: %s", ecu, did, err);
        return false;
    }
    cJSON *v = cJSON_Parse(json);
    free(json);
    if (!v) {
        return false;
    }
    const cJSON *n = field ? cJSON_GetObjectItemCaseSensitive(v, field) : v;
    bool ok = cJSON_IsNumber(n);
    if (ok) {
        double d = n->valuedouble;
        if (d > INT16_MAX) d = INT16_MAX;
        if (d < INT16_MIN) d = INT16_MIN;
        *out = (int16_t)d;
    }
    cJSON_Delete(v);
    return ok;
}

/* What the grid setpoint has to be for the storage to take `want` watts,
 * given the current readings: the house's net demand is whatever the grid
 * delivers beyond what the storage takes, and that demand plus the wanted
 * charge is the draw to ask for. Never feed-in, never over the cap. */
static int16_t charge_setpoint(int16_t grid_w, int16_t storage_w, uint16_t want)
{
    int32_t charging = -(int32_t)storage_w;          /* positive while charging */
    int32_t house    = (int32_t)grid_w - charging;   /* may be negative with PV */
    int32_t draw     = house + (int32_t)want;
    if (draw < 0) {
        draw = 0;
    }
    if (draw > GRID_HOLD_MAX_W) {
        draw = GRID_HOLD_MAX_W;
    }
    return (int16_t)-draw;
}

/* Hold task only. Reads both powers and moves the grid setpoint towards the
 * wanted charge, at most CHARGE_STEP_MAX_W at a time. A failed reading
 * leaves the setpoint where it is: a known value beats a guess. */
static void charge_adjust(uint16_t ecu)
{
    int16_t g = 0, s = 0;
    bool ok = read_watts(ecu, CHARGE_DID_GRID, "ActivePower", &g) &&
              read_watts(ecu, CHARGE_DID_STORAGE, NULL, &s);
    xSemaphoreTake(lock, portMAX_DELAY);
    charge_readings = ok;
    if (!ok || !charge_w || !slots[SLOT_GRID].active) {
        xSemaphoreGive(lock);
        return;
    }
    charge_grid_w = g;
    charge_storage_w = s;
    int32_t target = charge_setpoint(g, s, charge_w);
    int32_t cur = grid_watts;
    int32_t next = target;
    if (next < cur - CHARGE_STEP_MAX_W) next = cur - CHARGE_STEP_MAX_W;
    if (next > cur + CHARGE_STEP_MAX_W) next = cur + CHARGE_STEP_MAX_W;
    bool change = (next > cur ? next - cur : cur - next) >= CHARGE_DEADBAND_W;
    if (change) {
        grid_watts = (int16_t)next;
        grid_bytes(slots[SLOT_GRID].bytes, grid_watts);
        charge_adjusts++;
    }
    xSemaphoreGive(lock);
    if (change) {
        ESP_LOGI(TAG, "charge loop: grid %d W, storage %d W, want %u W -> setpoint %d W",
                 g, s, (unsigned)charge_w, (int)next);
        hold_publish();
    }
}

static bool slot_start(int idx, uint16_t ecu, uint16_t did,
                       const uint8_t *bytes, uint8_t len, uint32_t seconds,
                       char *err, size_t err_sz)
{
    if (seconds == 0 || seconds > GRID_HOLD_MAX_S) {
        snprintf(err, err_sz, "the duration must be between 1 and %d seconds",
                 GRID_HOLD_MAX_S);
        return false;
    }
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    if (!sys.write_enabled) {
        snprintf(err, err_sz, "writing is disabled in the system settings");
        return false;
    }
    if (!ensure_task(err, err_sz)) {
        return false;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    slot_t *s = &slots[idx];
    s->ecu = ecu;
    s->did = did;
    s->len = len;
    memcpy(s->bytes, bytes, len);
    s->deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s->writes = s->fails = 0;
    s->last_error[0] = '\0';
    s->active = true;
    xSemaphoreGive(lock);

    ESP_LOGW(TAG, "holding 0x%03X.%u for %u s", ecu, did, (unsigned)seconds);
    hold_publish();
    return true;
}

static void slot_stop(int idx)
{
    /* Ohne Mutex hat nie ein Hold begonnen, also gibt es auch nichts zu
     * beenden. Diese Prüfung fehlte, und das war kein theoretischer Fall:
     * xSemaphoreTake(NULL) ist in FreeRTOS ein configASSERT und damit ein
     * Panic. Erreichbar über {"mode":"grid","stop":true} auf dem
     * Kommando-Topic und über POST /api/grid -- also von jedem, der den
     * Broker erreicht, auf einem frisch gestarteten Gerät. Die erzeugte
     * evcc-Konfiguration schickt genau das für batterymode "normal". */
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool was = slots[idx].active;
    slots[idx].active = false;
    xSemaphoreGive(lock);
    if (was) {
        ESP_LOGI(TAG, "hold on %u stopped after %u writes",
                 slots[idx].did, (unsigned)slots[idx].writes);
        hold_publish();
    }
}

static void hold_task(void *arg)
{
    (void)arg;
    uint32_t turns = 0;
    for (;;) {
        /* Woken either by the heartbeat or, sooner, by the manager writing one
         * of the held datapoints. The timeout is the floor, not the cadence. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HOLD_PERIOD_MS));
        bool any = false;

        for (int i = 0; i < N_SLOTS; i++) {
            xSemaphoreTake(lock, portMAX_DELAY);
            slot_t s = slots[i];
            bool run = s.active && esp_timer_get_time() < s.deadline_us;
            bool expired = s.active && !run;
            if (expired) {
                slots[i].active = false;
            }
            xSemaphoreGive(lock);

            if (expired) {
                ESP_LOGI(TAG, "hold on %u finished after %u writes",
                         s.did, (unsigned)s.writes);
                hold_publish();
                continue;
            }
            if (!run) {
                continue;
            }
            any = true;

            char json[40];
            as_json(json, sizeof(json), s.bytes, s.len);
            char err[96] = "";
            /* Forced: the database marks both datapoints read-only, which the
             * device does not -- it acknowledges the write and acts on it. */
            if (poller_write_now(s.ecu, s.did, json, true, err, sizeof(err))) {
                xSemaphoreTake(lock, portMAX_DELAY);
                slots[i].writes++;
                xSemaphoreGive(lock);
            } else {
                xSemaphoreTake(lock, portMAX_DELAY);
                slots[i].fails++;
                snprintf(slots[i].last_error, sizeof(slots[i].last_error),
                         "%s", err);
                bool give_up = slots[i].fails >= HOLD_MAX_FAILS;
                if (give_up) {
                    slots[i].active = false;
                }
                xSemaphoreGive(lock);
                ESP_LOGW(TAG, "write to %u failed: %s", s.did, err);
                if (give_up) {
                    ESP_LOGE(TAG, "giving up on %u", s.did);
                }
                hold_publish();
            }
        }

        /* The charge loop rides on the grid slot: once that is gone, so is
         * the loop; while it runs, the setpoint is re-derived every period. */
        xSemaphoreTake(lock, portMAX_DELAY);
        bool loop = charge_w && slots[SLOT_GRID].active;
        uint16_t loop_ecu = slots[SLOT_GRID].ecu;
        if (!slots[SLOT_GRID].active) {
            charge_w = 0;
        }
        xSemaphoreGive(lock);
        if (loop && esp_timer_get_time() >= charge_next_us) {
            charge_next_us = esp_timer_get_time() + (int64_t)CHARGE_PERIOD_MS * 1000;
            charge_adjust(loop_ecu);
        }

        /* Every fifth turn, so a countdown on a dashboard moves without a
         * message every two seconds. */
        if (any && ++turns % 5 == 0) {
            hold_publish();
        }
    }
}

static bool ensure_task(char *err, size_t err_sz)
{
    /* hold_init() legt den Mutex beim Start an; das hier bleibt als Rückfall
     * stehen, falls jemand das Modul ohne Initialisierung benutzt. Träge
     * erzeugt war er ursprünglich allein -- zwei Tasks hätten ihn dann
     * gleichzeitig anlegen können, und wer zuletzt schreibt, lässt den
     * anderen auf einem Mutex warten, den niemand mehr freigibt. */
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        if (!lock) {
            snprintf(err, err_sz, "out of memory");
            return false;
        }
    }
    /* The same call chain as the poll task -- UDS, ISO-TP, codec compile,
     * merge and encode -- plus hold_publish() on top, so the same stack as
     * the poll and command tasks. With 4096 the two holds together overflowed
     * it: crash report "task hold, stack corrupted", device gone until a
     * power cycle. */
    if (!task_h &&
        xTaskCreate(hold_task, "hold", 6144, NULL, 4, &task_h) != pdPASS) {
        snprintf(err, err_sz, "cannot start the hold task");
        return false;
    }
    return true;
}

void hold_note_foreign(uint16_t did)
{
    /* Called for every message the collect receiver decodes -- eight a second
     * on a Vitocharge bus -- so the two datapoints this could possibly concern
     * are ruled out before touching the lock the writer holds. */
    if ((did != GRID_HOLD_DID && did != STORAGE_HOLD_DID) || !lock || !task_h) {
        return;
    }
    bool ours = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < N_SLOTS; i++) {
        if (slots[i].active && slots[i].did == did) {
            ours = true;
            break;
        }
    }
    xSemaphoreGive(lock);
    if (ours) {
        xTaskNotifyGive(task_h);
    }
}

/* ------------------------------------------------------------------ */
/* the grid setpoint                                                    */

static bool grid_start(uint16_t ecu, int16_t w, uint32_t seconds,
                       uint16_t charge, char *err, size_t err_sz)
{
    if (w > GRID_HOLD_MAX_W || w < -GRID_HOLD_MAX_W) {
        snprintf(err, err_sz, "%d W is beyond the %d W limit", w, GRID_HOLD_MAX_W);
        return false;
    }
    uint8_t b[6];
    grid_bytes(b, w);
    if (lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
    }
    grid_watts = w;
    charge_w = charge;
    charge_adjusts = 0;
    charge_readings = false;
    if (lock) {
        xSemaphoreGive(lock);
    }
    return slot_start(SLOT_GRID, ecu, GRID_HOLD_DID, b, sizeof(b), seconds,
                      err, err_sz);
}

bool grid_hold_start(uint16_t ecu, int16_t w, uint32_t seconds,
                     char *err, size_t err_sz)
{
    return grid_start(ecu, w, seconds, 0, err, err_sz);
}

bool charge_hold_start(uint16_t ecu, uint16_t watts, uint32_t seconds,
                       char *err, size_t err_sz)
{
    if (watts == 0 || watts > GRID_HOLD_MAX_W) {
        snprintf(err, err_sz, "the charge power must be between 1 and %d W",
                 GRID_HOLD_MAX_W);
        return false;
    }
    /* Start from what the house takes right now; if that cannot be read,
     * start from the wanted power alone and let the loop find the rest. */
    int16_t g = 0, s = 0;
    int16_t w = -(int16_t)watts;
    bool ok = read_watts(ecu, CHARGE_DID_GRID, "ActivePower", &g) &&
              read_watts(ecu, CHARGE_DID_STORAGE, NULL, &s);
    if (ok) {
        w = charge_setpoint(g, s, watts);
    }
    if (!grid_start(ecu, w, seconds, watts, err, err_sz)) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    charge_grid_w = g;
    charge_storage_w = s;
    charge_readings = ok;
    xSemaphoreGive(lock);
    charge_next_us = esp_timer_get_time() + (int64_t)CHARGE_PERIOD_MS * 1000;
    ESP_LOGW(TAG, "charge loop: want %u W into the storage, grid %d W, storage %d W, "
             "starting at %d W", (unsigned)watts, g, s, w);
    hold_publish();
    return true;
}

void hold_init(void)
{
    if (!lock) {
        lock = xSemaphoreCreateMutex();
    }
}

void grid_hold_stop(void) { slot_stop(SLOT_GRID); }

bool grid_hold_switch(bool on, char *err, size_t err_sz)
{
    if (!on) {
        grid_hold_stop();
        return true;
    }
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    if (!sys.grid_ecu) {
        snprintf(err, err_sz, "no storage ECU configured for grid charging");
        return false;
    }
    /* The setting is positive and reads as "draw this much"; the datapoint
     * wants a negative number for the same thing. */
    return grid_hold_start(sys.grid_ecu, -(int16_t)sys.grid_watts,
                           (uint32_t)sys.grid_minutes * 60, err, err_sz);
}

void grid_hold_status(grid_hold_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const slot_t *s = &slots[SLOT_GRID];
    out->active = s->active;
    out->ecu = s->ecu;
    out->watts = grid_watts;
    int64_t left = s->deadline_us - esp_timer_get_time();
    out->remaining_s = (s->active && left > 0) ? (uint32_t)(left / 1000000) : 0;
    out->writes = s->writes;
    out->failures = s->fails;
    snprintf(out->last_error, sizeof(out->last_error), "%s", s->last_error);
    out->charge_w = s->active ? charge_w : 0;
    out->grid_w = charge_grid_w;
    out->storage_w = charge_storage_w;
    out->adjusts = charge_adjusts;
    out->readings = charge_readings;
    xSemaphoreGive(lock);
}

/* ------------------------------------------------------------------ */
/* the storage's own limits                                             */

static const struct { storage_mode_t mode; const char *name; } MODES[] = {
    { STORAGE_MODE_NORMAL,          "normal" },
    { STORAGE_MODE_IDLE,            "steht still" },
    { STORAGE_MODE_CHARGE_ONLY,     "nur laden" },
    { STORAGE_MODE_DISCHARGE_ONLY,  "nur entladen" },
};

const char *storage_mode_name(storage_mode_t mode)
{
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        if (MODES[i].mode == mode) {
            return MODES[i].name;
        }
    }
    return "normal";
}

bool storage_mode_parse(const char *name, storage_mode_t *out)
{
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        if (name && strcmp(MODES[i].name, name) == 0) {
            *out = MODES[i].mode;
            return true;
        }
    }
    return false;
}

bool storage_hold_start(uint16_t ecu, storage_mode_t mode, uint32_t seconds,
                        char *err, size_t err_sz)
{
    if (mode == STORAGE_MODE_NORMAL) {
        storage_mode = mode;
        slot_stop(SLOT_STORAGE);
        return true;
    }
    /* Only the extremes, so nothing here depends on knowing the scale. */
    int32_t charge = (mode == STORAGE_MODE_DISCHARGE_ONLY) ? 0 : HOLD_LIMIT_OPEN;
    int32_t discharge = (mode == STORAGE_MODE_CHARGE_ONLY) ? 0 : HOLD_LIMIT_OPEN;
    if (mode == STORAGE_MODE_IDLE) {
        charge = discharge = 0;
    }

    uint8_t b[12];
    put_i32(b + 0, charge);
    put_i32(b + 4, discharge);
    put_i32(b + 8, HOLD_VALIDITY);
    storage_mode = mode;
    return slot_start(SLOT_STORAGE, ecu, STORAGE_HOLD_DID, b, sizeof(b),
                      seconds, err, err_sz);
}

void storage_hold_status(storage_hold_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const slot_t *s = &slots[SLOT_STORAGE];
    out->active = s->active;
    out->mode = s->active ? storage_mode : STORAGE_MODE_NORMAL;
    int64_t left = s->deadline_us - esp_timer_get_time();
    out->remaining_s = (s->active && left > 0) ? (uint32_t)(left / 1000000) : 0;
    out->writes = s->writes;
    out->failures = s->fails;
    snprintf(out->last_error, sizeof(out->last_error), "%s", s->last_error);
    xSemaphoreGive(lock);
}

/* ------------------------------------------------------------------ */

void hold_publish(void)
{
    /* Called from the hold task, the HTTP task and the command task; the two
     * config copies and the payload are over a kilobyte, which no caller's
     * stack needs to carry. One shared buffer under its own lock instead. */
    static struct {
        mqtt_cfg_t mq;
        sys_cfg_t  sys;
        char       topic[CFG_TOPIC_MAX + 8];
        char       payload[400];
    } b;
    static SemaphoreHandle_t b_lock;
    if (!b_lock) {
        b_lock = xSemaphoreCreateMutex();
        if (!b_lock) {
            return;
        }
    }
    xSemaphoreTake(b_lock, portMAX_DELAY);
    mqtt_cfg_get(&b.mq);
    if (!b.mq.enabled || !mqtt_pub_connected()) {
        xSemaphoreGive(b_lock);
        return;
    }
    sys_cfg_get(&b.sys);

    grid_hold_status_t g;
    storage_hold_status_t s;
    grid_hold_status(&g);
    storage_hold_status(&s);

    snprintf(b.topic, sizeof(b.topic), "%s/hold", b.mq.base_topic);
    snprintf(b.payload, sizeof(b.payload),
             "{\"active\": %s, \"power\": %u, \"minutes\": %u, "
             "\"setpoint\": %d, \"charge\": %u, \"gridPower\": %d, "
             "\"storagePower\": %d, \"adjusts\": %u, "
             "\"remainingS\": %u, \"writes\": %u, \"failures\": %u, "
             "\"storage\": \"%s\", \"storageRemainingS\": %u, "
             "\"storageWrites\": %u, \"storageFailures\": %u}",
             g.active ? "true" : "false",
             (unsigned)b.sys.grid_watts, (unsigned)b.sys.grid_minutes,
             g.watts, (unsigned)g.charge_w, g.grid_w, g.storage_w,
             (unsigned)g.adjusts,
             (unsigned)g.remaining_s, (unsigned)g.writes, (unsigned)g.failures,
             storage_mode_name(s.mode), (unsigned)s.remaining_s,
             (unsigned)s.writes, (unsigned)s.failures);
    mqtt_pub_raw(b.topic, b.payload, true);
    xSemaphoreGive(b_lock);
}
