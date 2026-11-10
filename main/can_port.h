/* TWAI (the ESP32's CAN controller) wrapped for ISO-TP.
 *
 * open3e warns "do not start more than one instance" because two ISO-TP
 * sessions on one bus corrupt each other.  The same applies here, except the
 * competing users are internal: the poller, the scanner, the web UI's manual
 * read and the MQTT command listener.  A mutex held for the duration of one
 * UDS exchange gives that property directly -- the exchange runs in the
 * calling task, and its result is returned rather than shared.
 */
#ifndef O3E_CAN_PORT_H
#define O3E_CAN_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "isotp.h"
#include "uds.h"

/* Waveshare ESP32-S3-RS485-CAN pin assignment. */
#define CAN_TX_GPIO 15
#define CAN_RX_GPIO 16
/* E3 controllers run the bus at 250 kbit/s. */
#define CAN_BITRATE 250000

typedef struct {
    bool     running;
    uint32_t tx_failed;
    uint32_t rx_missed;        /* frames the ISR could not queue */
    uint32_t bus_errors;       /* the driver's count; includes lost arbitration */
    /* What those bus errors were. Lost arbitration is not a fault at all,
     * just another node starting a frame in the same bit and winning; it can
     * only happen while we transmit, so a counter that includes it climbs
     * exactly when the poller runs and looks like trouble that is not there.
     * Bit, stuff and form errors are signal quality, ACK errors mean nobody
     * answered the frame at all. */
    uint32_t arb_lost;
    uint32_t bit_errors;
    uint32_t stuff_errors;
    uint32_t form_errors;
    uint32_t crc_errors;       /* the controller files these under "other" */
    uint32_t ack_errors;       /* while transmitting: nobody acknowledged */
    uint32_t other_errors;     /* "other" anywhere else in the frame */
    /* Which side of the exchange the controller was on when it saw the
     * error. Receive errors are frames from the other node that arrived
     * damaged and were repeated by it; transmit errors are our own frames,
     * and those the firmware repeats itself. */
    uint32_t rx_errors;
    uint32_t tx_errors;
    /* Where in the frame the error was seen, indexed by the controller's
     * segment code (0..31); the status endpoint names the nonzero ones. */
    uint32_t err_segments[32];
    uint32_t tx_retries;       /* frames sent again after the first attempt
                                  failed (lost arbitration or a bus error);
                                  tx_failed counts the ones that never went */
    uint16_t tx_err_count;     /* the controller's TEC and REC: a steady climb
                                  points at wiring or a wrong bitrate long
                                  before the node actually goes bus-off */
    uint16_t rx_err_count;
    uint32_t recovery_count;   /* how often we came back from bus-off */
    const char *state;         /* running, error-warning, error-passive, bus-off */
    /* Who currently owns the bus, and for how long. A stuck exchange used to
     * show up only as "transport error" somewhere unrelated; naming the holder
     * turns that into one line that says which task and since when. */
    char     holder[16];       /* empty when free */
    uint32_t held_ms;
} can_stats_t;

/* Bring up the TWAI peripheral and start the request-serving task. */
/* Bit timing: what the settings ask for and what the driver made of it. A
 * requested value of 0 means the driver's default. */
typedef struct {
    uint16_t sp_permill;    /* requested sample point, permill of the bit */
    uint8_t  sjw;           /* requested synchronisation jump width, quanta */
    bool     known;         /* the applied values below could be derived */
    uint8_t  quanta;        /* applied: time quanta per bit */
    uint16_t sp_actual;     /* applied sample point, permill */
    uint8_t  sjw_actual;    /* applied SJW, quanta */
} can_timing_t;

/* Store the requested timing; a running node is re-created with it. Called
 * before can_port_start() it only stores. */
void can_port_set_timing(uint16_t sp_permill, uint8_t sjw);
void can_port_timing(can_timing_t *out);

bool can_port_start(void);
void can_port_stop(void);
void can_port_stats(can_stats_t *out);
/* JSON key for an entry of err_segments, NULL for a code the controller
 * never produces. Several ID segments share one name. */
const char *can_port_err_segment_name(int code);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                          */
/*
 * TEC climbing to 128 while REC stays at 0 means "transmitted, never
 * acknowledged" -- on CAN every other active node acknowledges every frame,
 * so it says no one else is listening. That can be wiring, bit rate, a swapped
 * CAN-H/CAN-L pair or a silent bus, and guessing between them is tedious.
 * These two tests separate the cases.
 */
#define CAN_DIAG_MAX_IDS 16

typedef struct {
    uint32_t bitrate;
    uint32_t frames;                      /* frames seen at this bit rate */
    uint8_t  n_ids;
    uint32_t ids[CAN_DIAG_MAX_IDS];       /* distinct IDs, in order of arrival */
    uint32_t repeats;                     /* frames identical to the one before;
                                           * thousands of them mean nobody else
                                           * acknowledges and the transmitter is
                                           * retrying the same frame non-stop */
    uint32_t errors;                      /* bus errors flagged while listening;
                                           * in listen-only the node never
                                           * acknowledges, so these cannot come
                                           * from its own transmissions */
} can_diag_t;

/* Internal loopback: the controller transmits to itself with acknowledgement
 * disabled. Proves the TWAI peripheral and its pin routing work without any
 * bus attached, so a failure here is firmware, not wiring. */
bool can_diag_loopback(char *err, size_t err_sz);

/* Listen-only sweep: never transmits and never acknowledges, so it cannot
 * disturb the bus. Frames at a given bit rate prove the wiring and that rate
 * are right, which narrows the fault to the transmit side. */
bool can_diag_listen(const uint32_t *bitrates, size_t n, uint32_t ms_each,
                     can_diag_t *out, char *err, size_t err_sz);

/* Submit a UDS read and block until it completes. Safe to call from any task.
 * `ecu_tx` is the ECU's request COB-ID (0x680..0x6EF on an E3 bus). */
uds_result_t can_read_did(uint16_t ecu_tx, uint16_t did,
                          uint8_t *buf, size_t buf_sz, size_t *out_len,
                          uint32_t p2_ms);

uds_result_t can_write_did(uint16_t ecu_tx, uint16_t did,
                           const uint8_t *data, size_t len, uint32_t p2_ms);

/* ------------------------------------------------------------------ */
/* Passive reception                                                    */
/*
 * Viessmann energy meters (E380) never answer a request -- they broadcast on
 * fixed CAN-IDs and that is the whole protocol. Those frames must therefore
 * survive the receive path even when no UDS exchange is running, which is
 * exactly when the ISO-TP queue is being flushed.
 *
 * The callback runs in interrupt context: copy what is needed and return
 * whether it woke a higher priority task. It must NOT yield by itself -- the
 * TWAI driver performs the context switch once it has finished its own
 * interrupt handling, and yielding from inside the callback cuts that short.
 */
typedef bool (*can_listen_cb_t)(uint32_t id, const uint8_t *data, uint8_t len);

/* Route frames whose ID is in [first, last] to `cb` instead of the ISO-TP
 * queue. Several listeners can coexist -- the energy meter broadcasts on one
 * range and a device's own "collect" channel on another -- so each is added
 * and removed by its callback. */
#define CAN_MAX_LISTENERS 4

bool can_port_add_listener(uint32_t first, uint32_t last, can_listen_cb_t cb);

/* Like can_port_add_listener(), but only frames whose ID is actually one of
 * `ids` are routed to `cb` -- unlike a plain [first, last] range, other
 * traffic in the gaps between scattered IDs still reaches the normal ISO-TP
 * path instead of being silently swallowed. Needed once the configured IDs
 * can be far apart and are not known in advance to avoid straddling an ECU's
 * own request/response addresses (see raw_relay.c, whose whole point is
 * relaying whatever IDs a caller asks for). */
#define CAN_LISTENER_MAX_IDS 32

bool can_port_add_id_listener(const uint16_t *ids, size_t n_ids, can_listen_cb_t cb);

void can_port_remove_listener(can_listen_cb_t cb);

#endif /* O3E_CAN_PORT_H */
