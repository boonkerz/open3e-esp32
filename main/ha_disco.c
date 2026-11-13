#include "ha_disco.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "app_config.h"
#include "contact.h"
#include "hold.h"
#include "mqtt_pub.h"
#include "o3e_codec.h"
#include "o3e_db.h"
#include "o3e_json.h"

static const char *TAG = "ha";

static void ha_disco_grid(const mqtt_cfg_t *cfg, const char *dev_id, bool clear);
static void ha_disco_contacts(const mqtt_cfg_t *cfg, const char *dev_id, bool clear);

/* Map the open3e unit string onto a Home Assistant device class. Only units
 * that actually occur in the database are listed; anything else is published
 * without a class, which HA renders fine. */
static const char *device_class_for(const char *unit)
{
    if (!unit || !unit[0]) {
        return NULL;
    }
    if (strcmp(unit, "°C") == 0)  return "temperature";
    if (strcmp(unit, "hPa") == 0)      return "pressure";
    if (strcmp(unit, "bar") == 0)      return "pressure";
    if (strcmp(unit, "kWh") == 0)      return "energy";
    if (strcmp(unit, "Wh") == 0)       return "energy";
    if (strcmp(unit, "W") == 0)        return "power";
    if (strcmp(unit, "kW") == 0)       return "power";
    if (strcmp(unit, "V") == 0)        return "voltage";
    if (strcmp(unit, "A") == 0)        return "current";
    if (strcmp(unit, "Hz") == 0)       return "frequency";
    if (strcmp(unit, "h") == 0)        return "duration";
    if (strcmp(unit, "l/h") == 0)      return "volume_flow_rate";
    return NULL;
}

static void device_id(char *out, size_t out_sz)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_sz, "open3e_%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/* One config topic per published scalar. In JSON mode a value_template picks
 * the field out of the payload; in flat mode the field already has its own
 * topic and no template is needed. */
/* User-supplied names for the numbers a datapoint carries.
 *
 * The database knows enumerations for the datapoints Viessmann documented as
 * such, and plain byte values for the rest -- BypassOperationState is a number
 * from 0 to 2 with no list behind it, so a control for it is a spin box and a
 * reading of "2" means nothing without the manual. The selection may therefore
 * carry labels of the operator's own:
 *
 *     "labels": {"*": {"0": "geschlossen", "1": "offen", "2": "automatisch"}}
 *
 * A field name in place of "*" restricts them to that field; "*" applies to
 * every numeric leaf of the datapoint, which is what one usually wants because
 * the interesting one is typically the only numeric field there.
 *
 * They turn a number into a select and a bare digit into a word, in both
 * directions, without touching the database or the decoded value: what goes on
 * the bus and onto the state topic stays numeric, and only Home Assistant sees
 * the names.
 */
static const cJSON *labels_for(const cJSON *labels, const char *field)
{
    if (!cJSON_IsObject(labels)) {
        return NULL;
    }
    const cJSON *by_field = field
        ? cJSON_GetObjectItemCaseSensitive(labels, field) : NULL;
    if (cJSON_IsObject(by_field)) {
        return by_field;
    }
    const cJSON *any = cJSON_GetObjectItemCaseSensitive(labels, "*");
    return cJSON_IsObject(any) ? any : NULL;
}

/* Limits of the operator's own for a writable number, in the same shape:
 *
 *     "range": {"*": {"min": 10, "max": 60}}
 *
 * The database bounds a number only by its byte width, so a temperature
 * setpoint arrives in Home Assistant as a slider from -3276.8 to 3276.7 (#21).
 * The device itself rejects nonsense with requestOutOfRange, but a control
 * that ends where the plant's limits end is the better interface -- and the
 * same entry is what poller_write_now() enforces on every write, not only the
 * ones Home Assistant sends. Either bound may be left out. */
static const cJSON *range_for(const cJSON *range, const char *field)
{
    return labels_for(range, field);   /* same lookup: the field, else "*" */
}

/* {% set m = {'0': 'zu', '1': 'offen'} %} -- the head both directions share. */
static void add_label_map(o3e_buf_t *b, const cJSON *labels, bool text_to_value)
{
    o3e_buf_adds(b, "{% set m = {");
    const cJSON *it;
    bool first = true;
    cJSON_ArrayForEach(it, labels) {
        if (!it->string || !cJSON_IsString(it)) {
            continue;
        }
        if (!first) {
            o3e_buf_adds(b, ", ");
        }
        first = false;
        /* Jinja dictionary, so single quotes and no JSON escaping. A label with
         * a quote in it would break the template, so those are dropped rather
         * than smuggled through. */
        o3e_buf_adds(b, "'");
        for (const char *p = text_to_value ? it->valuestring : it->string; *p; p++) {
            if (*p != '\'' && *p != '\\' && (unsigned char)*p >= 0x20) {
                char c[2] = { *p, '\0' };
                o3e_buf_adds(b, c);
            }
        }
        o3e_buf_adds(b, "': ");
        if (text_to_value) {
            o3e_buf_adds(b, it->string);       /* the number, unquoted */
        } else {
            o3e_buf_adds(b, "'");
            for (const char *p = it->valuestring; *p; p++) {
                if (*p != '\'' && *p != '\\' && (unsigned char)*p >= 0x20) {
                    char c[2] = { *p, '\0' };
                    o3e_buf_adds(b, c);
                }
            }
            o3e_buf_adds(b, "'");
        }
    }
    o3e_buf_adds(b, "} %}");
}

/* The state template: a number on the topic becomes the name of that number,
 * and anything unmapped is shown as it arrived rather than as "None". */
static char *label_state_template(const cJSON *labels, const char *json_path)
{
    o3e_buf_t b;
    o3e_buf_init(&b);
    add_label_map(&b, labels, false);
    if (json_path && json_path[0]) {
        o3e_buf_adds(&b, "{% set v = value_json");
        o3e_buf_adds(&b, json_path);
        o3e_buf_adds(&b, " | string %}");
    } else {
        o3e_buf_adds(&b, "{% set v = value | string %}");
    }
    o3e_buf_adds(&b, "{{ m.get(v, v) }}");
    if (b.oom || !b.buf) {
        o3e_buf_free(&b);
        return NULL;
    }
    char *out = b.buf;
    b.buf = NULL;
    o3e_buf_free(&b);
    return out;
}

/* What the last walk actually put on the broker. Kept apart because
 "published for 19 datapoints" says nothing about whether any of them
 became operable, and a control that never appears looks exactly like a
 datapoint that is read-only. */
static int n_sensors, n_controls;

/* Home Assistant keeps long-term statistics, and admits a sensor to the energy
 * dashboard, only when the state class suits the device class. An energy
 * reading marked `measurement` is rejected outright -- that is what an entity
 * page saying "no statistics found" is reporting.
 *
 * The rolling windows are the exception. Past7Days, PastMonth and PastYear
 * fall as well as rise, and `total_increasing` reads every fall as a counter
 * reset and books the entire new value as fresh consumption. Those get no
 * state class rather than a wrong one: no statistics is a gap, invented
 * kilowatt-hours are a lie. */
static const char *state_class_for(const char *device_class, const char *field)
{
    bool counter = strcmp(device_class, "energy") == 0
                || strcmp(device_class, "duration") == 0;
    if (!counter) {
        return "measurement";
    }
    if (field && strncmp(field, "Past", 4) == 0) {
        return NULL;
    }
    return "total_increasing";
}

static void publish_entity(const mqtt_cfg_t *cfg, const char *dev_id,
                           const char *state_topic, const char *object_id,
                           const char *name, const o3e_node_t *leaf,
                           const char *value_template, bool clear)
{
    char topic[320];
    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config",
             cfg->ha_prefix, dev_id, object_id);
    if (clear) {
        /* An empty retained payload is how Home Assistant is told an entity is
         * gone. It has to target the exact same topic the entity was published
         * on, which is why removal walks the codec tree too. */
        mqtt_pub_raw(topic, "", true);
        n_sensors++;
        return;
    }

    o3e_buf_t b;
    o3e_buf_init(&b);

    o3e_buf_adds(&b, "{\"name\": ");
    o3e_buf_add_json_str(&b, name);
    o3e_buf_adds(&b, ", \"state_topic\": ");
    o3e_buf_add_json_str(&b, state_topic);
    o3e_buf_adds(&b, ", \"unique_id\": ");
    char uid[160];
    snprintf(uid, sizeof(uid), "%s_%s", dev_id, object_id);
    o3e_buf_add_json_str(&b, uid);

    if (value_template) {
        o3e_buf_adds(&b, ", \"value_template\": ");
        o3e_buf_add_json_str(&b, value_template);
    }
    if (leaf && leaf->unit && leaf->unit[0]) {
        o3e_buf_adds(&b, ", \"unit_of_measurement\": ");
        o3e_buf_add_json_str(&b, leaf->unit);
        const char *dc = device_class_for(leaf->unit);
        if (dc) {
            o3e_buf_adds(&b, ", \"device_class\": ");
            o3e_buf_add_json_str(&b, dc);
            const char *sc = state_class_for(dc, leaf->id);
            if (sc) {
                o3e_buf_adds(&b, ", \"state_class\": ");
                o3e_buf_add_json_str(&b, sc);
            }
        }
    }

    char lwt[CFG_TOPIC_MAX + 8];
    snprintf(lwt, sizeof(lwt), "%s/LWT", cfg->base_topic);
    o3e_buf_adds(&b, ", \"availability_topic\": ");
    o3e_buf_add_json_str(&b, lwt);
    o3e_buf_adds(&b, ", \"payload_available\": \"online\""
                     ", \"payload_not_available\": \"offline\"");

    o3e_buf_adds(&b, ", \"device\": {\"identifiers\": [");
    o3e_buf_add_json_str(&b, dev_id);
    o3e_buf_adds(&b, "], \"manufacturer\": \"Viessmann\", \"model\": \"E3 via open3e\""
                     ", \"name\": ");
    /* The base topic, because one household can run more than one of these --
     * one on the storage bus, one on the ventilation unit -- and two devices
     * both called "open3e Gateway" are told apart only by a MAC address. */
    o3e_buf_add_json_str(&b, cfg->base_topic[0] ? cfg->base_topic : "open3e Gateway");
    o3e_buf_adds(&b, ", \"sw_version\": ");
    o3e_buf_add_json_str(&b, o3e_db_version());
    o3e_buf_adds(&b, "}}");

    if (!b.oom && b.buf) {
        mqtt_pub_raw(topic, b.buf, true);
        n_sensors++;
    }
    o3e_buf_free(&b);
}

/* A writable datapoint also gets something to operate it.
 *
 * The command goes to the gateway's open3e-compatible command topic, which
 * takes an object of DID to value. Only one field of that value is set here --
 * a select can send a ventilation stage and nothing else -- and the firmware
 * fills the rest in from the datapoint's current reading before encoding.
 * Without that merge, encoding would fail on the first field the control does
 * not mention, including ones the database only knows as "Unknown1".
 *
 * Limited to the first level: a control for a field nested two deep would have
 * to send a nested object, and a half-specified sub-structure is more likely a
 * mistake than an intention.
 */
static void publish_control(const mqtt_cfg_t *cfg, const char *dev_id,
                            const char *state_topic, const char *object_id,
                            const char *name, const o3e_node_t *leaf,
                            const char *value_template,
                            uint16_t ecu, uint16_t did, const char *path,
                            bool writable, const cJSON *labels,
                            const cJSON *range, bool clear)
{
    if (!writable || !cfg->cmnd_topic[0]) {
        return;
    }
    /* path is "" for a scalar datapoint and "_Field" one level down. Anything
     * with a second separator is nested deeper than a control can address. */
    const char *field = path[0] ? path + 1 : NULL;
    if (field && strchr(field, '_')) {
        return;
    }

    const char *component = NULL;
    if (labels) {
        /* Named numbers: a list of words beats a spin box that accepts 0..255
         * when only three of those values mean anything. */
        component = "select";
    } else if (leaf->kind == O3E_K_ENUM && leaf->list_name) {
        component = "select";
    } else if (leaf->kind == O3E_K_INT || leaf->kind == O3E_K_BYTEVAL) {
        /* Only these two: their byte width bounds the value, so the control
         * can offer the range the datapoint actually accepts. A float has no
         * such bound, and text, raw bytes and timestamps have no sensible
         * control at all. */
        component = "number";
    } else {
        return;
    }

    char topic[352];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s_set/config",
             cfg->ha_prefix, component, dev_id, object_id);

    /* Retract the other kind of control for this leaf.
     *
     * A discovery config is retained, so the broker keeps handing it to Home
     * Assistant until something overwrites it. Giving a number a set of labels
     * turns it from a number into a select -- and without this, the number's
     * config would sit there for ever and the entity would never change. The
     * symptom is that entering labels appears to do nothing at all. */
    {
        char stale[352];
        snprintf(stale, sizeof(stale), "%s/%s/%s/%s_set/config", cfg->ha_prefix,
                 (component[0] == 's') ? "number" : "select", dev_id, object_id);
        mqtt_pub_raw(stale, "", true);
    }

    if (clear) {
        mqtt_pub_raw(topic, "", true);
        n_controls++;
        return;
    }

    /* The value the command carries: a name for a select, a number otherwise.
     * The encoder resolves an enum by its text, which is exactly the option
     * Home Assistant sends back. */
    o3e_buf_t cmdb;
    o3e_buf_init(&cmdb);
    if (labels) {
        /* Home Assistant sends the option's text, the bus wants the number, so
         * the template carries the reverse map. Built on the heap: a label set
         * is unbounded and this used to be a fixed 320 bytes. */
        add_label_map(&cmdb, labels, true);
    }
    char head[160];
    snprintf(head, sizeof(head),
             "{\"mode\": \"write\", \"addr\": \"0x%03X\", \"data\": {\"%u\": ",
             ecu, did);
    o3e_buf_adds(&cmdb, head);
    if (field) {
        o3e_buf_adds(&cmdb, "{\"");
        o3e_buf_adds(&cmdb, field);
        o3e_buf_adds(&cmdb, "\": ");
    }
    if (labels) {
        o3e_buf_adds(&cmdb, "{{ m[value] }}");
    } else if (component[0] == 's') {
        o3e_buf_adds(&cmdb, "\"{{ value }}\"");
    } else {
        o3e_buf_adds(&cmdb, "{{ value }}");
    }
    o3e_buf_adds(&cmdb, field ? "}}}" : "}}");
    if (cmdb.oom || !cmdb.buf) {
        o3e_buf_free(&cmdb);
        return;
    }

    o3e_buf_t b;
    o3e_buf_init(&b);
    o3e_buf_adds(&b, "{\"name\": ");
    o3e_buf_add_json_str(&b, name);
    o3e_buf_adds(&b, ", \"state_topic\": ");
    o3e_buf_add_json_str(&b, state_topic);
    if (value_template) {
        o3e_buf_adds(&b, ", \"value_template\": ");
        o3e_buf_add_json_str(&b, value_template);
    }
    o3e_buf_adds(&b, ", \"command_topic\": ");
    o3e_buf_add_json_str(&b, cfg->cmnd_topic);
    o3e_buf_adds(&b, ", \"command_template\": ");
    o3e_buf_add_json_str(&b, cmdb.buf);
    o3e_buf_free(&cmdb);

    char uid[176];
    snprintf(uid, sizeof(uid), "%s_%s_set", dev_id, object_id);
    o3e_buf_adds(&b, ", \"unique_id\": ");
    o3e_buf_add_json_str(&b, uid);

    if (labels) {
        /* The options are the labels themselves, in the order they were given.
         * A select without options is rejected outright by Home Assistant. */
        o3e_buf_adds(&b, ", \"options\": [");
        const cJSON *it;
        bool first = true;
        cJSON_ArrayForEach(it, labels) {
            if (!cJSON_IsString(it)) {
                continue;
            }
            if (!first) {
                o3e_buf_adds(&b, ", ");
            }
            first = false;
            o3e_buf_add_json_str(&b, it->valuestring);
        }
        o3e_buf_adds(&b, "]");
    } else if (component[0] == 's') {
        o3e_buf_adds(&b, ", \"options\": [");
        size_t count = o3e_db_enum_count(leaf->list_name);
        for (size_t i = 0, written = 0; i < count; i++) {
            int32_t val;
            const char *text = NULL;
            if (!o3e_db_enum_at(leaf->list_name, i, &val, &text) || !text) {
                continue;
            }
            if (written++) {
                o3e_buf_adds(&b, ", ");
            }
            o3e_buf_add_json_str(&b, text);
        }
        o3e_buf_adds(&b, "]");
    } else {
        /* Bounds from the field's own width and sign. Home Assistant defaults
         * a number to 0..100, which would silently forbid most of the range a
         * datapoint actually accepts. */
        double lo, hi;
        double span = 1.0;
        for (uint16_t i = 0; i < leaf->len && i < 4; i++) {
            span *= 256.0;
        }
        if (leaf->signd) {
            hi = span / 2.0 - 1.0;
            lo = -span / 2.0;
        } else {
            hi = span - 1.0;
            lo = 0.0;
        }
        double scale = leaf->scale > 0.0 ? leaf->scale : 1.0;
        lo /= scale;
        hi /= scale;
        /* The operator's limits narrow that range; they never widen it, since
         * the field could not carry the value anyway. */
        const cJSON *rmin = range ? cJSON_GetObjectItemCaseSensitive(range, "min") : NULL;
        const cJSON *rmax = range ? cJSON_GetObjectItemCaseSensitive(range, "max") : NULL;
        if (cJSON_IsNumber(rmin) && rmin->valuedouble > lo) {
            lo = rmin->valuedouble;
        }
        if (cJSON_IsNumber(rmax) && rmax->valuedouble < hi) {
            hi = rmax->valuedouble;
        }
        char nums[128];
        snprintf(nums, sizeof(nums),
                 ", \"min\": %.6g, \"max\": %.6g, \"step\": %.4g, \"mode\": \"box\"",
                 lo, hi, 1.0 / scale);
        o3e_buf_adds(&b, nums);
        if (leaf->unit && leaf->unit[0]) {
            o3e_buf_adds(&b, ", \"unit_of_measurement\": ");
            o3e_buf_add_json_str(&b, leaf->unit);
        }
    }

    char lwt[CFG_TOPIC_MAX + 8];
    snprintf(lwt, sizeof(lwt), "%s/LWT", cfg->base_topic);
    o3e_buf_adds(&b, ", \"availability_topic\": ");
    o3e_buf_add_json_str(&b, lwt);
    o3e_buf_adds(&b, ", \"payload_available\": \"online\""
                     ", \"payload_not_available\": \"offline\"");
    o3e_buf_adds(&b, ", \"device\": {\"identifiers\": [");
    o3e_buf_add_json_str(&b, dev_id);
    o3e_buf_adds(&b, "]}}");

    if (!b.oom && b.buf) {
        mqtt_pub_raw(topic, b.buf, true);
        n_controls++;
    }
    o3e_buf_free(&b);
}

/* Announce one scalar leaf: a sensor, and a control if it can be written. */
static void publish_leaf(const mqtt_cfg_t *cfg, const char *dev_id,
                         const char *state_topic, const o3e_node_t *n,
                         const char *did_name, uint16_t did, uint16_t ecu,
                         const char *path, const char *tmpl,
                         bool flat, bool writable, const cJSON *labels,
                         const cJSON *range, bool clear)
{
        char object_id[192];
        snprintf(object_id, sizeof(object_id), "%03x_%u%s", ecu, did, path);
        char name[192];
        snprintf(name, sizeof(name), "%s%s", did_name, path[0] ? path : "");
        for (char *p = name; *p; p++) {
            if (*p == '_') {
                *p = ' ';
            }
        }

        /* An enum leaf decodes to {"ID": n, "Text": "..."}, so it is not a
           scalar on the wire: flat mode gives it two topics and JSON mode a
           nested object. The Text is the half a person wants, and it is also
           what the encoder accepts back, so both the sensor and the control
           point at it. */
        bool is_enum = (n->kind == O3E_K_ENUM);
        /* Labels only make sense for a bare number. An enum already has names,
           and raw bytes or text have nothing to map. */
        const cJSON *lbl = (n->kind == O3E_K_INT || n->kind == O3E_K_BYTEVAL)
                         ? labels_for(labels, path[0] ? path + 1 : NULL) : NULL;
        const cJSON *rng = (n->kind == O3E_K_INT || n->kind == O3E_K_BYTEVAL)
                         ? range_for(range, path[0] ? path + 1 : NULL) : NULL;

        if (flat) {
            /* Flat mode gives each leaf its own topic; path segments are
               separated by '/' there rather than '_'. */
            char leaf_topic[384];
            char sub[192];
            snprintf(sub, sizeof(sub), "%s", path);
            for (char *p = sub; *p; p++) {
                if (*p == '_') {
                    *p = '/';
                }
            }
            snprintf(leaf_topic, sizeof(leaf_topic), "%s%s%s", state_topic, sub,
                     is_enum ? "/Text" : "");
            char *lt = lbl ? label_state_template(lbl, NULL) : NULL;
            publish_entity(cfg, dev_id, leaf_topic, object_id, name, n, lt, clear);
            publish_control(cfg, dev_id, leaf_topic, object_id, name, n, lt,
                            ecu, did, path, writable, lbl, rng, clear);
            free(lt);
        } else {
            char t[256];
            snprintf(t, sizeof(t), "{{ value_json%s%s }}", tmpl,
                     is_enum ? ".Text" : "");
            char *lt = lbl ? label_state_template(lbl, tmpl) : NULL;
            const char *vt = lt ? lt : ((tmpl[0] || is_enum) ? t : "{{ value }}");
            publish_entity(cfg, dev_id, state_topic, object_id, name, n, vt, clear);
            publish_control(cfg, dev_id, state_topic, object_id, name, n, vt,
                            ecu, did, path, writable, lbl, rng, clear);
            free(lt);
        }
}

/* Walk the codec tree and announce every scalar leaf, since that is what
 * actually reaches a topic. A complex datapoint like FlowTemperatureSensor
 * therefore becomes Actual/Minimum/Maximum/Average entities rather than one
 * opaque one. */
static void walk_leaves(const mqtt_cfg_t *cfg, const char *dev_id,
                        const char *state_topic, const o3e_node_t *n,
                        const char *did_name, uint16_t did, uint16_t ecu,
                        char *path, size_t path_sz, char *tmpl, size_t tmpl_sz,
                        bool flat, bool writable, const cJSON *labels,
                        const cJSON *range, bool clear, int depth)
{
    if (depth > 4) {
        return;   /* deeply nested lists are not useful as entities */
    }

    switch (n->kind) {
    case O3E_K_COMPLEX:
    case O3E_K_SWITCH:
        for (uint16_t i = 0; i < n->n_kids; i++) {
            const o3e_node_t *k = n->kids[i];
            if (k->kind == O3E_K_LIST || k->kind == O3E_K_ARRAY) {
                continue;   /* variable-length; not a sensible fixed entity */
            }
            size_t plen = strlen(path);
            size_t tlen = strlen(tmpl);
            snprintf(path + plen, path_sz - plen, "_%s", k->id ? k->id : "");
            snprintf(tmpl + tlen, tmpl_sz - tlen, ".%s", k->id ? k->id : "");
            walk_leaves(cfg, dev_id, state_topic, k, did_name, did, ecu,
                        path, path_sz, tmpl, tmpl_sz, flat, writable, labels,
                        range, clear, depth + 1);
            path[plen] = '\0';
            tmpl[tlen] = '\0';
        }
        return;

    case O3E_K_LIST:
    case O3E_K_ARRAY:
    case O3E_K_UNKNOWN:
        return;

    default:
        /* Not inlined: walk_leaves recurses, so every byte of frame it needs is
         * paid once per level. The publishing buffers are over a kilobyte and
         * are wanted only at the bottom, where nothing recurses further.
         * Keeping them in their own frame is what stops this from overflowing
         * the MQTT control task's stack. */
        publish_leaf(cfg, dev_id, state_topic, n, did_name, did, ecu,
                     path, tmpl, flat, writable, labels, range, clear);
        return;
    }
}

/* Both publishing and removal walk the same selection and the same codec
 * trees; only the payload differs. Keeping them in one function is what stops
 * removal from targeting topics publishing never created. */
static void ha_disco_walk_selection(bool clear)
{
    n_sensors = n_controls = 0;
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    if (!cfg.enabled || !mqtt_pub_connected()) {
        return;
    }
    if (!clear && !cfg.ha_discovery) {
        return;
    }

    char *raw = app_config_read_file(CFG_POINTS_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    char dev_id[32];
    device_id(dev_id, sizeof(dev_id));

    int count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, root) {
        /* Energy-meter frames live in the same file but have no ECU or DID. */
        if (!sel_is_datapoint(it)) {
            continue;
        }
        /* When clearing, ignore the flags: the entity may exist precisely
         * because one of them was just switched off. */
        if (!clear && (!sel_enabled(it) || !sel_bool(it, "ha", false))) {
            continue;
        }
        uint16_t ecu = sel_u16(it, "ecu", 0);
        uint16_t did = sel_u16(it, "did", 0);
        uint16_t dlen = sel_u16(it, "len", 0);
        const char *mode = sel_str(it, "mode");
        bool flat = mode && strcmp(mode, "flat") == 0;

        char *json = o3e_db_json(did, dlen);
        o3e_node_t *node = json ? o3e_codec_compile(json) : NULL;
        free(json);
        if (!node) {
            continue;
        }

        char state_topic[256];
        mqtt_pub_topic(state_topic, sizeof(state_topic), ecu, did, node->id, "",
                       sel_str(it, "topic"));

        char path[192] = "";
        char tmpl[192] = "";
        /* The datapoint's own access flag decides whether its leaves get a
           control. Sub-fields rarely carry one of their own, so taking it from
           the root is both what the database means and what poller_write_now
           checks before it writes. */
        bool writable = (node->acc == O3E_ACC_RW);
        walk_leaves(&cfg, dev_id, state_topic, node,
                    node->id ? node->id : "datapoint", did, ecu,
                    path, sizeof(path), tmpl, sizeof(tmpl), flat, writable,
                    cJSON_GetObjectItemCaseSensitive(it, "labels"),
                    cJSON_GetObjectItemCaseSensitive(it, "range"), clear, 0);
        o3e_codec_free(node);
        count++;
    }
    cJSON_Delete(root);

    /* Not datapoints, so the walk never reaches them -- announced separately. */
    ha_disco_grid(&cfg, dev_id, clear);
    ha_disco_plant_image(clear);
    ha_disco_contacts(&cfg, dev_id, clear);

    /* Counted separately: "published for 19 datapoints" says nothing about
     * whether any of them became operable, and a control that never appears
     * looks exactly like a datapoint that is read-only. */
    ESP_LOGI(TAG, "discovery %s: %d datapoints, %d sensors, %d controls%s",
             clear ? "cleared" : "published", count, n_sensors, n_controls,
             (!clear && n_controls == 0 && !cfg.cmnd_topic[0])
                 ? " (no command topic configured)" : "");
}

/* The grid hold, as three entities of its own.
 *
 * It is not a datapoint, so the walk above never reaches it: it is a thing the
 * gateway does, not a value the bus carries. A switch to run it, a number for
 * how much, and a countdown -- because something overriding the installation's
 * own regulation should be visible and stoppable from wherever the operator
 * happens to be looking, not only on this device's own page.
 */
static void publish_extra_entity(const mqtt_cfg_t *cfg, const char *dev_id,
                                 const char *component, const char *object,
                                 const char *name, const char *state,
                                 const char *cmd, const char *extra, bool clear)
{
    /* Anything that only reads needs no command topic and counts as a sensor;
     * everything else is an operable control. `cmd` is where a control is
     * told what to do; NULL means the shared command topic. */
    bool is_sensor = strcmp(component, "sensor") == 0 ||
                     strcmp(component, "binary_sensor") == 0;
    if (!cmd) {
        cmd = cfg->cmnd_topic;
    }

    char topic[352];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
             cfg->ha_prefix, component, dev_id, object);
    if (clear) {
        mqtt_pub_raw(topic, "", true);
        *(is_sensor ? &n_sensors : &n_controls) += 1;
        return;
    }
    /* A switch or a number with nowhere to send is furniture: it would sit in
     * Home Assistant looking operable and do nothing. The countdown is worth
     * having either way, since it only reads. */
    if (!is_sensor && !cmd[0]) {
        return;
    }

    char lwt[CFG_TOPIC_MAX + 8];
    snprintf(lwt, sizeof(lwt), "%s/LWT", cfg->base_topic);

    o3e_buf_t b;
    o3e_buf_init(&b);
    o3e_buf_adds(&b, "{\"name\": ");
    o3e_buf_add_json_str(&b, name);
    o3e_buf_adds(&b, ", \"state_topic\": ");
    o3e_buf_add_json_str(&b, state);
    o3e_buf_adds(&b, ", \"unique_id\": ");
    char uid[160];
    snprintf(uid, sizeof(uid), "%s_%s", dev_id, object);
    o3e_buf_add_json_str(&b, uid);
    if (!is_sensor) {
        o3e_buf_adds(&b, ", \"command_topic\": ");
        o3e_buf_add_json_str(&b, cmd);
    }
    o3e_buf_adds(&b, ", ");
    o3e_buf_adds(&b, extra);
    o3e_buf_adds(&b, ", \"availability_topic\": ");
    o3e_buf_add_json_str(&b, lwt);
    o3e_buf_adds(&b, ", \"payload_available\": \"online\""
                     ", \"payload_not_available\": \"offline\"");
    o3e_buf_adds(&b, ", \"device\": {\"identifiers\": [");
    o3e_buf_add_json_str(&b, dev_id);
    o3e_buf_adds(&b, "]}}");
    if (!b.oom && b.buf) {
        mqtt_pub_raw(topic, b.buf, true);
        *(is_sensor ? &n_sensors : &n_controls) += 1;
    }
    o3e_buf_free(&b);
}

/* Everything the hold publishes lives in one JSON message on <base>/hold, so
 * its entities differ only in which field they pick out of it. */
static void publish_hold_entity(const mqtt_cfg_t *cfg, const char *dev_id,
                                const char *component, const char *object,
                                const char *name, const char *extra, bool clear)
{
    char state[CFG_TOPIC_MAX + 8];
    snprintf(state, sizeof(state), "%s/hold", cfg->base_topic);
    publish_extra_entity(cfg, dev_id, component, object, name, state, NULL, extra, clear);
}

static void ha_disco_grid(const mqtt_cfg_t *cfg, const char *dev_id, bool clear)
{
    publish_hold_entity(cfg, dev_id, "switch", "grid_hold",
        "Aus dem Netz laden",
        "\"value_template\": \"{{ 'ON' if value_json.active else 'OFF' }}\", "
        "\"payload_on\": \"{\\\"mode\\\": \\\"grid\\\", \\\"on\\\": true}\", "
        "\"payload_off\": \"{\\\"mode\\\": \\\"grid\\\", \\\"on\\\": false}\", "
        "\"icon\": \"mdi:transmission-tower-import\"", clear);

    char num[320];
    snprintf(num, sizeof(num),
        "\"value_template\": \"{{ value_json.power }}\", "
        "\"command_template\": \"{\\\"mode\\\": \\\"grid\\\", "
        "\\\"power\\\": {{ value }}}\", "
        "\"min\": 100, \"max\": %d, \"step\": 100, \"mode\": \"box\", "
        "\"unit_of_measurement\": \"W\", \"icon\": \"mdi:flash\"",
        GRID_HOLD_MAX_W);
    publish_hold_entity(cfg, dev_id, "number", "grid_power",
                        "Netzladeleistung", num, clear);

    snprintf(num, sizeof(num),
        "\"value_template\": \"{{ value_json.minutes }}\", "
        "\"command_template\": \"{\\\"mode\\\": \\\"grid\\\", "
        "\\\"minutes\\\": {{ value }}}\", "
        "\"min\": 1, \"max\": %d, \"step\": 1, \"mode\": \"box\", "
        "\"unit_of_measurement\": \"min\", \"icon\": \"mdi:timer-outline\"",
        GRID_HOLD_MAX_S / 60);
    publish_hold_entity(cfg, dev_id, "number", "grid_minutes",
                        "Netzladedauer", num, clear);

    publish_hold_entity(cfg, dev_id, "sensor", "grid_remaining",
        "Netzladen Restzeit",
        "\"value_template\": \"{{ value_json.remainingS }}\", "
        "\"unit_of_measurement\": \"s\", \"device_class\": \"duration\", "
        "\"icon\": \"mdi:timer-sand\"", clear);

    /* The storage's own limits, as a mode rather than two numbers: only the
     * extremes are written, so there is nothing to dial. */
    publish_hold_entity(cfg, dev_id, "select", "storage_mode",
        "Speicher-Betriebsart",
        "\"value_template\": \"{{ value_json.storage }}\", "
        "\"command_template\": \"{\\\"mode\\\": \\\"storage\\\", "
        "\\\"storage\\\": \\\"{{ value }}\\\"}\", "
        "\"options\": [\"normal\", \"steht still\", \"nur laden\", "
        "\"nur entladen\"], \"icon\": \"mdi:battery-lock\"", clear);

    publish_hold_entity(cfg, dev_id, "sensor", "storage_remaining",
        "Speicher-Betriebsart Restzeit",
        "\"value_template\": \"{{ value_json.storageRemainingS }}\", "
        "\"unit_of_measurement\": \"s\", \"device_class\": \"duration\", "
        "\"icon\": \"mdi:timer-sand\"", clear);
}

void ha_disco_counts(int *sensors, int *controls)
{
    if (sensors) {
        *sensors = n_sensors;
    }
    if (controls) {
        *controls = n_controls;
    }
}

/* Last known machine state, as the picture shows it. Unknown means moving:
 * until someone has looked, the picture is the plain one. */
static bool s_plant_comp = true;
static bool s_plant_pump = true;

void ha_disco_plant_state(bool compressor_running, bool pump_running)
{
    if (compressor_running == s_plant_comp && pump_running == s_plant_pump) {
        return;
    }
    s_plant_comp = compressor_running;
    s_plant_pump = pump_running;
    ha_disco_plant_image(false);
}

void ha_disco_plant_image(bool clear)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    if (!cfg.enabled || !mqtt_pub_connected()) {
        return;
    }
    char dev_id[32];
    device_id(dev_id, sizeof(dev_id));
    char topic[CFG_TOPIC_MAX + 16];
    snprintf(topic, sizeof(topic), "%s/plant/svg", cfg.base_topic);
    char conf[352];
    snprintf(conf, sizeof(conf), "%s/image/%s/plant/config", cfg.ha_prefix, dev_id);

    char *svg = clear ? NULL : app_config_read_file(CFG_WWW_DIR "/plant.svg");
    /* Only a real picture goes out; a missing file, or the wrong file handed
     * back by a test stub, must not become a retained payload. */
    bool have = svg && strncmp(svg, "<svg", 4) == 0;
    if (!have) {
        free(svg);
        mqtt_pub_raw(topic, "", true);
        mqtt_pub_raw(conf, "", true);
        return;
    }
    /* Idle circuits stand still: a style rule appended inside the document
     * overrides the animation the web interface put there. */
    if (!s_plant_comp || !s_plant_pump) {
        char *end = strstr(svg, "</svg>");
        if (end) {
            const char *rule = (!s_plant_comp && !s_plant_pump)
                ? "<style>.pipe{animation:none}</style>"
                : (!s_plant_comp ? "<style>.hi,.lo{animation:none}</style>"
                                 : "<style>.flow,.ret{animation:none}</style>");
            size_t head = (size_t)(end - svg);
            char *with = malloc(strlen(svg) + strlen(rule) + 1);
            if (with) {
                memcpy(with, svg, head);
                strcpy(with + head, rule);
                strcat(with, end);
                free(svg);
                svg = with;
            }
        }
    }
    mqtt_pub_raw(topic, svg, true);
    free(svg);
    if (!cfg.ha_discovery) {
        return;
    }
    o3e_buf_t b;
    o3e_buf_init(&b);
    o3e_buf_adds(&b, "{\"name\": \"Anlagenschema\", \"image_topic\": ");
    o3e_buf_add_json_str(&b, topic);
    o3e_buf_adds(&b, ", \"content_type\": \"image/svg+xml\", \"unique_id\": ");
    char uid[48];
    snprintf(uid, sizeof(uid), "%s_plant", dev_id);
    o3e_buf_add_json_str(&b, uid);
    o3e_buf_adds(&b, ", \"device\": {\"identifiers\": [");
    o3e_buf_add_json_str(&b, dev_id);
    o3e_buf_adds(&b, "]}}");
    if (!b.oom && b.buf) {
        mqtt_pub_raw(conf, b.buf, true);
    }
    o3e_buf_free(&b);
}

void ha_disco_publish_all(void) { ha_disco_walk_selection(false); }
void ha_disco_clear_all(void)   { ha_disco_walk_selection(true); }

/* The two pins: a binary sensor per input, a switch per output.
 *
 * A doorbell is the reason the inputs exist, and a doorbell is worth exactly
 * one entity: it is on while somebody is pressing and off otherwise. The
 * device class is the user's, because only they know whether the thing on
 * the wire is a bell, a door, a float switch or a fault relay -- and the
 * class is what decides the icon and the wording Home Assistant uses for
 * both states.
 *
 * An output is a switch whose command topic is its own <base>/output/<name>/set
 * rather than the shared command topic: ON and OFF as payloads, nothing to
 * template, and it works the same for whoever is not Home Assistant.
 */
static void ha_disco_contacts(const mqtt_cfg_t *cfg, const char *dev_id, bool clear)
{
    sys_cfg_t sys;
    sys_cfg_get(&sys);

    for (int i = 0; i < CONTACT_COUNT; i++) {
        char object[32];
        snprintf(object, sizeof(object), "contact%d", i + 1);
        bool output = sys.contact[i].mode == CONTACT_MODE_OUTPUT;

        /* Retracted rather than skipped: switching a pin off has to remove
         * its entity, or Home Assistant keeps one that is permanently
         * unavailable and nothing says why. The same pin may have been the
         * other kind a moment ago, so the other component's entity goes too;
         * retracting what was never announced is a harmless empty publish. */
        char topic[352];
        if (clear || !sys.contact[i].enabled || output) {
            snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/%s/config",
                     cfg->ha_prefix, dev_id, object);
            mqtt_pub_raw(topic, "", true);
        }
        if (clear || !sys.contact[i].enabled || !output) {
            snprintf(topic, sizeof(topic), "%s/switch/%s/%s/config",
                     cfg->ha_prefix, dev_id, object);
            mqtt_pub_raw(topic, "", true);
        }
        if (clear || !sys.contact[i].enabled) {
            if (clear) {
                *(output ? &n_controls : &n_sensors) += 1;
            }
            continue;
        }

        char slug[CONTACT_NAME_MAX * 2];
        contact_slug(&sys.contact[i], i, slug, sizeof(slug));
        const char *name = sys.contact[i].name[0] ? sys.contact[i].name
                                                  : contact_default_name(&sys.contact[i], i);
        char state[CFG_TOPIC_MAX + sizeof(slug) + 16];

        if (output) {
            snprintf(state, sizeof(state), "%s/output/%s", cfg->base_topic, slug);
            char cmd[sizeof(state) + 4];
            snprintf(cmd, sizeof(cmd), "%s/set", state);
            publish_extra_entity(cfg, dev_id, "switch", object, name, state, cmd,
                                 "\"payload_on\": \"ON\", \"payload_off\": \"OFF\", "
                                 "\"icon\": \"mdi:electric-switch\"", false);
            continue;
        }

        snprintf(state, sizeof(state), "%s/contact/%s", cfg->base_topic, slug);
        char extra[192];
        int o = snprintf(extra, sizeof(extra),
                         "\"payload_on\": \"ON\", \"payload_off\": \"OFF\"");
        if (sys.contact[i].device_class[0]) {
            snprintf(extra + o, sizeof(extra) - o, ", \"device_class\": \"%s\"",
                     sys.contact[i].device_class);
        }
        publish_extra_entity(cfg, dev_id, "binary_sensor", object, name, state,
                             NULL, extra, false);
    }
}
