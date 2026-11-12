/* The Home Assistant discovery payloads, built on a workstation.
 *
 * This is the part of the firmware whose failures are silent: an entity that
 * never appears looks exactly like a datapoint that has none. Three separate
 * faults reached a real installation before this test existed -- an enum leaf
 * pointed at a topic that never carries a value, a control that was never
 * published because a setting was blank, and a stack overflow that stopped
 * publishing halfway. All three are cheap to catch here and expensive to
 * diagnose on a device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "../main/app_config.h"
#include "../main/contact.h"
#include "../main/ha_disco.h"
#include "../main/mqtt_pub.h"
#include "../main/o3e_db.h"

/* ---- the world the discovery code talks to ---- */

static mqtt_cfg_t g_cfg;
static sys_cfg_t  g_sys;
static char      *g_points;

typedef struct { char *topic; char *payload; } msg_t;
static msg_t  g_msgs[512];
static size_t g_n;

/* The discovery code only reads grid_hold's caps; the module itself is not
 * linked here, so its state accessor is stubbed. */
void grid_hold_status(void *out) { (void)out; }

int esp_read_mac(uint8_t *out, int t)
{
    (void)t;
    static const uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 };
    memcpy(out, mac, 6);
    return 0;
}

void mqtt_cfg_get(mqtt_cfg_t *out) { *out = g_cfg; }
void sys_cfg_get(sys_cfg_t *out) { *out = g_sys; }
bool mqtt_pub_connected(void) { return true; }

bool mqtt_pub_raw(const char *topic, const char *payload, bool retain)
{
    (void)retain;
    if (g_n < sizeof(g_msgs) / sizeof(g_msgs[0])) {
        g_msgs[g_n].topic = strdup(topic);
        g_msgs[g_n].payload = strdup(payload);
        g_n++;
    }
    return true;
}

/* Same expansion the firmware does; only {didName} is exercised here. */
void mqtt_pub_topic(char *out, size_t out_sz, uint16_t ecu, uint16_t did,
                    const char *did_name, const char *device,
                    const char *suffix_override)
{
    (void)ecu; (void)did; (void)device;
    snprintf(out, out_sz, "%s/%s", g_cfg.base_topic,
             (suffix_override && suffix_override[0]) ? suffix_override
                                                     : (did_name ? did_name : ""));
}

char *app_config_read_file(const char *path) { (void)path; return strdup(g_points); }

/* The selection helpers live in app_config.c next to NVS and LittleFS; only
 * these few lines of them matter here. */
bool sel_is_datapoint(const struct cJSON *e)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, "type");
    if (cJSON_IsString(t) && t->valuestring && strcmp(t->valuestring, "em380") == 0) {
        return false;
    }
    return cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive((const cJSON *)e, "ecu"))
        && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive((const cJSON *)e, "did"));
}
bool sel_enabled(const struct cJSON *e)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, "enabled");
    return !v || cJSON_IsTrue(v);
}
bool sel_bool(const struct cJSON *e, const char *k, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}
uint16_t sel_u16(const struct cJSON *e, const char *k, uint16_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, k);
    return cJSON_IsNumber(v) ? (uint16_t)v->valuedouble : dflt;
}
const char *sel_str(const struct cJSON *e, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ---- checks ---- */

static int fail;
static void ok(const char *what, int cond, const char *detail)
{
    if (!cond) {
        fail++;
        printf("  FAIL %-46s %s\n", what, detail ? detail : "");
    }
}

static const msg_t *find(const char *topic)
{
    for (size_t i = 0; i < g_n; i++) {
        if (strcmp(g_msgs[i].topic, topic) == 0) {
            return &g_msgs[i];
        }
    }
    return NULL;
}

static void reset(const char *points, const char *cmnd, const char *mode)
{
    for (size_t i = 0; i < g_n; i++) { free(g_msgs[i].topic); free(g_msgs[i].payload); }
    g_n = 0;
    free(g_points);
    g_points = strdup(points);
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(&g_sys, 0, sizeof(g_sys));
    g_cfg.enabled = true;
    g_cfg.ha_discovery = true;
    snprintf(g_cfg.base_topic, sizeof(g_cfg.base_topic), "open3e-vent");
    snprintf(g_cfg.ha_prefix, sizeof(g_cfg.ha_prefix), "homeassistant");
    snprintf(g_cfg.format, sizeof(g_cfg.format), "{didName}");
    snprintf(g_cfg.cmnd_topic, sizeof(g_cfg.cmnd_topic), "%s", cmnd);
    (void)mode;
}

/* 533 is writable and its Acutual field is a byte value: the exact datapoint a
 * ventilation unit is operated by. */
#define POINTS_533 "[{\"ecu\":1664,\"did\":533,\"len\":2,\"enabled\":true," \
                   "\"interval\":60,\"mode\":\"%s\",\"topic\":\"\",\"ha\":true}]"
#define POINTS_327 "[{\"ecu\":1664,\"did\":327,\"len\":9,\"enabled\":true," \
                   "\"interval\":60,\"mode\":\"%s\",\"topic\":\"\",\"ha\":true}]"

int main(int argc, char **argv)
{
    if (!o3e_db_open(argc > 1 ? argv[1] : "data/o3edb.bin")) {
        fprintf(stderr, "cannot open the datapoint database\n");
        return 1;
    }
    char points[512];
    const msg_t *m;

    /* --- a writable datapoint gets a control, in both publishing modes --- */
    for (const char *mode = "flat";; mode = "json") {
        snprintf(points, sizeof(points), POINTS_533, mode);
        reset(points, "open3e-vent/cmnd", mode);
        ha_disco_publish_all();

        int sensors = 0, controls = 0;
        ha_disco_counts(&sensors, &controls);
        char d[96];
        snprintf(d, sizeof(d), "mode=%s: %d sensors, %d controls", mode, sensors, controls);
        ok("writable datapoint yields a control", controls >= 1, d);

        m = find("homeassistant/number/open3e_112233/680_533_Acutual_set/config");
        ok("control config on the expected topic", m != NULL, d);
        if (m) {
            cJSON *j = cJSON_Parse(m->payload);
            ok("control payload is valid JSON", j != NULL, m->payload);
            if (j) {
                const cJSON *ct = cJSON_GetObjectItem(j, "command_topic");
                ok("command_topic is the configured one",
                   cJSON_IsString(ct) && strcmp(ct->valuestring, "open3e-vent/cmnd") == 0,
                   m->payload);
                const cJSON *tp = cJSON_GetObjectItem(j, "command_template");
                ok("command_template present", cJSON_IsString(tp), m->payload);
                if (cJSON_IsString(tp)) {
                    /* Substitute the way Home Assistant does, then insist the
                     * result is the command this gateway actually accepts. */
                    char filled[512], *w = filled;
                    for (const char *p = tp->valuestring; *p && w < filled + sizeof(filled) - 8; ) {
                        if (strncmp(p, "{{ value }}", 11) == 0) { *w++ = '4'; p += 11; }
                        else { *w++ = *p++; }
                    }
                    *w = '\0';
                    cJSON *c = cJSON_Parse(filled);
                    ok("rendered command is valid JSON", c != NULL, filled);
                    if (c) {
                        const cJSON *data = cJSON_GetObjectItem(c, "data");
                        const cJSON *did = data ? cJSON_GetObjectItem(data, "533") : NULL;
                        const cJSON *fld = did ? cJSON_GetObjectItem(did, "Acutual") : NULL;
                        ok("rendered command sets the right field",
                           cJSON_IsNumber(fld) && fld->valuedouble == 4, filled);
                        cJSON_Delete(c);
                    }
                }
                const cJSON *mx = cJSON_GetObjectItem(j, "max");
                ok("range comes from the field width, not HA's 0..100",
                   cJSON_IsNumber(mx) && mx->valuedouble == 255, m->payload);
                cJSON_Delete(j);
            }
        }
        if (strcmp(mode, "json") == 0) {
            break;
        }
    }

    /* --- an empty command topic must not silently swallow every control --- */
    snprintf(points, sizeof(points), POINTS_533, "flat");
    reset(points, "", "flat");
    ha_disco_publish_all();
    {
        int sensors = 0, controls = 0;
        ha_disco_counts(&sensors, &controls);
        char d[96];
        snprintf(d, sizeof(d), "%d sensors, %d controls", sensors, controls);
        ok("without a command topic: sensors still published", sensors > 0, d);
        ok("without a command topic: no controls, and it is countable",
           controls == 0, d);
    }

    /* --- an enum leaf is an object on the wire, not a scalar --- */
    snprintf(points, sizeof(points), POINTS_327, "flat");
    reset(points, "open3e-vent/cmnd", "flat");
    ha_disco_publish_all();
    m = find("homeassistant/sensor/open3e_112233/680_327_Error/config");
    ok("enum leaf is announced", m != NULL, NULL);
    if (m) {
        cJSON *j = cJSON_Parse(m->payload);
        const cJSON *st = j ? cJSON_GetObjectItem(j, "state_topic") : NULL;
        ok("enum sensor points at the Text sub-topic, which is what flat mode fills",
           cJSON_IsString(st)
             && strcmp(st->valuestring,
                       "open3e-vent/OutdoorAirTemperatureSensor/Error/Text") == 0,
           cJSON_IsString(st) ? st->valuestring : "(none)");
        cJSON_Delete(j);
    }
    m = find("homeassistant/sensor/open3e_112233/680_327_Actual/config");
    if (m) {
        cJSON *j = cJSON_Parse(m->payload);
        const cJSON *u = j ? cJSON_GetObjectItem(j, "unit_of_measurement") : NULL;
        ok("temperature carries the unit the overrides fill in",
           cJSON_IsString(u) && strcmp(u->valuestring, "°C") == 0,
           cJSON_IsString(u) ? u->valuestring : "(none)");
        const cJSON *sc = j ? cJSON_GetObjectItem(j, "state_class") : NULL;
        ok("a measurement is a measurement",
           cJSON_IsString(sc) && strcmp(sc->valuestring, "measurement") == 0, NULL);
        cJSON_Delete(j);
    } else {
        ok("temperature leaf is announced", 0, NULL);
    }

    /* --- an operator's range narrows the number, in HA's own min/max --- */
    reset("[{\"ecu\":1664,\"did\":533,\"len\":2,\"enabled\":true,\"interval\":60,"
          "\"mode\":\"flat\",\"topic\":\"\",\"ha\":true,"
          "\"range\":{\"*\":{\"min\":1,\"max\":4}}}]", "open3e-vent/cmnd", "flat");
    ha_disco_publish_all();
    m = find("homeassistant/number/open3e_112233/680_533_Acutual_set/config");
    ok("a ranged number is still a number", m != NULL, NULL);
    if (m) {
        cJSON *j = cJSON_Parse(m->payload);
        const cJSON *mn = j ? cJSON_GetObjectItem(j, "min") : NULL;
        const cJSON *mx = j ? cJSON_GetObjectItem(j, "max") : NULL;
        ok("min and max come from the selection's range (#21)",
           cJSON_IsNumber(mn) && mn->valuedouble == 1
             && cJSON_IsNumber(mx) && mx->valuedouble == 4, m->payload);
        cJSON_Delete(j);
    }
    /* A range for some other field leaves this one at its full width, and a
     * bound beyond the field's width is not widened to. */
    reset("[{\"ecu\":1664,\"did\":533,\"len\":2,\"enabled\":true,\"interval\":60,"
          "\"mode\":\"flat\",\"topic\":\"\",\"ha\":true,"
          "\"range\":{\"Other\":{\"max\":4},\"Acutual\":{\"max\":999}}}]",
          "open3e-vent/cmnd", "flat");
    ha_disco_publish_all();
    m = find("homeassistant/number/open3e_112233/680_533_Acutual_set/config");
    if (m) {
        cJSON *j = cJSON_Parse(m->payload);
        const cJSON *mx = j ? cJSON_GetObjectItem(j, "max") : NULL;
        ok("a range cannot widen a field beyond its width",
           cJSON_IsNumber(mx) && mx->valuedouble == 255, m->payload);
        cJSON_Delete(j);
    } else {
        ok("ranged control announced", 0, NULL);
    }

    /* --- labels turn a bare number into a named choice, both ways --- */
    reset("[{\"ecu\":1664,\"did\":437,\"len\":2,\"enabled\":true,\"interval\":60,"
          "\"mode\":\"flat\",\"topic\":\"\",\"ha\":true,"
          "\"labels\":{\"*\":{\"0\":\"geschlossen\",\"1\":\"offen\","
          "\"2\":\"automatisch\"}}}]", "open3e-vent/cmnd", "flat");
    ha_disco_publish_all();

    if (getenv("DUMP")) {
        for (size_t i = 0; i < g_n; i++) printf("    %s\n", g_msgs[i].topic);
    }
    m = find("homeassistant/select/open3e_112233/680_437_BypassStatus_set/config");
    ok("a labelled number becomes a select, not a spin box", m != NULL, NULL);
    if (m) {
        cJSON *j = cJSON_Parse(m->payload);
        ok("labelled control payload is valid JSON", j != NULL, m->payload);
        if (j) {
            const cJSON *opts = cJSON_GetObjectItem(j, "options");
            ok("options are the labels", cJSON_IsArray(opts)
                 && cJSON_GetArraySize(opts) == 3
                 && strcmp(cJSON_GetArrayItem(opts, 0)->valuestring, "geschlossen") == 0
                 && strcmp(cJSON_GetArrayItem(opts, 2)->valuestring, "automatisch") == 0,
               m->payload);
            ok("a labelled select has no numeric range",
               cJSON_GetObjectItem(j, "max") == NULL, m->payload);

            /* Home Assistant sends the word; the bus must receive the number. */
            const cJSON *tp = cJSON_GetObjectItem(j, "command_template");
            ok("command_template maps the word back to its number",
               cJSON_IsString(tp)
                 && strstr(tp->valuestring, "'automatisch': 2") != NULL
                 && strstr(tp->valuestring, "{{ m[value] }}") != NULL,
               cJSON_IsString(tp) ? tp->valuestring : "(none)");

            /* And the reading must show the word, not the digit. */
            const cJSON *vt = cJSON_GetObjectItem(j, "value_template");
            ok("value_template maps the number to its word",
               cJSON_IsString(vt)
                 && strstr(vt->valuestring, "'2': 'automatisch'") != NULL
                 && strstr(vt->valuestring, "m.get(v, v)") != NULL,
               cJSON_IsString(vt) ? vt->valuestring : "(none)");
            cJSON_Delete(j);
        }
    }
    /* A discovery config is retained. Publishing the select is not enough --
       the number's config would keep being handed to Home Assistant until it
       is overwritten with an empty payload, and the entity would never change
       kind. This is what made entering labels look like it did nothing. */
    m = find("homeassistant/number/open3e_112233/680_437_BypassStatus_set/config");
    ok("the number's config is retracted, not merely abandoned",
       m != NULL && m->payload[0] == '\0',
       m ? (m->payload[0] ? m->payload : "(empty)") : "(never published)");

    /* --- the grid hold announces itself, and its templates must be usable --- */
    snprintf(points, sizeof(points), POINTS_533, "flat");
    reset(points, "open3e-vent/cmnd", "flat");
    ha_disco_publish_all();

    struct { const char *topic; const char *what; } want[] = {
        { "homeassistant/switch/open3e_112233/grid_hold/config",     "Schalter" },
        { "homeassistant/number/open3e_112233/grid_power/config",    "Leistung" },
        { "homeassistant/number/open3e_112233/grid_minutes/config",  "Dauer" },
        { "homeassistant/sensor/open3e_112233/grid_remaining/config","Restzeit" },
        { "homeassistant/select/open3e_112233/storage_mode/config", "Betriebsart" },
        { "homeassistant/sensor/open3e_112233/storage_remaining/config",
          "Betriebsart Restzeit" },
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        m = find(want[i].topic);
        ok("grid entity announced", m != NULL, want[i].what);
        if (!m) {
            continue;
        }
        cJSON *j = cJSON_Parse(m->payload);
        ok("grid entity payload is valid JSON", j != NULL, m->payload);
        if (!j) {
            continue;
        }
        const cJSON *st = cJSON_GetObjectItem(j, "state_topic");
        ok("grid entity reads the hold's own topic",
           cJSON_IsString(st) && strcmp(st->valuestring, "open3e-vent/hold") == 0,
           cJSON_IsString(st) ? st->valuestring : "(none)");
        /* Whatever the entity sends must be a command this gateway accepts. */
        for (const char *key = "payload_on";; key = "command_template") {
            const cJSON *p = cJSON_GetObjectItem(j, key);
            if (cJSON_IsString(p)) {
                char filled[512], *w = filled;
                for (const char *q = p->valuestring;
                     *q && w < filled + sizeof(filled) - 8; ) {
                    if (strncmp(q, "{{ value }}", 11) == 0) { *w++ = '9'; q += 11; }
                    else { *w++ = *q++; }
                }
                *w = '\0';
                cJSON *c = cJSON_Parse(filled);
                ok("grid command renders to valid JSON", c != NULL, filled);
                if (c) {
                    /* Whichever entity it is, the command has to name a mode
                       the command listener actually dispatches on. */
                    const cJSON *md = cJSON_GetObjectItem(c, "mode");
                    ok("command names a mode this gateway dispatches",
                       cJSON_IsString(md)
                         && (strcmp(md->valuestring, "grid") == 0
                          || strcmp(md->valuestring, "storage") == 0),
                       filled);
                    cJSON_Delete(c);
                }
            }
            if (strcmp(key, "command_template") == 0) {
                break;
            }
        }
        cJSON_Delete(j);
    }

    m = find("homeassistant/select/open3e_112233/storage_mode/config");
    if (m) {
        cJSON *j = cJSON_Parse(m->payload);
        const cJSON *opts = j ? cJSON_GetObjectItem(j, "options") : NULL;
        ok("storage mode offers all four states",
           cJSON_IsArray(opts) && cJSON_GetArraySize(opts) == 4
             && strcmp(cJSON_GetArrayItem(opts, 0)->valuestring, "normal") == 0
             && strcmp(cJSON_GetArrayItem(opts, 1)->valuestring, "steht still") == 0,
           m->payload);
        cJSON_Delete(j);
    }

    /* --- the contact inputs --- */
    snprintf(points, sizeof(points), POINTS_533, "flat");
    reset(points, "open3e-vent/cmnd", "flat");
    g_sys.contact[0].enabled = true;
    snprintf(g_sys.contact[0].name, sizeof(g_sys.contact[0].name), "Klingel Haustür");
    snprintf(g_sys.contact[0].device_class,
             sizeof(g_sys.contact[0].device_class), "sound");
    ha_disco_publish_all();

    m = find("homeassistant/binary_sensor/open3e_112233/contact1/config");
    ok("contact input is announced", m != NULL && m->payload[0], NULL);
    if (m && m->payload[0]) {
        cJSON *j = cJSON_Parse(m->payload);
        ok("contact payload is valid JSON", j != NULL, m->payload);
        if (j) {
            const cJSON *st = cJSON_GetObjectItem(j, "state_topic");
            ok("contact reads the topic its own name produces",
               cJSON_IsString(st)
                 && strcmp(st->valuestring, "open3e-vent/contact/klingel_haustuer") == 0,
               cJSON_IsString(st) ? st->valuestring : "(none)");
            const cJSON *dc = cJSON_GetObjectItem(j, "device_class");
            ok("contact carries the chosen device class",
               cJSON_IsString(dc) && strcmp(dc->valuestring, "sound") == 0,
               m->payload);
            /* A binary sensor with a command topic is an entity Home Assistant
               will happily let somebody press, which would send an empty
               command to the bus. */
            ok("a contact is read-only",
               cJSON_GetObjectItem(j, "command_topic") == NULL, m->payload);
            cJSON_Delete(j);
        }
    }
    /* The second input is off, so its config has to be retracted rather than
       left alone -- otherwise a contact that is switched off keeps a
       permanently unavailable entity in Home Assistant. */
    m = find("homeassistant/binary_sensor/open3e_112233/contact2/config");
    ok("a disabled contact is retracted",
       m != NULL && m->payload[0] == '\0',
       m ? (m->payload[0] ? m->payload : "(empty)") : "(never published)");

    /* --- an output --- */
    reset(points, "open3e-vent/cmnd", "flat");
    g_sys.contact[1].enabled = true;
    g_sys.contact[1].mode = CONTACT_MODE_OUTPUT;
    snprintf(g_sys.contact[1].name, sizeof(g_sys.contact[1].name), "SG Sperre");
    ha_disco_publish_all();

    m = find("homeassistant/switch/open3e_112233/contact2/config");
    ok("an output is announced as a switch", m != NULL && m->payload[0], NULL);
    if (m && m->payload[0]) {
        cJSON *j = cJSON_Parse(m->payload);
        ok("switch payload is valid JSON", j != NULL, m->payload);
        if (j) {
            const cJSON *st = cJSON_GetObjectItem(j, "state_topic");
            const cJSON *ct = cJSON_GetObjectItem(j, "command_topic");
            ok("switch reads its own output topic",
               cJSON_IsString(st)
                 && strcmp(st->valuestring, "open3e-vent/output/sg_sperre") == 0,
               cJSON_IsString(st) ? st->valuestring : "(none)");
            /* Its own set topic, not the shared command topic: ON and OFF
               as payloads, nothing to template, and usable by anything that
               is not Home Assistant. */
            ok("switch commands its own set topic",
               cJSON_IsString(ct)
                 && strcmp(ct->valuestring, "open3e-vent/output/sg_sperre/set") == 0,
               cJSON_IsString(ct) ? ct->valuestring : "(none)");
            const cJSON *po = cJSON_GetObjectItem(j, "payload_on");
            ok("switch sends plain ON",
               cJSON_IsString(po) && strcmp(po->valuestring, "ON") == 0, m->payload);
            cJSON_Delete(j);
        }
    }
    /* The pin was a binary sensor before it became an output; that entity
       has to go, or Home Assistant shows a sensor and a switch for one pin. */
    m = find("homeassistant/binary_sensor/open3e_112233/contact2/config");
    ok("an output's old sensor entity is retracted",
       m != NULL && m->payload[0] == '\0',
       m ? (m->payload[0] ? m->payload : "(empty)") : "(never published)");
    /* And the other way round: the input on pin 1 must not grow a switch. */
    m = find("homeassistant/switch/open3e_112233/contact1/config");
    ok("an input has no switch entity",
       m == NULL || m->payload[0] == '\0', m ? m->payload : "(never published)");

    printf("%s: %zu discovery messages inspected\n", fail ? "FAILED" : "ha_disco", g_n);
    return fail ? 1 : 0;
}
