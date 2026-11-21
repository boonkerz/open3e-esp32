/* What counts as "somebody else is programming a control unit".
 *
 * A miss here is silent in the bad direction: the gateway keeps polling into
 * a download. A false alarm is loud but harmless: it stops polling for ten
 * minutes on a bus where nothing is wrong. Both are pinned down with the
 * frames each case really consists of.
 */
#include <stdio.h>
#include <string.h>

#include "../main/bus_guard.h"

static int fail;

static void ok(const char *what, int cond)
{
    if (!cond) {
        fail++;
        printf("  FAIL %s\n", what);
    }
}

static bus_guard_class_t cls(uint32_t id, const char *hex)
{
    uint8_t d[8];
    uint8_t n = 0;
    for (; hex[0] && hex[1] && n < 8; hex += 2) {
        unsigned v;
        sscanf(hex, "%2x", &v);
        d[n++] = (uint8_t)v;
    }
    return bus_guard_classify(id, d, n, NULL);
}

static void note(uint32_t id, const char *hex, uint32_t now)
{
    uint8_t d[8];
    uint8_t n = 0;
    for (; hex[0] && hex[1] && n < 8; hex += 2) {
        unsigned v;
        sscanf(hex, "%2x", &v);
        d[n++] = (uint8_t)v;
    }
    bus_guard_note(id, d, n, now);
}

int main(void)
{
    /* --- the replies seen in open3e discussion 413 --- */
    ok("RoutineControl reply",      cls(0x69C, "0471010203") == BUS_GUARD_CRITICAL);
    ok("ECUReset reply",            cls(0x69C, "025101") == BUS_GUARD_CRITICAL);
    ok("RequestDownload reply",     cls(0x69C, "0474200FFA") == BUS_GUARD_CRITICAL);
    ok("TransferData reply",        cls(0x69C, "027601") == BUS_GUARD_CRITICAL);
    /* --- and the requests that cause them --- */
    ok("RoutineControl request",    cls(0x68C, "0431010203") == BUS_GUARD_CRITICAL);
    ok("ECUReset request",          cls(0x68C, "021101") == BUS_GUARD_CRITICAL);
    ok("RequestDownload, first frame",
       cls(0x68C, "100B340044000100") == BUS_GUARD_CRITICAL);
    ok("TransferData, first frame", cls(0x68C, "1102360100112233") == BUS_GUARD_CRITICAL);
    ok("RequestTransferExit",       cls(0x68C, "0137") == BUS_GUARD_CRITICAL);
    ok("programming session",       cls(0x68C, "021002") == BUS_GUARD_CRITICAL);
    ok("extended session, reply suppressed",
       cls(0x68C, "021083") == BUS_GUARD_CRITICAL);
    ok("session reply",             cls(0x69C, "065003003201F4") == BUS_GUARD_CRITICAL);
    ok("refused download",          cls(0x69C, "037F3433") == BUS_GUARD_CRITICAL);

    /* --- ordinary traffic: a gap, not a hold --- */
    ok("read request",              cls(0x680, "0322010C") == BUS_GUARD_UDS);
    ok("read reply",                cls(0x690, "0562010C1234") == BUS_GUARD_UDS);
    ok("read reply, first frame",   cls(0x690, "1027620100AABBCC") == BUS_GUARD_UDS);
    ok("write request",             cls(0x680, "052E018C0102") == BUS_GUARD_UDS);
    ok("Viessmann's write service", cls(0x680, "1009770204011122") == BUS_GUARD_UDS);
    ok("its service byte as a reply is not a download",
       cls(0x690, "0177") == BUS_GUARD_UDS);
    ok("default session",           cls(0x680, "021001") == BUS_GUARD_UDS);
    ok("tester present",            cls(0x680, "023E80") == BUS_GUARD_UDS);
    ok("refused read",              cls(0x690, "037F2231") == BUS_GUARD_UDS);

    /* --- not a message start at all --- */
    ok("consecutive frame",         cls(0x68C, "2136343536373811") == BUS_GUARD_NONE);
    ok("flow control",              cls(0x69C, "300000") == BUS_GUARD_NONE);
    ok("collect stream",            cls(0x693, "21010C0512340000") == BUS_GUARD_NONE);
    ok("single frame longer than the frame",
       cls(0x68C, "0731") == BUS_GUARD_NONE);
    ok("first frame shorter than a single one",
       cls(0x68C, "1005340000000000") == BUS_GUARD_NONE);
    ok("outside the diagnostic range",
       cls(0x451, "021101") == BUS_GUARD_NONE);
    ok("one byte",                  cls(0x68C, "02") == BUS_GUARD_NONE);

    /* --- the hold --- */
    bus_guard_stats_t st;
    uint32_t t = 4294000000u;            /* close to the wrap, on purpose */
    bus_guard_set_enabled(true);
    ok("free at first", !bus_guard_blocked(t));
    note(0x680, "0322010C", t);
    ok("an ordinary request does not hold", !bus_guard_blocked(t));
    ok("but asks for a gap", bus_guard_settle_ms(t + 50) == BUS_GUARD_SETTLE_MS - 50);
    ok("which ends", bus_guard_settle_ms(t + BUS_GUARD_SETTLE_MS) == 0);
    ok("and stays ended", bus_guard_settle_ms(t + 60) == 0);

    note(0x68C, "100B340044000100", t);
    ok("a download holds", bus_guard_blocked(t + 1));
    bus_guard_stats(&st, t + 1000);
    ok("stats name the frame", st.active && st.last_id == 0x68C && st.last_sid == 0x34
                               && st.holds == 1 && st.critical == 1);
    ok("stats count down", st.remaining_s == BUS_GUARD_HOLD_MS / 1000 - 1);
    ok("still held just before the end", bus_guard_blocked(t + BUS_GUARD_HOLD_MS - 1));
    note(0x69C, "027601", t + BUS_GUARD_HOLD_MS - 1);
    ok("each further frame extends it",
       bus_guard_blocked(t + BUS_GUARD_HOLD_MS + 1000));
    bus_guard_stats(&st, t + BUS_GUARD_HOLD_MS + 1000);
    ok("without starting a second hold", st.holds == 1 && st.critical == 2);
    uint32_t end = t + BUS_GUARD_HOLD_MS - 1 + BUS_GUARD_HOLD_MS;   /* wraps */
    ok("the end is past the counter's wrap", end < t);
    ok("held across the wrap", bus_guard_blocked(end - 1));
    ok("released after the silence", !bus_guard_blocked(end));
    ok("and stays released", !bus_guard_blocked(end + 5));

    note(0x68C, "021101", end + 10);
    ok("a reset holds again", bus_guard_blocked(end + 11));
    bus_guard_release();
    ok("released by hand", !bus_guard_blocked(end + 12));
    note(0x68C, "021101", end + 20);
    ok("the next frame holds again", bus_guard_blocked(end + 21));

    bus_guard_set_enabled(false);
    ok("switched off: free", !bus_guard_blocked(end + 22));
    note(0x68C, "021101", end + 30);
    ok("switched off: stays free", !bus_guard_blocked(end + 31));
    ok("switched off: no gap either", bus_guard_settle_ms(end + 31) == 0);
    bus_guard_stats(&st, end + 31);
    ok("but still counts what it sees", st.critical == 5 && !st.active && !st.enabled);
    bus_guard_set_enabled(true);
    ok("switched on again: old frames do not hold", !bus_guard_blocked(end + 40));

    puts(fail ? "FAILED" : "all checks passed");
    return fail ? 1 : 0;
}
