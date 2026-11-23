#include "plant_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_config.h"
#include "ha_disco.h"
#include "heat_curve.h"
#include "mqtt_pub.h"
#include "o3e_json.h"
#include "poller.h"

static const char *TAG = "plant";

#define PLANT_PERIOD_MS   30000
#define LIVE_EVERY        2      /* cycles: the filled picture every minute */
#define DID_COMPRESSOR    2346   /* CompressorSpeedPercent, on the outdoor unit */
#define DID_DIVERTER      386    /* DiverterValveTargetPosition, boilers: 0, 1, 2 */
#define DID_PUMP          381    /* CentralHeatingPump {State, TargetValue, Actual} */

#define LIVE_TEMPLATE     CFG_WWW_DIR "/plant_live.svg"
#define LIVE_SLOTS        CFG_MOUNT "/plant_slots.json"

/* The controller addresses come from the probe the web interface stored:
 * "roles": {"indoor": "0x680", "outdoor": "0x68C"}. A gas boiler has no
 * outdoor unit; `*outdoor` is 0 then. */
static bool plant_roles(uint16_t *indoor, uint16_t *outdoor)
{
    char *raw = app_config_read_file(CFG_PLANT_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!root) {
        return false;
    }
    const cJSON *roles = cJSON_GetObjectItemCaseSensitive(root, "roles");
    const cJSON *ji = roles ? cJSON_GetObjectItemCaseSensitive(roles, "indoor") : NULL;
    const cJSON *jo = roles ? cJSON_GetObjectItemCaseSensitive(roles, "outdoor") : NULL;
    bool ok = cJSON_IsString(ji);
    if (ok) {
        *indoor = (uint16_t)strtol(ji->valuestring, NULL, 0);
        *outdoor = cJSON_IsString(jo) ? (uint16_t)strtol(jo->valuestring, NULL, 0) : 0;
        ok = *indoor != 0;
    }
    cJSON_Delete(root);
    return ok;
}

/* One datapoint, decoded; the caller owns the result. */
static cJSON *read_value(uint16_t ecu, uint16_t did)
{
    char err[96];
    char *json = poller_read_now(ecu, did, err, sizeof(err));
    if (!json) {
        return NULL;
    }
    cJSON *v = cJSON_Parse(json);
    free(json);
    return v;
}

/* The number a slot shows: the value itself, or `field` of an object, or
 * its "Actual" when no field is named. */
static bool slot_number(const cJSON *v, const char *field, double *out)
{
    const cJSON *n = v;
    if (cJSON_IsObject(v)) {
        n = field ? cJSON_GetObjectItemCaseSensitive(v, field)
                  : cJSON_GetObjectItemCaseSensitive(v, "Actual");
    }
    if (!cJSON_IsNumber(n)) {
        return false;
    }
    *out = n->valuedouble;
    return true;
}

static bool read_number(uint16_t ecu, uint16_t did, const char *field, double *out)
{
    cJSON *v = read_value(ecu, did);
    bool ok = v && slot_number(v, field, out);
    cJSON_Delete(v);
    return ok;
}

/* The text for one slot, the way the web interface writes it: German decimal
 * comma, the slot's decimals and unit, a word from its map for a state. */
static void slot_text(const cJSON *slot, const cJSON *v, char *out, size_t out_sz)
{
    const cJSON *jf = cJSON_GetObjectItemCaseSensitive(slot, "f");
    const char *field = cJSON_IsString(jf) ? jf->valuestring : NULL;
    double x;
    if (!v || !slot_number(v, field, &x)) {
        /* A string value (an enum's text) is shown as it is. */
        const cJSON *s = v;
        if (cJSON_IsObject(v) && field) {
            s = cJSON_GetObjectItemCaseSensitive(v, field);
        }
        snprintf(out, out_sz, "%s", cJSON_IsString(s) ? s->valuestring : "\xe2\x80\x93");
        return;
    }
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(slot, "map");
    if (cJSON_IsObject(map)) {
        char key[16];
        snprintf(key, sizeof(key), "%d", (int)x);
        const cJSON *word = cJSON_GetObjectItemCaseSensitive(map, key);
        if (cJSON_IsString(word)) {
            snprintf(out, out_sz, "%s", word->valuestring);
            return;
        }
    }
    const cJSON *jscale = cJSON_GetObjectItemCaseSensitive(slot, "scale");
    if (cJSON_IsNumber(jscale) && jscale->valuedouble > 0) {
        x *= jscale->valuedouble;
    }
    const cJSON *junit = cJSON_GetObjectItemCaseSensitive(slot, "unit");
    const char *unit = cJSON_IsString(junit) ? junit->valuestring : "";
    const cJSON *jd = cJSON_GetObjectItemCaseSensitive(slot, "d");
    int d = cJSON_IsNumber(jd) ? (int)jd->valuedouble
          : ((strcmp(unit, "%") == 0 || !unit[0]) ? 0 : 1);
    char num[32];
    snprintf(num, sizeof(num), "%.*f", d, x);
    for (char *p = num; *p; p++) {
        if (*p == '.') {
            *p = ',';
        }
    }
    snprintf(out, out_sz, "%s%s%s", num, unit[0] ? " " : "", unit);
}

/* Fill the template: every {{id}} becomes the text of the slot with that id.
 * Reads happen one after another on the bus, like the web interface does. */
static bool render_live(bool *comp_running, bool *pump_running, int *valve)
{
    char *tpl = app_config_read_file(LIVE_TEMPLATE);
    if (!tpl || strncmp(tpl, "<svg", 4) != 0) {
        free(tpl);
        return false;
    }
    char *raw = app_config_read_file(LIVE_SLOTS);
    cJSON *slots = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!cJSON_IsArray(slots)) {
        cJSON_Delete(slots);
        free(tpl);
        return false;
    }

    /* Read first, substitute after: the bus is the slow part, and a value
     * used twice in the picture is read once. */
    int n = cJSON_GetArraySize(slots);
    char (*texts)[48] = calloc((size_t)(n > 0 ? n : 1), sizeof(*texts));
    if (!texts) {
        cJSON_Delete(slots);
        free(tpl);
        return false;
    }
    for (int i = 0; i < n; i++) {
        const cJSON *slot = cJSON_GetArrayItem(slots, i);
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(slot, "id");
        uint16_t ecu = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "ecu"));
        uint16_t did = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "did"));
        if (!cJSON_IsString(jid) || !ecu || !did) {
            continue;
        }
        cJSON *v = read_value(ecu, did);
        slot_text(slot, v, texts[i], sizeof(texts[i]));
        double x;
        if (v && strcmp(jid->valuestring, "comp") == 0 && slot_number(v, NULL, &x)) {
            *comp_running = x > 0;
        }
        if (v && strcmp(jid->valuestring, "pump") == 0 && slot_number(v, "Actual", &x)) {
            *pump_running = x > 0;
        }
        /* Positions rather than a percentage: the slot carries a map then. */
        if (v && strcmp(jid->valuestring, "divert") == 0
            && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(slot, "map"))
            && slot_number(v, NULL, &x)) {
            *valve = (int)x;
        }
        cJSON_Delete(v);
    }

    o3e_buf_t b;
    o3e_buf_init(&b);
    const char *p = tpl;
    while (*p) {
        const char *open = strstr(p, "{{");
        const char *close = open ? strstr(open, "}}") : NULL;
        if (!open || !close) {
            o3e_buf_adds(&b, p);
            break;
        }
        o3e_buf_add(&b, p, (size_t)(open - p));
        char id[32];
        size_t idl = (size_t)(close - open - 2);
        if (idl >= sizeof(id)) {
            idl = sizeof(id) - 1;
        }
        memcpy(id, open + 2, idl);
        id[idl] = '\0';
        const char *text = "\xe2\x80\x93";
        for (int i = 0; i < n; i++) {
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(slots, i), "id");
            if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0) {
                text = texts[i];
                break;
            }
        }
        /* Values are numbers and short words; the one character that could
         * break the document is escaped anyway. */
        for (const char *c = text; *c; c++) {
            if (*c == '<') {
                o3e_buf_adds(&b, "&lt;");
            } else if (*c == '&') {
                o3e_buf_adds(&b, "&amp;");
            } else {
                o3e_buf_addc(&b, *c);
            }
        }
        p = close + 2;
    }
    free(texts);
    cJSON_Delete(slots);
    free(tpl);

    bool ok = !b.oom && b.buf;
    if (ok) {
        ha_disco_plant_live(b.buf);
    }
    o3e_buf_free(&b);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Heating curves                                                       */
/*
 * Whoever selects a circuit's heating curve (DID 880..887) for publishing
 * gets its picture with it: the two numbers are what one sets, the line is
 * what they mean. Independent of the schematic -- no probe, no stored SVG,
 * the selection alone decides, and the controller address comes from it.
 */
#define DID_CURVE          880    /* +1 per circuit: {Gradient, Level, BasePoint} */
#define CURVE_CIRCUITS     8
#define DID_ROOM_SETPOINT  424    /* +2 per circuit: {Comfort, Standard, Reduced} */
#define DID_CIRCUIT_NAME   627    /* +1 per circuit */
#define DID_FLOW_TARGET    987    /* +1, circuits one to four: flow setpoint in force */
#define DID_FLOW_LIMITS    1192   /* +1 per circuit: {Minimum, Maximum} */
#define DID_ROOM_CURRENT   1643   /* +1, circuits one to four: room setpoint in force */
#define DID_OUTSIDE        274    /* {Actual} */
#define DID_OUTSIDE_DAMPED 2457   /* {DampedActual}: what the controller works with */
/* A picture that did not change is not sent again -- except this often, so a
 * broker that lost its retained messages gets them back. In render cycles. */
#define CURVE_REFRESH      30

static uint32_t s_curve_hash[CURVE_CIRCUITS];
static uint8_t  s_curve_age[CURVE_CIRCUITS];

/* Which circuits have a picture at the broker, one bit each, kept across
 * restarts. Taking a deselected curve back means publishing empty retained
 * messages, and doing that for all eight after every start made topics
 * appear at whoever watches the broker -- ioBroker's MQTT adapter keeps an
 * object for each -- on installations that never had a curve (discussion
 * #23). Now only what was published is taken back. */
#define CURVE_NVS_NS   "plant"
#define CURVE_NVS_KEY  "curve_pub"
static uint8_t s_curve_pub;
static bool    s_curve_pub_loaded;

static void curve_pub_load(void)
{
    if (s_curve_pub_loaded) {
        return;
    }
    s_curve_pub_loaded = true;
    /* No record yet: a firmware before this one may have left pictures
     * behind, so the first run still takes back all eight, once. */
    s_curve_pub = 0xFF;
    nvs_handle_t h;
    if (nvs_open(CURVE_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, CURVE_NVS_KEY, &s_curve_pub);
        nvs_close(h);
    }
}

static void curve_pub_store(uint8_t mask)
{
    if (mask == s_curve_pub) {
        return;
    }
    s_curve_pub = mask;
    nvs_handle_t h;
    if (nvs_open(CURVE_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, CURVE_NVS_KEY, mask);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h ? h : 1;   /* 0 means "nothing published" */
}

/* Which controller each selected curve belongs to; 0 where none is selected. */
static void curve_selection(uint16_t ecu_of[CURVE_CIRCUITS])
{
    memset(ecu_of, 0, sizeof(uint16_t) * CURVE_CIRCUITS);
    char *raw = app_config_read_file(CFG_POINTS_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    const cJSON *p;
    cJSON_ArrayForEach(p, root) {
        const cJSON *jd = cJSON_GetObjectItemCaseSensitive(p, "did");
        const cJSON *je = cJSON_GetObjectItemCaseSensitive(p, "ecu");
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(p, "enabled");
        if (!cJSON_IsNumber(jd) || !cJSON_IsNumber(je) || cJSON_IsFalse(on)) {
            continue;
        }
        int did = (int)jd->valuedouble;
        if (did >= DID_CURVE && did < DID_CURVE + CURVE_CIRCUITS) {
            ecu_of[did - DID_CURVE] = (uint16_t)je->valuedouble;
        }
    }
    cJSON_Delete(root);
}

static void curve_render(int n, uint16_t ecu)
{
    heat_curve_t c = { 0 };
    cJSON *curve = read_value(ecu, (uint16_t)(DID_CURVE + n));
    bool have = curve && slot_number(curve, "Gradient", &c.gradient)
                && slot_number(curve, "Level", &c.level);
    cJSON_Delete(curve);
    if (!have) {
        /* Not readable right now: the last picture stays. */
        return;
    }
    /* The line is drawn for the room setpoint in force, so that at night it
     * is the reduced line the plant follows and the point sits on it. Where
     * the controller does not say (circuits five to eight, or a circuit that
     * is off and reports nothing sensible), the normal setpoint stands in. */
    double x;
    bool have_room = n < 4 && read_number(ecu, (uint16_t)(DID_ROOM_CURRENT + n), NULL, &x)
                     && x >= 5.0 && x <= 35.0;
    if (!have_room) {
        have_room = read_number(ecu, (uint16_t)(DID_ROOM_SETPOINT + 2 * n), "Standard", &x)
                    && x >= 5.0 && x <= 35.0;
    }
    c.room = have_room ? x : 20.0;

    double lo, hi;
    cJSON *limits = read_value(ecu, (uint16_t)(DID_FLOW_LIMITS + n));
    if (limits && slot_number(limits, "Minimum", &lo)
        && slot_number(limits, "Maximum", &hi) && hi > lo) {
        c.have_limits = true;
        c.flow_min = lo;
        c.flow_max = hi;
    }
    cJSON_Delete(limits);
    c.have_outside = read_number(ecu, DID_OUTSIDE_DAMPED, "DampedActual", &c.outside)
                     || read_number(ecu, DID_OUTSIDE, "Actual", &c.outside);
    /* A circuit that is not heating reports 0 here; then the formula's value
     * is the more useful thing to show, and the picture says which it is. */
    c.have_flow = c.have_outside && n < 4
                  && read_number(ecu, (uint16_t)(DID_FLOW_TARGET + n), NULL, &c.flow)
                  && c.flow > 5.0;

    cJSON *jname = read_value(ecu, (uint16_t)(DID_CIRCUIT_NAME + n));
    c.name = cJSON_IsString(jname) ? jname->valuestring : NULL;

    char *svg = heat_curve_svg(&c);
    if (svg) {
        uint32_t h = fnv1a(svg);
        if ((h != s_curve_hash[n] || ++s_curve_age[n] >= CURVE_REFRESH)
            && ha_disco_curve_image(n + 1, c.name, svg)) {
            s_curve_hash[n] = h;
            s_curve_age[n] = 0;
            curve_pub_store(s_curve_pub | (uint8_t)(1u << n));
        }
        free(svg);
    }
    cJSON_Delete(jname);
}

static void curve_tick(void)
{
    uint16_t ecu_of[CURVE_CIRCUITS];
    curve_selection(ecu_of);
    curve_pub_load();
    uint8_t cleared = 0;
    for (int n = 0; n < CURVE_CIRCUITS; n++) {
        if (ecu_of[n]) {
            curve_render(n, ecu_of[n]);
        } else if ((s_curve_pub & (1u << n)) && ha_disco_curve_image(n + 1, NULL, NULL)) {
            /* Deselected, now or while the device was off. */
            s_curve_hash[n] = 0;
            cleared |= (uint8_t)(1u << n);
        }
    }
    /* One write for the lot: the first run after an update clears eight. */
    curve_pub_store(s_curve_pub & (uint8_t)~cleared);
}

static void plant_task(void *arg)
{
    (void)arg;
    unsigned cycle = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PLANT_PERIOD_MS));
        cycle++;
        if (!mqtt_pub_connected()) {
            continue;
        }
        if (cycle % LIVE_EVERY == 0) {
            curve_tick();
        }
        /* No picture stored, nothing to keep in step. */
        FILE *f = fopen(CFG_WWW_DIR "/plant.svg", "rb");
        if (!f) {
            continue;
        }
        fclose(f);
        uint16_t indoor, outdoor;
        if (!plant_roles(&indoor, &outdoor)) {
            continue;
        }
        /* A value that could not be read keeps the circuit moving: a stale
         * picture that moves is less misleading than one that stops on a
         * bus hiccup. */
        bool comp = true, pump = true;
        int valve = -1;
        if (cycle % LIVE_EVERY == 0 && render_live(&comp, &pump, &valve)) {
            /* The filled picture already read compressor, pump and valve. */
            ha_disco_plant_state(outdoor ? comp : false, pump);
            ha_disco_plant_valve(valve);
            continue;
        }
        double c = 0, w = 0;
        bool have_comp = outdoor && read_number(outdoor, DID_COMPRESSOR, NULL, &c);
        bool have_pump = read_number(indoor, DID_PUMP, "Actual", &w);
        if (!have_comp && !have_pump) {
            continue;
        }
        ha_disco_plant_state(have_comp ? c > 0 : outdoor != 0, have_pump ? w > 0 : true);
        double pos = 0;
        if (!outdoor) {
            ha_disco_plant_valve(read_number(indoor, DID_DIVERTER, NULL, &pos) ? (int)pos : -1);
        }
    }
}

void plant_image_start(void)
{
    if (xTaskCreate(plant_task, "plant", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the plant image task");
    }
}
