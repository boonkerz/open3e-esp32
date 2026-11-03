/* MQTT publishing, deliberately compatible with open3e's own output.
 *
 * Topics, payload shapes, the LWT and the command topic all follow
 * Open3Eclient.py, so an existing broker setup, Home Assistant template or
 * subscription keeps working when the Raspberry Pi running open3e is replaced
 * by this board.
 */
#ifndef O3E_MQTT_PUB_H
#define O3E_MQTT_PUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "o3e_codec.h"

typedef enum {
    /* One topic, payload is the decoded object as JSON. open3e's -j. */
    PUB_MODE_JSON = 0,
    /* One topic per scalar leaf, payload is the bare value. open3e's default,
     * implemented by its mqttdump(). */
    PUB_MODE_FLAT,
} pub_mode_t;

typedef struct {
    bool     connected;
    uint32_t published;
    uint32_t errors;
    uint32_t reconnects;
} mqtt_stats_t;

bool mqtt_pub_start(void);
void mqtt_pub_stop(void);
void mqtt_pub_restart(void);      /* after a settings change */
void mqtt_pub_stats(mqtt_stats_t *out);
bool mqtt_pub_connected(void);
/* True once a client object exists, independent of whether it is connected.
 * The WireGuard task uses this to avoid starting a second client when the
 * tunnel comes up before app_main has reached mqtt_pub_start(). */
bool mqtt_pub_running(void);

/* Expand the configured format string for one datapoint into a full topic.
 * Supports open3e's placeholders: {didNumber}, {didName}, {ecuAddr}, {device},
 * plus the {ecuAddr:03X} and {didNumber:04d} forms its README documents. */
void mqtt_pub_topic(char *out, size_t out_sz, uint16_t ecu, uint16_t did,
                    const char *did_name, const char *device,
                    const char *suffix_override);

/* Publish one decoded datapoint. */
bool mqtt_pub_value(const char *topic, const o3e_node_t *node,
                    const uint8_t *payload, size_t len, pub_mode_t mode);

bool mqtt_pub_raw(const char *topic, const char *payload, bool retain);


#endif /* O3E_MQTT_PUB_H */
