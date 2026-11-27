#include "mqtt_cmnd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "can_port.h"
#include "contact.h"
#include "e3_scan.h"
#include "hold.h"
#include "mqtt_pub.h"
#include "o3e_db.h"
#include "o3e_json.h"
#include "poller.h"

static const char *TAG = "cmnd";

static void reply_error(const char *fmt, ...)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    char topic[CFG_TOPIC_MAX + 8];
    snprintf(topic, sizeof(topic), "%s/ERR", cfg.base_topic);

    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ESP_LOGW(TAG, "%s", msg);
    mqtt_pub_raw(topic, msg, false);
}

/* open3e accepts the ECU either as "0x680" or as a plain number, and omits it
 * entirely for single-ECU systems. */
static bool parse_addr(const cJSON *addr, uint16_t dflt, uint16_t *out)
{
    if (!addr) {
        *out = dflt;
        return true;
    }
    if (cJSON_IsNumber(addr) && addr->valuedouble >= 0 &&
        addr->valuedouble <= 0x7ef && addr->valuedouble == (uint16_t)addr->valuedouble) {
        *out = (uint16_t)addr->valuedouble;
        return true;
    }
    if (cJSON_IsString(addr) && addr->valuestring && addr->valuestring[0] &&
        addr->valuestring[0] != '-' && addr->valuestring[0] != '+') {
        char *end;
        unsigned long value = strtoul(addr->valuestring, &end, 0);
        if (!*end && value <= 0x7ef) {
            *out = (uint16_t)value;
            return true;
        }
    }
    return false;
}

/* Falls back to the first ECU the scan found, which is what open3e does when
 * no address is given. */
static uint16_t default_ecu(void)
{
    uint16_t addr = 0x680;
    char *sys = app_config_read_file(CFG_SYSTEM_PATH);
    if (!sys) {
        return addr;
    }
    cJSON *root = cJSON_Parse(sys);
    free(sys);
    if (root) {
        const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "devices"), 0);
        const cJSON *a = first ? cJSON_GetObjectItem(first, "addr") : NULL;
        if (cJSON_IsNumber(a)) {
            addr = (uint16_t)a->valuedouble;
        }
        cJSON_Delete(root);
    }
    return addr;
}

typedef struct {
    uint16_t ecu;
    uint16_t did;
    char field[96];
} cmnd_address_t;

static bool parse_number(const char *text, unsigned long max, uint16_t *out)
{
    if (!text || !*text || *text == '-' || *text == '+') {
        return false;
    }
    char *end;
    unsigned long n = strtoul(text, &end, 0);
    if (*end || n > max) {
        return false;
    }
    *out = (uint16_t)n;
    return true;
}

static bool resolve_did(const char *text, uint16_t *did)
{
    if (parse_number(text, UINT16_MAX, did)) {
        return true;
    }
    for (size_t i = 0; i < o3e_db_count(); i++) {
        const o3e_dp_entry_t *entry = o3e_db_at(i);
        if (entry && strcasecmp(o3e_db_name(entry), text) == 0) {
            *did = entry->did;
            return true;
        }
    }
    return false;
}

static bool parse_address(const char *text, uint16_t default_addr, cmnd_address_t *out)
{
    char buf[192];
    if (!text || strlen(text) >= sizeof(buf)) {
        return false;
    }
    snprintf(buf, sizeof(buf), "%s", text);
    out->ecu = default_addr;
    out->field[0] = '\0';
    char *part = buf;
    char *dot = strchr(part, '.');
    if (dot && part[0] == '0' && (part[1] == 'x' || part[1] == 'X')) {
        *dot = '\0';
        if (!parse_number(part, 0x7ef, &out->ecu)) {
            return false;
        }
        part = dot + 1;
        dot = strchr(part, '.');
    }
    if (dot) {
        *dot++ = '\0';
        if (!*dot || strchr(dot, '.') || strlen(dot) >= sizeof(out->field)) {
            return false;
        }
        snprintf(out->field, sizeof(out->field), "%s", dot);
    }
    return resolve_did(part, &out->did);
}

static const cJSON *find_field(const cJSON *value, const char *field)
{
    if (!cJSON_IsObject(value)) {
        return NULL;
    }
    uint16_t index;
    if (parse_number(field, UINT16_MAX, &index)) {
        return cJSON_GetArrayItem(value, index);
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, value) {
        if (item->string && strcasecmp(item->string, field) == 0) {
            return item;
        }
    }
    return NULL;
}

static void command_topic(char *topic, size_t size, const cmnd_address_t *addr,
                          const char *name)
{
    char device[48];
    poller_device_name(addr->ecu, device, sizeof(device));
    mqtt_pub_topic(topic, size, addr->ecu, addr->did, name, device, NULL);
}

static void read_one(const cmnd_address_t *addr, bool raw, bool pure)
{
    char topic[256];
    const o3e_dp_entry_t *entry = o3e_db_find(addr->did);
    if (raw || pure) {
        uint8_t *buf = malloc(ISOTP_MAX_PAYLOAD);
        if (!buf) {
            reply_error("read: out of memory");
            return;
        }
        size_t len = 0;
        uds_result_t result = can_read_did(addr->ecu, addr->did, buf,
                                           ISOTP_MAX_PAYLOAD, &len, UDS_P2_MS);
        if (result.err != UDS_OK) {
            reply_error("read 0x%03X.%u failed: %s", addr->ecu, addr->did,
                        uds_strerror(result));
            free(buf);
            return;
        }
        const char *name = entry ? o3e_db_name(entry) : NULL;
        char pure_name[48];
        if (pure || !name) {
            snprintf(pure_name, sizeof(pure_name), "DID_%u:len=%u",
                     addr->did, (unsigned)len);
            name = pure_name;
        }
        o3e_node_t *node = NULL;
        if (addr->field[0]) {
            char *description = o3e_db_json(addr->did, (uint16_t)len);
            node = description ? o3e_codec_compile(description) : NULL;
            free(description);
            size_t offset = 0;
            const o3e_node_t *selected = NULL;
            uint16_t index;
            bool by_index = parse_number(addr->field, UINT16_MAX, &index);
            if (node && node->kind == O3E_K_COMPLEX) {
                for (uint16_t i = 0; i < node->n_kids; i++) {
                    const o3e_node_t *kid = node->kids[i];
                    if ((by_index && i == index) ||
                        (!by_index && kid->id && strcasecmp(kid->id, addr->field) == 0)) {
                        selected = kid;
                        break;
                    }
                    offset += kid->len;
                }
            }
            if (!selected || offset + selected->len > len) {
                reply_error("read 0x%03X.%u: unknown field '%s'",
                            addr->ecu, addr->did, addr->field);
                o3e_codec_free(node);
                free(buf);
                return;
            }
            name = selected->id;
            memmove(buf, buf + offset, selected->len);
            len = selected->len;
        }
        char *hex = malloc(len * 2 + 1);
        if (!hex) {
            reply_error("read: out of memory");
        } else {
            static const char digits[] = "0123456789abcdef";
            for (size_t i = 0; i < len; i++) {
                hex[2 * i] = digits[buf[i] >> 4];
                hex[2 * i + 1] = digits[buf[i] & 15];
            }
            hex[len * 2] = '\0';
            command_topic(topic, sizeof(topic), addr, name);
            if (!mqtt_pub_raw(topic, hex, false)) {
                reply_error("read: MQTT publish failed");
            }
            free(hex);
        }
        o3e_codec_free(node);
        free(buf);
        return;
    }
    char err[128] = "";
    char *json = poller_read_now(addr->ecu, addr->did, err, sizeof(err));
    if (!json) {
        reply_error("read 0x%03X.%u failed: %s", addr->ecu, addr->did, err);
        return;
    }
    char unknown_name[24];
    snprintf(unknown_name, sizeof(unknown_name), "DID_%u", addr->did);
    const char *name = entry ? o3e_db_name(entry) : unknown_name;
    cJSON *value = NULL;
    char *selected_json = NULL;
    if (addr->field[0]) {
        value = cJSON_Parse(json);
        const cJSON *field = find_field(value, addr->field);
        if (!field || !field->string || !(selected_json = cJSON_PrintUnformatted(field))) {
            reply_error("read 0x%03X.%u: unknown or invalid field '%s'",
                        addr->ecu, addr->did, addr->field);
            cJSON_Delete(value);
            free(json);
            return;
        }
        name = field->string;
    }
    command_topic(topic, sizeof(topic), addr, name);
    if (!mqtt_pub_raw(topic, selected_json ? selected_json : json, false)) {
        reply_error("read: MQTT publish failed");
    }
    free(selected_json);
    cJSON_Delete(value);
    free(json);
}

static void read_text(uint16_t ecu, const char *text, bool raw, bool pure)
{
    const char *list = strstr(text, ".[");
    if (list) {
        size_t prefix_len = (size_t)(list - text);
        size_t text_len = strlen(text);
        if (!prefix_len || prefix_len > 16 || text[text_len - 1] != ']') {
            reply_error("invalid DID address '%s'", text);
            return;
        }
        char prefix[18];
        memcpy(prefix, text, prefix_len);
        prefix[prefix_len] = '\0';
        uint16_t list_ecu;
        if ((prefix[0] != '0' || (prefix[1] != 'x' && prefix[1] != 'X')) ||
            !parse_number(prefix, 0x7ef, &list_ecu)) {
            reply_error("invalid ECU address '%s'", prefix);
            return;
        }
        size_t list_len = text_len - prefix_len - 3;
        if (!list_len || list_len >= 192) {
            reply_error("invalid DID list '%s'", text);
            return;
        }
        char copy[192];
        memcpy(copy, list + 2, list_len);
        copy[list_len] = '\0';
        bool have_item = false;
        for (size_t i = 0; i <= list_len; i++) {
            if (copy[i] == ',' || copy[i] == '\0') {
                if (!have_item) {
                    reply_error("invalid DID list '%s'", text);
                    return;
                }
                have_item = false;
            } else if (copy[i] != ' ') {
                have_item = true;
            }
        }
        char *save = NULL;
        for (char *item = strtok_r(copy, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
            while (*item == ' ') item++;
            char *end = item + strlen(item);
            while (end > item && end[-1] == ' ') *--end = '\0';
            read_text(list_ecu, item, raw, pure);
        }
        return;
    }
    cmnd_address_t addr;
    if (!parse_address(text, ecu, &addr)) {
        reply_error("invalid DID address '%s'", text);
        return;
    }
    read_one(&addr, raw, pure);
}

static void do_read(uint16_t ecu, const cJSON *data, bool raw, bool pure)
{
    if (!cJSON_IsArray(data)) {
        reply_error("read expects an array of DIDs");
        return;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, data) {
        char number[16];
        if (cJSON_IsNumber(it) && it->valuedouble >= 0 &&
            it->valuedouble <= UINT16_MAX && it->valuedouble == (uint16_t)it->valuedouble) {
            snprintf(number, sizeof(number), "%u", (unsigned)it->valuedouble);
            read_text(ecu, number, raw, pure);
        } else if (cJSON_IsString(it)) {
            read_text(ecu, it->valuestring, raw, pure);
        } else {
            reply_error("invalid DID in read request");
        }
    }
}

/* {"mode":"grid","addr":"0x6A1","watts":-2000,"seconds":1800}
 * {"mode":"grid","addr":"0x6A1","charge":2000,"seconds":1800}
 * {"mode":"grid","stop":true}
 *
 * "watts" is the grid setpoint as written; "charge" is power into the
 * storage, with the setpoint re-derived from the house load (hold.h).
 *
 * Negative watts draw from the grid. Reachable from MQTT on purpose, unlike
 * forcing a single write: this is meant to be driven by a tariff, and it is
 * bounded by its own caps and its own deadline rather than by who asked. The
 * caps live in grid_hold.c; nothing here can widen them. */
static void do_grid(uint16_t ecu, const cJSON *root)
{
    char err[128] = "";

    /* "power" and "minutes" adjust the stored settings -- what a bare switch
     * means when it is turned on. Positive watts, reading as "draw this much
     * from the grid"; the sign belongs to the datapoint, not to the operator. */
    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "power");
    const cJSON *jm = cJSON_GetObjectItemCaseSensitive(root, "minutes");
    if (cJSON_IsNumber(jp) || cJSON_IsNumber(jm)) {
        sys_cfg_t sys;
        sys_cfg_get(&sys);
        if (cJSON_IsNumber(jp)) {
            int w = (int)jp->valuedouble;
            sys.grid_watts = (uint16_t)(w < 0 ? -w : w);
        }
        if (cJSON_IsNumber(jm)) {
            sys.grid_minutes = (uint16_t)jm->valuedouble;
        }
        /* Only an explicitly addressed message may change which unit is meant;
         * without "addr" the caller got a default and did not choose it. */
        if (cJSON_GetObjectItemCaseSensitive(root, "addr")) {
            sys.grid_ecu = ecu;
        }
        sys_cfg_set(&sys);

        grid_hold_status_t st;
        grid_hold_status(&st);
        if (st.active) {
            /* Restart at the new value: a setting that only takes effect the
             * next time round is a trap. */
            if (!grid_hold_switch(true, err, sizeof(err))) {
                reply_error("grid hold refused: %s", err);
            }
        } else {
            hold_publish();
        }
        return;
    }

    const cJSON *on = cJSON_GetObjectItemCaseSensitive(root, "on");
    if (cJSON_IsBool(on)) {
        if (!grid_hold_switch(cJSON_IsTrue(on), err, sizeof(err))) {
            reply_error("grid hold refused: %s", err);
        }
        return;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "stop"))) {
        grid_hold_stop();
        return;
    }

    const cJSON *jw = cJSON_GetObjectItemCaseSensitive(root, "watts");
    const cJSON *jc = cJSON_GetObjectItemCaseSensitive(root, "charge");
    const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "seconds");
    if (cJSON_IsNumber(jc) && cJSON_IsNumber(js)) {
        double w = jc->valuedouble < 0 ? -jc->valuedouble : jc->valuedouble;
        if (!charge_hold_start(ecu, (uint16_t)w, (uint32_t)js->valuedouble,
                               err, sizeof(err))) {
            reply_error("charge hold refused: %s", err);
        }
        return;
    }
    if (!cJSON_IsNumber(jw) || !cJSON_IsNumber(js)) {
        reply_error("grid expects \"watts\" or \"charge\" and \"seconds\", "
                    "\"on\": true/false, \"stop\": true, or \"power\"/\"minutes\"");
        return;
    }
    if (!grid_hold_start(ecu, (int16_t)jw->valuedouble,
                         (uint32_t)js->valuedouble, err, sizeof(err))) {
        reply_error("grid hold refused: %s", err);
    }
}

/* {"mode":"storage","addr":"0x6A1","storage":"steht still","seconds":1800}
 *
 * "normal" ends the hold and hands the limits back to the manager. The other
 * three write only zero and the value the manager itself uses, so none of them
 * needs the scale of that datapoint to be known. */
static void do_storage(uint16_t ecu, const cJSON *root)
{
    const cJSON *jm = cJSON_GetObjectItemCaseSensitive(root, "storage");
    storage_mode_t mode;
    if (!cJSON_IsString(jm) || !storage_mode_parse(jm->valuestring, &mode)) {
        reply_error("storage expects \"storage\": one of normal, steht still, "
                    "nur laden, nur entladen");
        return;
    }
    const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "seconds");
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    uint32_t seconds = cJSON_IsNumber(js) ? (uint32_t)js->valuedouble
                                          : (uint32_t)sys.grid_minutes * 60;
    if (!cJSON_GetObjectItemCaseSensitive(root, "addr") && sys.grid_ecu) {
        ecu = sys.grid_ecu;
    }
    char err[128] = "";
    if (!storage_hold_start(ecu, mode, seconds, err, sizeof(err))) {
        reply_error("storage hold refused: %s", err);
    }
}

/* {"mode": "output", "gpio": 1, "on": true} -- the same thing the set topic
 * does, for automations that already talk to the command topic. */
static void do_output(const cJSON *root)
{
    const cJSON *jg = cJSON_GetObjectItem(root, "gpio");
    const cJSON *jo = cJSON_GetObjectItem(root, "on");
    if (!cJSON_IsNumber(jg) || !cJSON_IsBool(jo)) {
        reply_error("output: \"gpio\" (number) and \"on\" (bool) are required");
        return;
    }
    int gpio = (int)jg->valuedouble;
    for (int i = 0; i < CONTACT_COUNT; i++) {
        if (CONTACT_PINS[i] == gpio) {
            if (!contact_output_set(i, cJSON_IsTrue(jo))) {
                reply_error("output: GPIO%d is not an enabled output", gpio);
            }
            return;
        }
    }
    reply_error("output: no such pin GPIO%d", gpio);
}

static void write_one(uint16_t ecu, const char *key, const cJSON *input,
                      bool parse_string)
{
    cmnd_address_t addr;
    if (!parse_address(key, ecu, &addr)) {
        reply_error("invalid DID address '%s'", key);
        return;
    }
    cJSON *value = NULL;
    if (parse_string && cJSON_IsString(input)) {
        value = cJSON_ParseWithOpts(input->valuestring, NULL, true);
    }
    if (!value) {
        value = cJSON_Duplicate(input, true);
    }
    if (!value) {
        reply_error("write: out of memory");
        return;
    }
    if (addr.field[0]) {
        char err[128] = "";
        char *current = poller_read_now(addr.ecu, addr.did, err, sizeof(err));
        cJSON *object = current ? cJSON_Parse(current) : NULL;
        free(current);
        const cJSON *field = find_field(object, addr.field);
        if (!field || !field->string) {
            reply_error("write 0x%03X.%u: field '%s' unavailable%s%s",
                        addr.ecu, addr.did, addr.field, err[0] ? ": " : "", err);
            cJSON_Delete(object);
            cJSON_Delete(value);
            return;
        }
        cJSON *partial = cJSON_CreateObject();
        if (!partial || !cJSON_AddItemToObject(partial, field->string, value)) {
            reply_error("write: out of memory");
            cJSON_Delete(partial);
            cJSON_Delete(object);
            cJSON_Delete(value);
            return;
        }
        cJSON_Delete(object);
        value = partial;
    }
    char *json = cJSON_PrintUnformatted(value);
    cJSON_Delete(value);
    if (!json) {
        reply_error("write: out of memory");
        return;
    }
    char err[192] = "";
    if (!poller_write_now(addr.ecu, addr.did, json, false, err, sizeof(err))) {
        reply_error("write 0x%03X.%u failed: %s", addr.ecu, addr.did, err);
    } else {
        ESP_LOGI(TAG, "wrote 0x%03X.%u via MQTT", addr.ecu, addr.did);
    }
    free(json);
}

static void do_write(uint16_t ecu, const cJSON *data)
{
    if (!cJSON_IsArray(data) && !cJSON_IsObject(data)) {
        reply_error("write expects an array of [did, value] pairs or a DID object");
        return;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, data) {
        if (cJSON_IsObject(data)) {
            write_one(ecu, it->string, it, false);
        } else {
            const cJSON *key = cJSON_GetArrayItem(it, 0);
            const cJSON *value = cJSON_GetArrayItem(it, 1);
            if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) != 2 || !value) {
                reply_error("write expects [did, value] pairs");
                continue;
            }
            if (cJSON_IsString(key)) {
                write_one(ecu, key->valuestring, value, true);
            } else if (cJSON_IsNumber(key) && key->valuedouble >= 0 &&
                       key->valuedouble <= UINT16_MAX &&
                       key->valuedouble == (uint16_t)key->valuedouble) {
                char number[16];
                snprintf(number, sizeof(number), "%u", (unsigned)key->valuedouble);
                write_one(ecu, number, value, true);
            } else {
                reply_error("invalid DID in write request");
            }
        }
    }
}

static void do_system(void)
{
    char *scan = e3_scan_result_json();
    cJSON *root = scan ? cJSON_Parse(scan) : NULL;
    free(scan);
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsArray(devices)) {
        reply_error("system: no valid scan result; scan the bus first");
        cJSON_Delete(root);
        return;
    }
    o3e_buf_t out;
    o3e_buf_init(&out);
    o3e_buf_adds(&out, "{\"devices\":[");
    bool first_device = true;
    const cJSON *device;
    cJSON_ArrayForEach(device, devices) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(device, "addr");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(device, "prop");
        const cJSON *serial = cJSON_GetObjectItemCaseSensitive(device, "vin");
        const cJSON *sw = cJSON_GetObjectItemCaseSensitive(device, "sw");
        const cJSON *hw = cJSON_GetObjectItemCaseSensitive(device, "hw");
        const cJSON *dids = cJSON_GetObjectItemCaseSensitive(device, "dids");
        if (!cJSON_IsNumber(id) || id->valuedouble < 0 || id->valuedouble > 0x7ef ||
            !cJSON_IsString(name) || !cJSON_IsString(serial) || !cJSON_IsString(sw) ||
            !cJSON_IsString(hw) || !cJSON_IsArray(dids)) {
            reply_error("system: invalid device in scan result");
            o3e_buf_free(&out);
            cJSON_Delete(root);
            return;
        }
        uint16_t ecu = (uint16_t)id->valuedouble;
        char number[32], device_name[48];
        const cJSON *configured_name = cJSON_GetObjectItemCaseSensitive(device, "name");
        snprintf(device_name, sizeof(device_name), "%s",
                 cJSON_IsString(configured_name) ? configured_name->valuestring : "");
        snprintf(number, sizeof(number), "%u", ecu);
        o3e_buf_adds(&out, first_device ? "{\"id\":" : ",{\"id\":");
        first_device = false;
        o3e_buf_adds(&out, number);
        o3e_buf_adds(&out, ",\"name\":");
        o3e_buf_add_json_str(&out, name->valuestring);
        o3e_buf_adds(&out, ",\"serial_number\":");
        o3e_buf_add_json_str(&out, serial->valuestring);
        o3e_buf_adds(&out, ",\"software_version\":");
        o3e_buf_add_json_str(&out, sw->valuestring);
        o3e_buf_adds(&out, ",\"hardware_version\":");
        o3e_buf_add_json_str(&out, hw->valuestring);
        o3e_buf_adds(&out, ",\"features\":[");
        bool first_feature = true;
        const cJSON *pair;
        cJSON_ArrayForEach(pair, dids) {
            const cJSON *did = cJSON_GetArrayItem(pair, 0);
            if (!cJSON_IsNumber(did) || did->valuedouble < 0 || did->valuedouble > UINT16_MAX) {
                reply_error("system: invalid DID in scan result");
                o3e_buf_free(&out);
                cJSON_Delete(root);
                return;
            }
            uint16_t did_num = (uint16_t)did->valuedouble;
            const o3e_dp_entry_t *entry = o3e_db_find(did_num);
            if (!entry) {
                continue;
            }
            char topic[256];
            mqtt_pub_topic(topic, sizeof(topic), ecu, did_num, o3e_db_name(entry),
                           device_name, NULL);
            snprintf(number, sizeof(number), "%u", did_num);
            o3e_buf_adds(&out, first_feature ? "{\"id\":" : ",{\"id\":");
            first_feature = false;
            o3e_buf_adds(&out, number);
            o3e_buf_adds(&out, ",\"topic\":");
            o3e_buf_add_json_str(&out, topic);
            o3e_buf_addc(&out, '}');
        }
        o3e_buf_adds(&out, "]}");
    }
    o3e_buf_adds(&out, "]}");
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    char topic[CFG_TOPIC_MAX + 8];
    snprintf(topic, sizeof(topic), "%s/system", cfg.base_topic);
    if (out.oom || !out.buf) {
        reply_error("system: out of memory");
    } else if (!mqtt_pub_raw(topic, out.buf, false)) {
        reply_error("system: MQTT publish failed");
    }
    o3e_buf_free(&out);
    cJSON_Delete(root);
}

static void do_read_all(uint16_t ecu)
{
    char *scan = e3_scan_result_json();
    cJSON *root = scan ? cJSON_Parse(scan) : NULL;
    free(scan);
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsArray(devices)) {
        reply_error("read-all: no valid scan result; scan the bus first");
        cJSON_Delete(root);
        return;
    }

    const cJSON *device;
    const cJSON *dids = NULL;
    cJSON_ArrayForEach(device, devices) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(device, "addr");
        if (cJSON_IsNumber(id) && id->valuedouble == ecu) {
            dids = cJSON_GetObjectItemCaseSensitive(device, "dids");
            break;
        }
    }
    if (!device) {
        reply_error("read-all: ECU 0x%03X not found in scan result", ecu);
        cJSON_Delete(root);
        return;
    }
    if (!cJSON_IsArray(dids) || cJSON_GetArraySize(dids) == 0) {
        reply_error("read-all: no DIDs for ECU 0x%03X; scan the device first", ecu);
        cJSON_Delete(root);
        return;
    }

    const cJSON *pair;
    cJSON_ArrayForEach(pair, dids) {
        const cJSON *did = cJSON_GetArrayItem(pair, 0);
        const cJSON *len = cJSON_GetArrayItem(pair, 1);
        if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) != 2 ||
            !cJSON_IsNumber(len) || len->valuedouble < 0 ||
            len->valuedouble > UINT16_MAX ||
            len->valuedouble != (uint16_t)len->valuedouble ||
            !cJSON_IsNumber(did) ||
            did->valuedouble < 0 || did->valuedouble > UINT16_MAX ||
            did->valuedouble != (uint16_t)did->valuedouble) {
            reply_error("read-all: invalid DID in scan result for ECU 0x%03X", ecu);
            cJSON_Delete(root);
            return;
        }
    }
    size_t count = (size_t)cJSON_GetArraySize(dids);
    uint16_t *numbers = malloc(count * sizeof(*numbers));
    if (!numbers) {
        reply_error("read-all: out of memory");
        cJSON_Delete(root);
        return;
    }
    size_t i = 0;
    cJSON_ArrayForEach(pair, dids) {
        numbers[i++] = (uint16_t)cJSON_GetArrayItem(pair, 0)->valuedouble;
    }
    cJSON_Delete(root);
    for (i = 0; i < count; i++) {
        cmnd_address_t addr = { .ecu = ecu, .did = numbers[i] };
        read_one(&addr, false, false);
    }
    free(numbers);
}

/* Commands run on their own task.
 *
 * A read or write can wait seconds for the CAN bus; doing that in the MQTT
 * event callback would stall the client's own task, stop its keepalive and
 * drop the connection under exactly the load it exists to handle. */
#define CMND_MAX_PAYLOAD 1024

typedef struct {
    char payload[CMND_MAX_PAYLOAD];
} cmnd_job_t;

static QueueHandle_t cmnd_q;

static void execute(const char *payload);

static void cmnd_task(void *arg)
{
    (void)arg;
    cmnd_job_t job;
    for (;;) {
        if (xQueueReceive(cmnd_q, &job, portMAX_DELAY) == pdTRUE) {
            execute(job.payload);
        }
    }
}

/* <base>/output/<slug>/set with ON or OFF. Handled right here on the client's
 * task: it only records a wish for the pin task, nothing blocks. */
static bool output_set_topic(const mqtt_cfg_t *cfg, const char *topic, int topic_len,
                             const char *data, int data_len)
{
    char prefix[CFG_TOPIC_MAX + 16];
    int pl = snprintf(prefix, sizeof(prefix), "%s/output/", cfg->base_topic);
    if (topic_len <= pl + 4 || strncmp(topic, prefix, (size_t)pl) != 0 ||
        strncmp(topic + topic_len - 4, "/set", 4) != 0) {
        return false;
    }
    char slug[CONTACT_NAME_MAX * 2];
    int sl = topic_len - pl - 4;
    if (sl <= 0 || sl >= (int)sizeof(slug)) {
        return true;
    }
    memcpy(slug, topic + pl, (size_t)sl);
    slug[sl] = '\0';
    bool on;
    if (!contact_parse_onoff(data, (size_t)(data_len > 0 ? data_len : 0), &on)) {
        reply_error("output %s: payload must be ON or OFF", slug);
    } else if (!contact_output_set_by_slug(slug, on)) {
        reply_error("output %s: no enabled output by that name", slug);
    }
    return true;
}

void mqtt_cmnd_dispatch(const char *topic, int topic_len,
                        const char *data, int data_len)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    if (output_set_topic(&cfg, topic, topic_len, data, data_len)) {
        return;
    }
    if ((int)strlen(cfg.cmnd_topic) != topic_len ||
        strncmp(cfg.cmnd_topic, topic, (size_t)topic_len) != 0) {
        return;
    }
    /* An empty message is nobody's command: it is how a retained message is
     * deleted, and what ioBroker sends when its tree is tidied up. Answering
     * it with an error only leaves a puzzling ERR behind (discussion #23). */
    if (data_len <= 0) {
        return;
    }
    if (data_len >= CMND_MAX_PAYLOAD) {
        reply_error("command payload must be 1..%d bytes", CMND_MAX_PAYLOAD - 1);
        return;
    }

    if (!cmnd_q) {
        cmnd_q = xQueueCreate(4, sizeof(cmnd_job_t));
        if (!cmnd_q) {
            reply_error("could not create command queue");
            return;
        }
        /* 6 KiB: decoding recurses through the codec tree and parses JSON. */
        if (xTaskCreate(cmnd_task, "mqttcmd", 6144, NULL, 4, NULL) != pdPASS) {
            vQueueDelete(cmnd_q);
            cmnd_q = NULL;
            reply_error("could not start command task");
            return;
        }
    }

    cmnd_job_t job;
    memcpy(job.payload, data, (size_t)data_len);
    job.payload[data_len] = '\0';
    if (xQueueSend(cmnd_q, &job, 0) != pdTRUE) {
        reply_error("busy: a previous command is still running");
    }
}

static void execute(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        reply_error("bad payload: not valid JSON");
        return;
    }

    const cJSON *mode = cJSON_GetObjectItem(root, "mode");
    const cJSON *jdata = cJSON_GetObjectItem(root, "data");
    uint16_t ecu;

    if (!cJSON_IsString(mode)) {
        reply_error("bad payload: \"mode\" is required");
    } else if (!parse_addr(cJSON_GetObjectItem(root, "addr"), default_ecu(), &ecu)) {
        reply_error("bad payload: invalid ECU address");
    } else if (strcmp(mode->valuestring, "read") == 0 ||
               strcmp(mode->valuestring, "read-json") == 0) {
        do_read(ecu, jdata, false, false);
    } else if (strcmp(mode->valuestring, "read-raw") == 0 ||
               strcmp(mode->valuestring, "read-pure") == 0) {
        do_read(ecu, jdata, true, strcmp(mode->valuestring, "read-pure") == 0);
    } else if (strcmp(mode->valuestring, "read-all") == 0 ||
               strcmp(mode->valuestring, "read-all-json") == 0) {
        do_read_all(ecu);
    } else if (strcmp(mode->valuestring, "write") == 0) {
        do_write(ecu, jdata);
    } else if (strcmp(mode->valuestring, "system") == 0) {
        do_system();
    } else if (strcmp(mode->valuestring, "grid") == 0) {
        do_grid(ecu, root);
    } else if (strcmp(mode->valuestring, "storage") == 0) {
        do_storage(ecu, root);
    } else if (strcmp(mode->valuestring, "output") == 0) {
        do_output(root);
    } else if (strcmp(mode->valuestring, "write-raw") == 0 ||
               strcmp(mode->valuestring, "write-sid77") == 0 ||
               strcmp(mode->valuestring, "write-raw-sid77") == 0) {
        reply_error("mode '%s' is not supported by this gateway", mode->valuestring);
    } else {
        reply_error("bad mode '%s'; supported: read, read-json, read-raw, read-pure, "
                    "read-all, read-all-json, write, system, grid, storage, output",
                    mode->valuestring);
    }
    cJSON_Delete(root);
}
