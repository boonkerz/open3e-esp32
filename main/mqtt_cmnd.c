#include "mqtt_cmnd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "contact.h"
#include "hold.h"
#include "mqtt_pub.h"
#include "o3e_db.h"
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
static uint16_t parse_addr(const cJSON *addr, uint16_t dflt)
{
    if (cJSON_IsNumber(addr)) {
        return (uint16_t)addr->valuedouble;
    }
    if (cJSON_IsString(addr) && addr->valuestring) {
        return (uint16_t)strtol(addr->valuestring, NULL, 0);
    }
    return dflt;
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

static void do_read(uint16_t ecu, const cJSON *data)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);

    const cJSON *it;
    cJSON_ArrayForEach(it, data) {
        if (!cJSON_IsNumber(it)) {
            continue;
        }
        uint16_t did = (uint16_t)it->valuedouble;
        char err[128] = "";
        char *json = poller_read_now(ecu, did, err, sizeof(err));
        if (!json) {
            reply_error("read 0x%03X.%u failed: %s", ecu, did, err);
            continue;
        }
        /* Replies land on the same topic a scheduled poll would use, so a
         * subscriber does not need to know which triggered it. */
        const o3e_dp_entry_t *e = o3e_db_find(did);
        char topic[256];
        mqtt_pub_topic(topic, sizeof(topic), ecu, did,
                       e ? o3e_db_name(e) : "", "", NULL);
        mqtt_pub_raw(topic, json, false);
        free(json);
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

static void do_write(uint16_t ecu, const cJSON *data)
{
    if (!cJSON_IsObject(data)) {
        reply_error("write expects an object of did -> value");
        return;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, data) {
        if (!it->string) {
            continue;
        }
        uint16_t did = (uint16_t)strtol(it->string, NULL, 0);
        char *value = cJSON_PrintUnformatted(it);
        char err[192] = "";
        if (!value) {
            continue;
        }
        /* Never forced over MQTT: the command topic is where automations
           live, and one of those repeating a mistake is worse than a person
           making it once. */
        if (!poller_write_now(ecu, did, value, false, err, sizeof(err))) {
            reply_error("write 0x%03X.%u failed: %s", ecu, did, err);
        } else {
            ESP_LOGI(TAG, "wrote 0x%03X.%u via MQTT", ecu, did);
        }
        free(value);
    }
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
    if (data_len <= 0 || data_len >= CMND_MAX_PAYLOAD) {
        reply_error("command payload must be 1..%d bytes", CMND_MAX_PAYLOAD - 1);
        return;
    }

    if (!cmnd_q) {
        cmnd_q = xQueueCreate(4, sizeof(cmnd_job_t));
        if (!cmnd_q) {
            return;
        }
        /* 6 KiB: decoding recurses through the codec tree and parses JSON. */
        if (xTaskCreate(cmnd_task, "mqttcmd", 6144, NULL, 4, NULL) != pdPASS) {
            vQueueDelete(cmnd_q);
            cmnd_q = NULL;
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
    uint16_t ecu = parse_addr(cJSON_GetObjectItem(root, "addr"), default_ecu());

    if (!cJSON_IsString(mode)) {
        reply_error("bad payload: \"mode\" is required (read or write)");
    } else if (strcmp(mode->valuestring, "read") == 0) {
        do_read(ecu, jdata);
    } else if (strcmp(mode->valuestring, "write") == 0) {
        do_write(ecu, jdata);
    } else if (strcmp(mode->valuestring, "grid") == 0) {
        do_grid(ecu, root);
    } else if (strcmp(mode->valuestring, "storage") == 0) {
        do_storage(ecu, root);
    } else if (strcmp(mode->valuestring, "output") == 0) {
        do_output(root);
    } else if (strcmp(mode->valuestring, "read-raw") == 0 ||
               strcmp(mode->valuestring, "write-raw") == 0) {
        /* open3e's raw modes exchange undecoded hex. Not implemented here:
         * the point of this gateway is the decoded view, and a raw write is
         * the easiest way to put a heat pump into a state nobody intended. */
        reply_error("mode '%s' is not supported by this gateway", mode->valuestring);
    } else {
        reply_error("bad mode '%s'; supported: read, write, grid, storage, output",
                    mode->valuestring);
    }
    cJSON_Delete(root);
}
