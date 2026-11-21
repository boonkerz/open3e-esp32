/* Keep quiet while somebody else is talking UDS to the control units.
 *
 * The request identifier of an E3 control unit says who is addressed, not who
 * is asking: the gateway and the installation's own controller are the same
 * tester as far as the addressed device can tell. Normally that does not
 * matter, because on most buses nobody else asks anything -- ninety seconds
 * on a Vitocal's external bus showed not one foreign frame in the diagnostic
 * range. It matters during commissioning and software updates, when the
 * controller itself opens sessions, starts routines and downloads data to
 * the other units. A read request of ours in the middle of such a segmented
 * transfer lands in the same ISO-TP receiver and can abort it
 * (open3e/open3e discussion 413: outdoor unit gone after a commissioning run
 * with open3e polling the internal bus).
 *
 * So the receive path shows every frame in the diagnostic range to this
 * module, and it answers two questions for whoever wants to transmit:
 *
 *   * Is a programming-type exchange going on? Then nothing is sent at all
 *     until the bus has been free of them for BUS_GUARD_HOLD_MS.
 *   * Did somebody else just ask something ordinary? Then wait for a short
 *     gap first, bounded, so a controller that polls all day cannot starve
 *     the gateway.
 *
 * It cannot see the first foreign frame coming, so an exchange of ours may
 * still coincide with it. Unplugging the gateway before a commissioning run
 * remains the advice; this is the belt to that pair of braces.
 *
 * No FreeRTOS, no allocation, no logging, time passed in: the classification
 * is the part that must not be wrong, and it is tested on a workstation.
 */
#ifndef O3E_BUS_GUARD_H
#define O3E_BUS_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/* Request identifiers 0x680..0x6EF, replies sixteen above. */
#define BUS_GUARD_ID_FIRST 0x680
#define BUS_GUARD_ID_LAST  0x6FF

/* Silence after the last programming-type frame before polling resumes. A
 * commissioning run pauses between its steps for as long as the person at
 * the panel takes to read the next screen. */
#define BUS_GUARD_HOLD_MS    (10 * 60 * 1000)
/* Gap to leave after an ordinary foreign request or reply... */
#define BUS_GUARD_SETTLE_MS  150
/* ...and the longest a single exchange of ours is delayed for it. */
#define BUS_GUARD_SETTLE_MAX_MS 1500

typedef enum {
    BUS_GUARD_NONE = 0,   /* not a diagnostic message start */
    BUS_GUARD_UDS,        /* somebody else's request or reply */
    BUS_GUARD_CRITICAL,   /* session change, reset, routine, download */
} bus_guard_class_t;

/* What a received frame is. `sid` receives the service byte when the result
 * is not BUS_GUARD_NONE. Pure. */
bus_guard_class_t bus_guard_classify(uint32_t id, const uint8_t *d, uint8_t len,
                                     uint8_t *sid);

void bus_guard_set_enabled(bool on);
bool bus_guard_enabled(void);

/* Feed a received frame that is not the reply the gateway is waiting for.
 * Called from the receive interrupt. */
void bus_guard_note(uint32_t id, const uint8_t *d, uint8_t len, uint32_t now_ms);

/* True while nothing may be transmitted. */
bool bus_guard_blocked(uint32_t now_ms);
/* Milliseconds still to wait for the gap after an ordinary foreign message;
 * 0 means go. */
uint32_t bus_guard_settle_ms(uint32_t now_ms);

/* End a hold early: the person at the device knows the commissioning run is
 * over. A new programming-type frame starts the next one. */
void bus_guard_release(void);

/* Bookkeeping by the transmit path. */
void bus_guard_count_refused(void);
void bus_guard_count_settled(void);

typedef struct {
    bool     enabled;
    bool     active;        /* holding right now */
    uint32_t remaining_s;   /* until polling resumes, if nothing else comes */
    uint32_t holds;         /* how often a hold began */
    uint32_t critical;      /* programming-type frames seen */
    uint32_t foreign;       /* ordinary foreign messages seen */
    uint32_t refused;       /* exchanges of ours not sent because of a hold */
    uint32_t settled;       /* exchanges of ours delayed for a gap */
    uint16_t last_id;       /* of the last programming-type frame */
    uint8_t  last_sid;
} bus_guard_stats_t;

void bus_guard_stats(bus_guard_stats_t *out, uint32_t now_ms);

#endif /* O3E_BUS_GUARD_H */
