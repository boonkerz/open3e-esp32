/* Raw CAN-frame relay over MQTT, for a consumer with its own codec.
 *
 * Viessmann energy meters and a device's own "collect" broadcast channel
 * never answer a request -- see em380.c/collect.c, which decode them with
 * this firmware's open3e codec. A caller that keeps its own datapoint
 * definitions instead (e.g. ioBroker.e3oncan in gateway mode, see
 * docs/raw-gateway-api.md section 2) needs the bytes as they arrived, not
 * this firmware's interpretation of them.
 *
 * Deliberately its own module rather than a mode of collect.c/em380.c: it
 * shares nothing with their frame-reassembly state machines (open3e's DID
 * framing for "collect", the E380's own multi-byte layout) because it does
 * not reassemble anything -- one CAN frame in, one MQTT message out.
 */
#ifndef O3E_RAW_RELAY_H
#define O3E_RAW_RELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Not tied to COLLECT_MAX_IDS: unlike collect.c, this keeps no per-ID
 * reassembly state, so the only real cost of a longer list is the ISR-context
 * linear scan in on_frame() against however many IDs are configured. */
#define RAW_RELAY_MAX_IDS 32

typedef struct {
    bool     enabled;
    uint32_t frames;
    uint32_t published;
    uint8_t  n_ids;
    uint16_t can_ids[RAW_RELAY_MAX_IDS];
} raw_relay_stats_t;

/* Start relaying the given CAN-IDs. Passive: this only installs a receive
 * filter, it never transmits. */
bool raw_relay_start(const uint16_t *ids, size_t n);
void raw_relay_stop(void);
void raw_relay_stats(raw_relay_stats_t *out);

#endif /* O3E_RAW_RELAY_H */
