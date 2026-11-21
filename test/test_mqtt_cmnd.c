#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/mqtt_cmnd.c"

static const o3e_dp_entry_t entries[] = {{.did = 257}, {.did = 259}, {.did = 396}};
static char last_topic[256], last_payload[8192], write_value[512];
static unsigned publications, reads, writes;
static uint16_t last_ecu, last_did;
static uint16_t read_ecus[16], read_dids[16];
static bool write_force;
static const char *scan_json =
    "{\"devices\":[{\"addr\":1664,\"name\":\"unit\",\"prop\":\"HPMUMASTER\","
    "\"vin\":\"VIN\",\"sw\":\"1.0\",\"hw\":\"2.0\","
    "\"dids\":[[257,2],[396,4],[999,1]]},"
    "{\"addr\":1697,\"prop\":\"OTHER\",\"vin\":\"\",\"sw\":\"\",\"hw\":\"\","
    "\"dids\":[[259,2]]}]}";

void mqtt_cfg_get(mqtt_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->base_topic, "open3e");
    strcpy(cfg->cmnd_topic, "open3e/cmnd");
}
void sys_cfg_get(sys_cfg_t *cfg) { memset(cfg, 0, sizeof(*cfg)); }
bool sys_cfg_set(const sys_cfg_t *cfg) { (void)cfg; return true; }
char *app_config_read_file(const char *path)
{ (void)path; return scan_json ? strdup(scan_json) : NULL; }
char *e3_scan_result_json(void) { return scan_json ? strdup(scan_json) : NULL; }
void poller_device_name(uint16_t ecu, char *out, size_t size)
{ (void)ecu; snprintf(out, size, "unit"); }
void mqtt_pub_topic(char *out, size_t size, uint16_t ecu, uint16_t did,
                    const char *name, const char *device, const char *override)
{
    (void)ecu; (void)did; (void)device; (void)override;
    snprintf(out, size, "open3e/%s", name);
}
bool mqtt_pub_raw(const char *topic, const char *payload, bool retain)
{
    (void)retain;
    snprintf(last_topic, sizeof(last_topic), "%s", topic);
    snprintf(last_payload, sizeof(last_payload), "%s", payload);
    publications++;
    return true;
}
size_t o3e_db_count(void) { return sizeof(entries) / sizeof(entries[0]); }
const o3e_dp_entry_t *o3e_db_at(size_t index)
{ return index < o3e_db_count() ? &entries[index] : NULL; }
const o3e_dp_entry_t *o3e_db_find(uint16_t did)
{
    for (size_t i = 0; i < o3e_db_count(); i++) {
        if (entries[i].did == did) return &entries[i];
    }
    return NULL;
}
const char *o3e_db_name(const o3e_dp_entry_t *entry)
{
    switch (entry->did) {
    case 257: return "First";
    case 259: return "Second";
    case 396: return "Temperature";
    default: return "";
    }
}
char *o3e_db_json(uint16_t did, uint16_t len)
{ (void)did; (void)len; return NULL; }
o3e_node_t *o3e_codec_compile(const char *json) { (void)json; return NULL; }
void o3e_codec_free(o3e_node_t *node) { (void)node; }
char *poller_read_now(uint16_t ecu, uint16_t did, char *err, size_t size)
{
    (void)err; (void)size;
    assert(reads < sizeof(read_dids) / sizeof(read_dids[0]));
    read_ecus[reads] = ecu;
    read_dids[reads] = did;
    reads++;
    last_ecu = ecu;
    last_did = did;
    return strdup("{\"Comfort\":21.5,\"Other\":10}");
}
bool poller_write_now(uint16_t ecu, uint16_t did, const char *json, bool force,
                      char *err, size_t size)
{
    (void)err; (void)size;
    writes++;
    last_ecu = ecu;
    last_did = did;
    write_force = force;
    snprintf(write_value, sizeof(write_value), "%s", json);
    return true;
}
uds_result_t can_read_did(uint16_t ecu, uint16_t did, uint8_t *buf, size_t size,
                          size_t *len, uint32_t timeout)
{
    (void)size; (void)timeout;
    reads++; last_ecu = ecu; last_did = did;
    buf[0] = 0xc2; buf[1] = 0x01; *len = 2;
    return (uds_result_t){UDS_OK, 0};
}
const char *uds_strerror(uds_result_t result) { (void)result; return "error"; }
const int CONTACT_PINS[CONTACT_COUNT] = {1, 2};
bool contact_parse_onoff(const char *s, size_t len, bool *on)
{ (void)s; (void)len; (void)on; return false; }
bool contact_output_set(int i, bool on) { (void)i; (void)on; return false; }
bool contact_output_set_by_slug(const char *s, bool on)
{ (void)s; (void)on; return false; }
bool grid_hold_start(uint16_t ecu, int16_t w, uint32_t sec, char *err, size_t size)
{ (void)ecu; (void)w; (void)sec; (void)err; (void)size; return false; }
bool charge_hold_start(uint16_t ecu, uint16_t w, uint32_t sec, char *err, size_t size)
{ (void)ecu; (void)w; (void)sec; (void)err; (void)size; return false; }
bool grid_hold_switch(bool on, char *err, size_t size)
{ (void)on; (void)err; (void)size; return false; }
void grid_hold_status(grid_hold_status_t *status) { memset(status, 0, sizeof(*status)); }
void grid_hold_stop(void) {}
void hold_publish(void) {}
bool storage_mode_parse(const char *s, storage_mode_t *mode)
{ (void)s; (void)mode; return false; }
bool storage_hold_start(uint16_t ecu, storage_mode_t mode, uint32_t sec,
                        char *err, size_t size)
{ (void)ecu; (void)mode; (void)sec; (void)err; (void)size; return false; }

static void reset(void)
{
    publications = reads = writes = 0;
    last_topic[0] = last_payload[0] = write_value[0] = '\0';
}

int main(void)
{
    reset();
    execute("{\"mode\":\"system\"}");
    assert(strcmp(last_topic, "open3e/system") == 0);
    cJSON *system = cJSON_Parse(last_payload);
    cJSON *device = cJSON_GetArrayItem(cJSON_GetObjectItem(system, "devices"), 0);
    assert(cJSON_GetObjectItem(device, "id")->valueint == 1664);
    assert(strcmp(cJSON_GetObjectItem(device, "name")->valuestring, "HPMUMASTER") == 0);
    cJSON *features = cJSON_GetObjectItem(device, "features");
    assert(cJSON_GetArraySize(features) == 2);
    assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(features, 1), "topic")->valuestring,
                  "open3e/Temperature") == 0);
    cJSON_Delete(system);

    reset();
    execute("{\"mode\":\"read-json\",\"addr\":\"1664\",\"data\":[257,\"0x6a1.[257,259]\"]}");
    assert(reads == 3 && last_ecu == 0x6a1 && last_did == 259);
    assert(strcmp(last_topic, "open3e/Second") == 0);

    reset();
    execute("{\"mode\":\"read-json\",\"data\":[\"396.Comfort\"]}");
    assert(strcmp(last_topic, "open3e/Comfort") == 0 && strcmp(last_payload, "21.5") == 0);
    execute("{\"mode\":\"read-json\",\"data\":[\"396.0\"]}");
    assert(strcmp(last_topic, "open3e/Comfort") == 0);

    reset();
    execute("{\"mode\":\"read-raw\",\"data\":[396]}");
    assert(strcmp(last_topic, "open3e/Temperature") == 0 && strcmp(last_payload, "c201") == 0);
    execute("{\"mode\":\"read-pure\",\"data\":[396]}");
    assert(strcmp(last_topic, "open3e/DID_396:len=2") == 0);

    reset();
    execute("{\"mode\":\"read-all-json\",\"addr\":\"0x6a1\"}");
    assert(reads == 1 && read_ecus[0] == 0x6a1 && read_dids[0] == 259);
    reset();
    execute("{\"mode\":\"read-all\"}");
    assert(reads == 3 && read_ecus[0] == 0x680 && read_ecus[2] == 0x680);
    assert(read_dids[0] == 257 && read_dids[1] == 396 && read_dids[2] == 999);
    assert(strcmp(last_topic, "open3e/DID_999") == 0);
    execute("{\"mode\":\"read\",\"data\":[\"0x6a1.[257,,259]\"]}");
    assert(reads == 3 && strstr(last_payload, "invalid DID list"));

    const char *invalid_scans[] = {
        NULL, "{", "{\"devices\":{}}",
        "{\"devices\":[{\"addr\":1664}]}",
        "{\"devices\":[{\"addr\":1664,\"dids\":[]}]}",
        "{\"devices\":[{\"addr\":1664,\"dids\":[[257,2],[1.5,2]]}]}",
        "{\"devices\":[{\"addr\":1664,\"dids\":[[257,2],[65536,2]]}]}",
        "{\"devices\":[{\"addr\":1664,\"dids\":[[257,2],[396]]}]}",
        "{\"devices\":[{\"addr\":1664,\"dids\":[[257,2],\"396\"]}]}"
    };
    for (size_t i = 0; i < sizeof(invalid_scans) / sizeof(invalid_scans[0]); i++) {
        scan_json = invalid_scans[i];
        reset();
        execute("{\"mode\":\"read-all\",\"addr\":\"0x680\"}");
        assert(reads == 0 && strcmp(last_topic, "open3e/ERR") == 0);
    }
    scan_json = "{\"devices\":[{\"addr\":1664,\"dids\":[[257,2]]}]}";
    reset();
    execute("{\"mode\":\"read-all-json\",\"addr\":\"0x6a1\"}");
    assert(reads == 0 && strcmp(last_topic, "open3e/ERR") == 0);
    reset();
    execute("{\"mode\":\"write\",\"data\":[[\"0x6a1.396.Comfort\",\"22.5\"]]}");
    assert(writes == 1 && last_ecu == 0x6a1 && last_did == 396 && !write_force);
    assert(strcmp(write_value, "{\"Comfort\":22.5}") == 0);
    execute("{\"mode\":\"write\",\"data\":[[396,\"18:00\"]]}");
    assert(strcmp(write_value, "\"18:00\"") == 0);
    execute("{\"mode\":\"write\",\"data\":{\"396\":42}}");
    assert(strcmp(write_value, "42") == 0);
    execute("{\"mode\":\"write\",\"data\":{\"396\":\"42\"}}");
    assert(strcmp(write_value, "\"42\"") == 0);

    reset();
    execute("{\"mode\":\"write\",\"data\":[[\"396.missing\",42]]}");
    assert(writes == 0 && strcmp(last_topic, "open3e/ERR") == 0);
    execute("{\"mode\":\"write-raw\",\"data\":[[396,\"c201\"]]}");
    assert(writes == 0 && strstr(last_payload, "not supported"));
    execute("{\"mode\":\"write-sid77\",\"data\":[[396,42]]}");
    assert(writes == 0 && strstr(last_payload, "not supported"));
    execute("{\"mode\":\"write-raw-sid77\",\"data\":[[396,\"c201\"]]}");
    assert(writes == 0 && strstr(last_payload, "not supported"));
    execute("{\"mode\":\"read\",\"addr\":\"invalid\",\"data\":[257]}");
    assert(reads == 1 && strstr(last_payload, "invalid ECU address"));

    scan_json = NULL;
    reset();
    execute("{\"mode\":\"system\"}");
    assert(strcmp(last_topic, "open3e/ERR") == 0 && strstr(last_payload, "scan"));
    puts("mqtt commands: ok");
    return 0;
}
