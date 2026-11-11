/* UDS (ISO 14229) client for E3 controllers, on top of ISO-TP.
 *
 * Only the two services open3e relies on are implemented:
 *   0x22 ReadDataByIdentifier
 *   0x2E WriteDataByIdentifier
 * Service 0x77 is deliberately absent -- open3e marks it experimental and
 * warns to always verify the result, which is not a promise this gateway
 * should make about somebody's heating system.
 */
#ifndef O3E_UDS_H
#define O3E_UDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "isotp.h"

#define UDS_SID_READ        0x22
#define UDS_SID_WRITE       0x2E
#define UDS_RESP_READ       0x62
#define UDS_RESP_WRITE      0x6E
#define UDS_NEGATIVE        0x7F
#define UDS_NRC_PENDING     0x78   /* requestCorrectlyReceived-ResponsePending */

/* P2: normal answer deadline. P2*: extended deadline once the ECU has replied
 * "response pending", which E3 devices do for slower datapoints.
 *
 * P2* is derived from P2 rather than fixed, so a caller that asked for a short
 * deadline gets a correspondingly short extension. A scan probes ~10000
 * datapoints with a 200 ms deadline; a fixed 5 s extension let a single
 * stubborn one hold the bus far longer than the 30 s another caller is willing
 * to wait for it, which surfaced as "transport error" on an unrelated read. */
#define UDS_P2_MS         1000
#define UDS_P2_STAR_MAX   5000
#define UDS_P2_STAR_OF(p2) ((p2) * 5 > UDS_P2_STAR_MAX ? UDS_P2_STAR_MAX : (p2) * 5)

/* How many "response pending" replies to absorb before giving up. More than a
 * couple is an ECU misbehaving, not a slow datapoint, and waiting on it costs
 * every other user of the bus. */
#define UDS_MAX_PENDING   3
/* The ECU discovery sweep touches 112 addresses, most of them empty, so it
 * uses a much shorter deadline than normal operation. */
#define UDS_SCAN_P2_MS   200

typedef enum {
    UDS_OK = 0,
    UDS_ERR_TIMEOUT,      /* silence: usually means no ECU at this address */
    UDS_ERR_TRANSPORT,    /* ISO-TP went wrong mid-message: sequence, PCI, overflow */
    UDS_ERR_SEND,         /* a frame of ours never got onto the bus */
    UDS_ERR_NEGATIVE,     /* ECU answered 0x7F; see `nrc` */
    UDS_ERR_MALFORMED,
    UDS_ERR_TOO_LONG,
} uds_err_t;

typedef struct {
    uds_err_t err;
    uint8_t   nrc;        /* valid when err == UDS_ERR_NEGATIVE */
} uds_result_t;

const char *uds_strerror(uds_result_t r);
const char *uds_nrc_str(uint8_t nrc);

/* Largest write payload we accept. The biggest datapoint open3e knows about is
 * 199 bytes; the headroom covers future database updates. */
#define UDS_MAX_WRITE 512

/* An E3 bus answers a request sent to `tx` on `tx + 0x10`. */
#define UDS_RX_OF(tx) ((tx) + 0x10)

uds_result_t uds_read_did(const isotp_link_t *link, uint16_t did,
                          uint8_t *buf, size_t buf_sz, size_t *out_len,
                          uint32_t p2_ms);

uds_result_t uds_write_did(const isotp_link_t *link, uint16_t did,
                           const uint8_t *data, size_t len, uint32_t p2_ms);

#endif /* O3E_UDS_H */
