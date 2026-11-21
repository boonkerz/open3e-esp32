#include "bus_guard.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

/* Written from the receive interrupt, read from tasks. Every field is a
 * single word, and a reader that sees the old value for one more call only
 * decides a few milliseconds late. */
static volatile bool     s_enabled = true;
static volatile bool     s_active;
static volatile uint32_t s_last_critical_ms;
static volatile bool     s_have_foreign;
static volatile uint32_t s_last_foreign_ms;
static volatile uint32_t s_holds, s_critical, s_foreign, s_refused, s_settled;
static volatile uint16_t s_last_id;
static volatile uint8_t  s_last_sid;

/* Requests that change what a control unit is or does, rather than ask it
 * something: ECUReset, RoutineControl, RequestDownload, RequestUpload,
 * TransferData, RequestTransferExit. DiagnosticSessionControl is handled
 * apart because only leaving the default session counts.
 *
 * 0x77 is not here although it is the reply to RequestTransferExit: as a
 * request it is Viessmann's own write service, which the devices use among
 * themselves in normal operation, and the identifier does not say which of
 * the two a frame is. A download is caught by everything around it. */
static inline bool IRAM_ATTR critical_request(uint8_t sid)
{
    return sid == 0x11 || sid == 0x31 ||
           (sid >= 0x34 && sid <= 0x37);
}

bus_guard_class_t IRAM_ATTR bus_guard_classify(uint32_t id, const uint8_t *d,
                                               uint8_t len, uint8_t *sid_out)
{
    if (id < BUS_GUARD_ID_FIRST || id > BUS_GUARD_ID_LAST || len < 2) {
        return BUS_GUARD_NONE;
    }
    /* Only the frame that starts a message carries the service byte. A
     * consecutive or flow-control frame belongs to a message whose start was
     * already seen -- and the broadcast "collect" stream, which lives in the
     * same identifier range, is made of bytes 0x21 and up, which never look
     * like a start. */
    uint8_t pci = d[0] >> 4;
    uint8_t sid, sub = 0;
    bool have_sub = false;
    if (pci == 0) {
        uint8_t n = d[0] & 0x0F;
        if (n < 1 || n > 7 || n > len - 1) {
            return BUS_GUARD_NONE;
        }
        sid = d[1];
        if (n >= 2) {
            sub = d[2];
            have_sub = true;
        }
    } else if (pci == 1) {
        uint16_t total = (uint16_t)((d[0] & 0x0F) << 8) | d[1];
        if (total < 8 || len < 8) {
            return BUS_GUARD_NONE;
        }
        sid = d[2];
        sub = d[3];
        have_sub = true;
    } else {
        return BUS_GUARD_NONE;
    }
    if (sid_out) {
        *sid_out = sid;
    }

    /* The same identifier can be one unit's reply and another's request, so
     * the service byte decides, in both directions. */
    if (critical_request(sid)) {
        return BUS_GUARD_CRITICAL;
    }
    if (sid >= 0x40 && sid != 0x77 && sid != 0x7F &&
        critical_request((uint8_t)(sid - 0x40))) {
        return BUS_GUARD_CRITICAL;
    }
    /* Session control: 0x01 is the default session, which is where a unit
     * is anyway. Anything else is somebody settling in for more. */
    if ((sid == 0x10 || sid == 0x50) && have_sub && (sub & 0x7F) != 0x01) {
        return BUS_GUARD_CRITICAL;
    }
    /* A refusal of one of the above still means somebody tried. */
    if (sid == 0x7F && have_sub && critical_request(sub)) {
        return BUS_GUARD_CRITICAL;
    }
    return BUS_GUARD_UDS;
}

void bus_guard_set_enabled(bool on)
{
    s_enabled = on;
    if (!on) {
        s_active = false;
    }
}

bool bus_guard_enabled(void) { return s_enabled; }

void IRAM_ATTR bus_guard_note(uint32_t id, const uint8_t *d, uint8_t len,
                              uint32_t now_ms)
{
    uint8_t sid = 0;
    switch (bus_guard_classify(id, d, len, &sid)) {
    case BUS_GUARD_CRITICAL:
        s_critical++;
        s_last_id = (uint16_t)id;
        s_last_sid = sid;
        s_last_critical_ms = now_ms;
        if (s_enabled && !s_active) {
            s_active = true;
            s_holds++;
        }
        /* It is foreign traffic as well. */
        __attribute__((fallthrough));
    case BUS_GUARD_UDS:
        s_foreign++;
        s_last_foreign_ms = now_ms;
        s_have_foreign = true;
        break;
    default:
        break;
    }
}

bool bus_guard_blocked(uint32_t now_ms)
{
    if (!s_enabled || !s_active) {
        return false;
    }
    if ((uint32_t)(now_ms - s_last_critical_ms) >= BUS_GUARD_HOLD_MS) {
        s_active = false;
        return false;
    }
    return true;
}

uint32_t bus_guard_settle_ms(uint32_t now_ms)
{
    if (!s_enabled || !s_have_foreign) {
        return 0;
    }
    uint32_t since = (uint32_t)(now_ms - s_last_foreign_ms);
    if (since >= BUS_GUARD_SETTLE_MS) {
        /* Forget it, so the comparison cannot come true again when the
         * millisecond counter wraps after seven weeks. */
        s_have_foreign = false;
        return 0;
    }
    return BUS_GUARD_SETTLE_MS - since;
}

void bus_guard_release(void) { s_active = false; }

void bus_guard_count_refused(void) { s_refused++; }
void bus_guard_count_settled(void) { s_settled++; }

void bus_guard_stats(bus_guard_stats_t *out, uint32_t now_ms)
{
    bool active = bus_guard_blocked(now_ms);
    out->enabled = s_enabled;
    out->active = active;
    out->remaining_s = active
        ? (BUS_GUARD_HOLD_MS - (uint32_t)(now_ms - s_last_critical_ms) + 999) / 1000
        : 0;
    out->holds = s_holds;
    out->critical = s_critical;
    out->foreign = s_foreign;
    out->refused = s_refused;
    out->settled = s_settled;
    out->last_id = s_last_id;
    out->last_sid = s_last_sid;
}
