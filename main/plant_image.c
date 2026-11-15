#include "plant_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "ha_disco.h"
#include "mqtt_pub.h"
#include "o3e_json.h"
#include "poller.h"

static const char *TAG = "plant";

#define PLANT_PERIOD_MS   30000
#define LIVE_EVERY        2      /* cycles: the filled picture every minute */
#define DID_COMPRESSOR    2346   /* CompressorSpeedPercent, on the outdoor unit */
#define DID_PUMP          381    /* CentralHeatingPump {State, TargetValue, Actual} */

#define LIVE_TEMPLATE     CFG_WWW_DIR "/plant_live.svg"
#define LIVE_SLOTS        CFG_MOUNT "/plant_slots.json"

/* The controller addresses come from the probe the web interface stored:
 * "roles": {"indoor": "0x680", "outdoor": "0x68C"}. */
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
    bool ok = cJSON_IsString(ji) && cJSON_IsString(jo);
    if (ok) {
        *indoor = (uint16_t)strtol(ji->valuestring, NULL, 0);
        *outdoor = (uint16_t)strtol(jo->valuestring, NULL, 0);
        ok = *indoor && *outdoor;
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
static bool render_live(bool *comp_running, bool *pump_running)
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
        if (cycle % LIVE_EVERY == 0 && render_live(&comp, &pump)) {
            /* The filled picture already read compressor and pump. */
            ha_disco_plant_state(comp, pump);
            continue;
        }
        double c = 0, w = 0;
        bool have_comp = read_number(outdoor, DID_COMPRESSOR, NULL, &c);
        bool have_pump = read_number(indoor, DID_PUMP, "Actual", &w);
        if (!have_comp && !have_pump) {
            continue;
        }
        ha_disco_plant_state(have_comp ? c > 0 : true, have_pump ? w > 0 : true);
    }
}

void plant_image_start(void)
{
    if (xTaskCreate(plant_task, "plant", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the plant image task");
    }
}
