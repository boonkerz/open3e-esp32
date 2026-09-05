/* Persistent configuration.
 *
 * Two stores with different jobs:
 *   NVS       - credentials and scalars that must survive a filesystem
 *               rebuild, and that we never want to serve to a browser.
 *   LittleFS  - the datapoint selection and the last scan result, which are
 *               list-shaped, can get large, and are edited as JSON by the UI.
 */
#ifndef O3E_APP_CONFIG_H
#define O3E_APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CFG_STR_MAX      64
/* The 802.11 limits, so these buffers match the fields in wifi_config_t
 * exactly: an SSID is at most 32 octets, a WPA2 passphrase 63 characters.
 * Sizing them larger only creates truncation that surfaces at connect time. */
#define CFG_SSID_MAX     33
#define CFG_PASS_MAX     64
#define CFG_TOPIC_MAX   128
/* Sized for RAW_RELAY_MAX_IDS (raw_relay.h) comma-separated "0x7FF" entries:
 * 32 * strlen("0x7FF,") + 1, rounded up. CFG_STR_MAX is too small once a
 * caller relays both energy meters plus a handful of collect IDs. */
#define CFG_RAW_IDS_MAX 256

/* Files on the storage partition. */
#define CFG_MOUNT        "/data"
#define CFG_DB_PATH      CFG_MOUNT "/o3edb.bin"
#define CFG_WWW_DIR      CFG_MOUNT "/www"
#define CFG_POINTS_PATH  CFG_MOUNT "/points.json"
#define CFG_SYSTEM_PATH  CFG_MOUNT "/system.json"

typedef struct {
    char ssid[CFG_SSID_MAX];
    char pass[CFG_PASS_MAX];
    char hostname[CFG_STR_MAX];   /* mDNS name, default "open3e" */
    char ap_pass[CFG_PASS_MAX];   /* setup access point password */
} wifi_cfg_t;

typedef struct {
    char     host[CFG_STR_MAX];
    uint16_t port;
    char     user[CFG_STR_MAX];
    char     pass[CFG_PASS_MAX];
    char     base_topic[CFG_TOPIC_MAX];   /* default "open3e" */
    /* open3e's -mfstr, e.g. "{didNumber}_{didName}". Default "{didName}". */
    char     format[CFG_TOPIC_MAX];
    char     cmnd_topic[CFG_TOPIC_MAX];   /* default "<base>/cmnd" */
    bool     enabled;
    bool     ha_discovery;
    char     ha_prefix[CFG_TOPIC_MAX];    /* default "homeassistant" */
} mqtt_cfg_t;

typedef struct {
    /* Writing to a heat pump is off until deliberately enabled; a datapoint
     * additionally has to be marked rw in the open3e database. */
    bool write_enabled;
    /* Separate from write_enabled on purpose: /api/rawwrite bypasses the
     * open3e database entirely -- no rw check, and unlike a decoded write it
     * accepts a DID this firmware's own database does not even know about.
     * That is a materially bigger trust step than the decoded path, so it
     * gets its own explicit opt-in rather than riding along with
     * write_enabled. */
    bool raw_write_enabled;
    /* Passive reception of the E380 energy meter. Off by default: most
     * installations have no meter, and an unused listener is one more thing
     * running in the receive interrupt. */
    bool em380_enabled;
    /* Passive reception of a device's own broadcast channel ("collect"): the
     * Vitocharge VX3 announces its datapoints on 0x451, the Vitocal on 0x693.
     * Off by default; the identifier depends on the installation. */
    bool collect_enabled;
    /* Comma-separated, e.g. "0x451,0x441": the Vitocharge announces its
     * datapoints on one channel and its control mode on another. */
    char collect_canids[CFG_STR_MAX];
    /* Comma-separated CAN-IDs whose raw bytes get relayed to MQTT
     * (<base>/raw/<id>), independent of the decoded topics, HA discovery and
     * points.json - see raw_relay.h. Empty by default (off); unrelated to
     * collect_canids/em380_enabled above, which publish this firmware's own
     * decoded view of the same kind of broadcast traffic. */
    char raw_canids[CFG_RAW_IDS_MAX];
    char tz[CFG_STR_MAX];   /* POSIX TZ, default "CET-1CEST,M3.5.0,M10.5.0/3" */
    /* What a bare "start charging" means: which storage unit, how much, and
     * for how long. Kept as settings rather than passed with every command so
     * that a Home Assistant switch has something to switch -- the hold itself
     * is deliberately not persistent, but what it would do is. `grid_watts` is
     * positive and means "draw this much from the grid". */
    uint16_t grid_ecu;
    uint16_t grid_watts;
    uint16_t grid_minutes;
} sys_cfg_t;

bool app_config_init(void);          /* opens NVS and mounts LittleFS */
bool app_config_fs_mounted(void);

void wifi_cfg_get(wifi_cfg_t *out);
bool wifi_cfg_set(const wifi_cfg_t *in);
bool wifi_cfg_clear(void);
bool wifi_cfg_present(void);         /* true once an SSID has been stored */

void mqtt_cfg_get(mqtt_cfg_t *out);
bool mqtt_cfg_set(const mqtt_cfg_t *in);

void sys_cfg_get(sys_cfg_t *out);
bool sys_cfg_set(const sys_cfg_t *in);

/* ------------------------------------------------------------------ */
/* Selection file entries                                               */
/*
 * points.json holds two kinds of entry: polled datapoints (ecu + did) and
 * passively received energy-meter frames (type "em380" + canId). Reading a
 * field that only the other kind has used to dereference NULL and panic, so
 * both the kind test and the field access live here rather than being
 * open-coded per consumer.
 */
struct cJSON;

/* True for a polled datapoint, i.e. an entry the poller owns. */
bool     sel_is_datapoint(const struct cJSON *entry);
/* True for a passively received energy-meter frame. */
bool     sel_is_em380(const struct cJSON *entry);
/* True unless the entry carries "enabled": false. */
bool     sel_enabled(const struct cJSON *entry);
/* Numeric field, or `dflt` when absent or not a number. */
uint16_t sel_u16(const struct cJSON *entry, const char *key, uint16_t dflt);
uint32_t sel_u32(const struct cJSON *entry, const char *key, uint32_t dflt);
/* String field, or NULL when absent or not a string. */
const char *sel_str(const struct cJSON *entry, const char *key);
/* Boolean field, or `dflt` when absent or not a bool. */
bool     sel_bool(const struct cJSON *entry, const char *key, bool dflt);

/* Read/replace a whole JSON file under CFG_MOUNT. Caller frees the read. */
char *app_config_read_file(const char *path);
bool  app_config_write_file(const char *path, const char *data, size_t len);

/* Streaming variant, for output too large to hold in memory first.
 *
 * The scan result runs to hundreds of kilobytes; building it in a growable
 * buffer meant a series of doubling reallocations, and every step below
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL came out of internal RAM -- the same
 * pool Wi-Fi and the web server need. Writing straight to the file removes the
 * buffer entirely.
 *
 * Writes go to a temporary name and are renamed on commit, so an interrupted
 * write cannot leave a half-file behind. */
FILE *app_config_begin_write(const char *path);
bool  app_config_commit_write(const char *path, FILE *f);
void  app_config_abort_write(const char *path, FILE *f);

/* Write `s` to `f` as a JSON string literal, escaping as needed. */
void  app_config_fput_json_str(FILE *f, const char *s);

#endif /* O3E_APP_CONFIG_H */
