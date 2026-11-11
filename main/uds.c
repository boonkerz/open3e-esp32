#include "uds.h"

#include <stdio.h>
#include <string.h>

const char *uds_nrc_str(uint8_t nrc)
{
    switch (nrc) {
    case 0x10: return "generalReject";
    case 0x11: return "serviceNotSupported";
    case 0x12: return "subFunctionNotSupported";
    case 0x13: return "incorrectMessageLengthOrInvalidFormat";
    case 0x14: return "responseTooLong";
    case 0x22: return "conditionsNotCorrect";
    case 0x24: return "requestSequenceError";
    case 0x31: return "requestOutOfRange";
    case 0x33: return "securityAccessDenied";
    case 0x35: return "invalidKey";
    case 0x72: return "generalProgrammingFailure";
    case 0x78: return "responsePending";
    case 0x7E: return "subFunctionNotSupportedInActiveSession";
    case 0x7F: return "serviceNotSupportedInActiveSession";
    default:   return "unknown";
    }
}

const char *uds_strerror(uds_result_t r)
{
    static char buf[64];
    switch (r.err) {
    case UDS_OK:            return "ok";
    case UDS_ERR_TIMEOUT:   return "no response";
    /* Both keep the "transport error" prefix clients already know from
     * rawread; the suffix is what tells a frame that never left (busy bus,
     * see can_port.c hal_send) from a response that fell apart. */
    case UDS_ERR_TRANSPORT: return "transport error: protocol";
    case UDS_ERR_SEND:      return "transport error: send failed";
    case UDS_ERR_MALFORMED: return "malformed response";
    case UDS_ERR_TOO_LONG:  return "response too long";
    case UDS_ERR_NEGATIVE:
        snprintf(buf, sizeof(buf), "rejected (0x%02X %s)", r.nrc, uds_nrc_str(r.nrc));
        return buf;
    }
    return "unknown";
}

static uds_result_t ok(void)          { return (uds_result_t){ UDS_OK, 0 }; }
static uds_result_t fail(uds_err_t e) { return (uds_result_t){ e, 0 }; }

static uds_err_t map_transport(isotp_err_t e)
{
    switch (e) {
    case ISOTP_OK:           return UDS_OK;
    case ISOTP_ERR_TIMEOUT:  return UDS_ERR_TIMEOUT;
    case ISOTP_ERR_TOO_LONG: return UDS_ERR_TOO_LONG;
    case ISOTP_ERR_BUS:      return UDS_ERR_SEND;
    default:                 return UDS_ERR_TRANSPORT;
    }
}

/* Receive one response, transparently absorbing "response pending" (NRC 0x78)
 * frames. An ECU may send several before the real answer; each one restarts
 * the clock at P2*, which is what ISO 14229 prescribes. */
static uds_result_t recv_response(const isotp_link_t *link, uint8_t expect_sid,
                                  uint8_t *buf, size_t buf_sz, size_t *out_len,
                                  uint32_t p2_ms)
{
    uint32_t timeout = p2_ms;
    for (int pending = 0; pending <= UDS_MAX_PENDING; pending++) {
        isotp_err_t te = isotp_recv(link, buf, buf_sz, out_len, timeout);
        if (te != ISOTP_OK) {
            return fail(map_transport(te));
        }
        if (*out_len < 1) {
            return fail(UDS_ERR_MALFORMED);
        }

        if (buf[0] == UDS_NEGATIVE) {
            if (*out_len < 3) {
                return fail(UDS_ERR_MALFORMED);
            }
            if (buf[2] == UDS_NRC_PENDING) {
                timeout = UDS_P2_STAR_OF(p2_ms);
                continue;
            }
            return (uds_result_t){ UDS_ERR_NEGATIVE, buf[2] };
        }
        if (buf[0] != expect_sid) {
            return fail(UDS_ERR_MALFORMED);
        }
        return ok();
    }
    /* An ECU stuck in responsePending is a fault, not something to wait on. */
    return fail(UDS_ERR_TIMEOUT);
}

uds_result_t uds_read_did(const isotp_link_t *link, uint16_t did,
                          uint8_t *buf, size_t buf_sz, size_t *out_len,
                          uint32_t p2_ms)
{
    /* Cleared up front so no failure path can leave a caller with a length
     * from a previous request. `buf` itself is scratch for the transport and
     * may hold a negative response; the length is what says whether any of it
     * counts as datapoint content. */
    *out_len = 0;

    const uint8_t req[3] = { UDS_SID_READ, (uint8_t)(did >> 8), (uint8_t)(did & 0xFF) };
    isotp_err_t te = isotp_send(link, req, sizeof(req));
    if (te != ISOTP_OK) {
        return fail(map_transport(te));
    }

    size_t n = 0;
    uds_result_t r = recv_response(link, UDS_RESP_READ, buf, buf_sz, &n, p2_ms);
    if (r.err != UDS_OK) {
        return r;
    }
    /* 0x62 <did-hi> <did-lo> <payload...> */
    if (n < 3 || buf[1] != req[1] || buf[2] != req[2]) {
        return fail(UDS_ERR_MALFORMED);
    }
    memmove(buf, buf + 3, n - 3);
    *out_len = n - 3;
    return ok();
}

uds_result_t uds_write_did(const isotp_link_t *link, uint16_t did,
                           const uint8_t *data, size_t len, uint32_t p2_ms)
{
    /* The largest writable datapoint in the open3e database is 199 bytes, so a
     * stack buffer avoids both a 4 KiB static and any reentrancy question. */
    uint8_t req[UDS_MAX_WRITE + 3];
    if (len > UDS_MAX_WRITE) {
        return fail(UDS_ERR_TOO_LONG);
    }
    req[0] = UDS_SID_WRITE;
    req[1] = (uint8_t)(did >> 8);
    req[2] = (uint8_t)(did & 0xFF);
    memcpy(req + 3, data, len);

    isotp_err_t te = isotp_send(link, req, len + 3);
    if (te != ISOTP_OK) {
        return fail(map_transport(te));
    }

    uint8_t resp[16];
    size_t n = 0;
    uds_result_t r = recv_response(link, UDS_RESP_WRITE, resp, sizeof(resp), &n, p2_ms);
    if (r.err != UDS_OK) {
        return r;
    }
    if (n < 3 || resp[1] != req[1] || resp[2] != req[2]) {
        return fail(UDS_ERR_MALFORMED);
    }
    return ok();
}
