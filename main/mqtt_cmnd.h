/* MQTT command listener.
 *
 * Accepts decoded reads and writes in open3e's -l/--listen schema:
 *
 *   {"mode": "read", "data": [268, 269]}
 *   {"mode": "read", "data": [268], "addr": "0x680"}
 *   {"mode": "write", "data": [[396, 52.0]], "addr": "0x680"}
 *
 * Replies go to <base>/<didName> for reads, exactly like a scheduled poll, and
 * errors to <base>/ERR. Raw writes and service 0x77 remain disabled.
 */
#ifndef O3E_MQTT_CMND_H
#define O3E_MQTT_CMND_H

#include <stddef.h>

void mqtt_cmnd_dispatch(const char *topic, int topic_len,
                        const char *data, int data_len);

#endif /* O3E_MQTT_CMND_H */
