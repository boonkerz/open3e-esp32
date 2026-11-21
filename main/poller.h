/* Cyclic reading of the selected datapoints and publishing them to MQTT.
 *
 * The selection lives in /data/points.json, written by the web UI.  Each entry
 * carries its own interval, topic and publish mode, so a flow temperature can
 * be read every 10 s while an energy counter is read every 15 minutes without
 * either of them being a special case.
 */
#ifndef O3E_POLLER_H
#define O3E_POLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "mqtt_pub.h"

#define POLL_MAX_POINTS 128

typedef struct {
    uint32_t polls;
    uint32_t failures;
    uint32_t published;
    uint16_t active_points;
    uint32_t last_poll_ms;
} poll_stats_t;

bool poller_start(void);
void poller_stop(void);

/* Suspend cyclic reading. A bus scan issues thousands of requests through the
 * same serialised CAN queue; letting the poller interleave would stretch a
 * twenty-minute scan considerably and publish stale values meanwhile. */
void poller_pause(bool paused);
bool poller_is_paused(void);

/* Re-read /data/points.json and rebuild the compiled codec trees.
 * Called after the web UI saves a new selection. */
bool poller_reload(void);

/* Make every selected datapoint due immediately.
 *
 * Used when MQTT connects: the poller starts before the broker connection is
 * established, so its first readings are decoded and then dropped for want of
 * a connection. Without this the first values appear only after a full
 * interval, which looks like the selection is not being polled at all. */
void poller_refresh(void);
void poller_stats(poll_stats_t *out);

/* Resolve the configured name used by MQTT's {device} placeholder. */
void poller_device_name(uint16_t ecu, char *out, size_t out_sz);

/* Read one datapoint immediately and return the decoded JSON, bypassing the
 * schedule. Used by the web UI's live value column and manual read.
 * Caller frees; `err` receives a reason when NULL is returned. */
char *poller_read_now(uint16_t ecu, uint16_t did, char *err, size_t err_sz);

/* Encode `value_json` for `did` and write it to the ECU. Refuses unless
 * writing is enabled in the system settings and the datapoint is marked rw. */
/* `force` ignores the database's access flag.
 *
 * That flag is open3e's knowledge, not the device's: it says what the project
 * has recorded about a datapoint, and nobody may ever have tried writing the
 * one in front of you. The device enforces its own access and answers a
 * genuine read-only write with a negative response, so the failure is safe --
 * but the guard exists because sending bytes to a heat pump on a hunch is the
 * mistake worth making hard, and forcing keeps it a deliberate act. The global
 * write switch still applies. */
bool poller_write_now(uint16_t ecu, uint16_t did, const char *value_json,
                      bool force,
                      char *err, size_t err_sz);

#endif /* O3E_POLLER_H */
