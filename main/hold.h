/* Hold control datapoints against the installation's own regulator.
 *
 * The Vitocal's energy manager writes the storage unit's control datapoints
 * roughly every ten seconds: the grid setpoint to zero, and the charge and
 * discharge limits to their maxima. That is ordinary self-consumption
 * regulation. It runs locally, it is not exposed as a setting anywhere, and
 * switching every management option off in the app changes nothing about it.
 *
 * So none of it is taken over -- it is out-written. This module rewrites the
 * wanted values every two seconds, which beats the regulator's nine and a half
 * by a comfortable margin. Nothing in the installation is modified. Stop, and
 * the manager has its setpoints back within ten seconds by itself.
 *
 * That is the safety property, and it is the reason for the design: no state
 * is left behind. A finished deadline, a reboot, a crash, a pulled cable --
 * every one of them ends every hold, and the system returns to normal
 * operation on its own. Nothing can be left running unattended.
 *
 * Two things are held, independently, each with its own deadline:
 *
 *   2188  the setpoint at the grid connection point. Plain watts in a signed
 *         16-bit field, negative to draw from the grid. Measured twice on a
 *         Vitocharge VX3: -1000 produced 1003 W drawn and 1046 W more
 *         charging, decaying within five seconds of the manager writing back.
 *
 *   2226  the storage's own charge and discharge limits, two 32-bit fields.
 *         Only the extremes are used here -- zero and whatever the manager
 *         itself writes -- because those need no knowledge of the scale, and
 *         the scale is not known. Zero is zero in any unit.
 */
#ifndef O3E_HOLD_H
#define O3E_HOLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GRID_HOLD_DID       2188
#define STORAGE_HOLD_DID    2226

/* Not written by anything here, but the datapoint to look at next: 2239
 * ElectricEnergyStorageControlMode, one byte, is what ViCare's own
 * "Energiemanagement -> Batterie -> Aus" sets to 0. Switching the battery off
 * there leaves 2226's two limits at 100000 untouched -- so the app's off
 * switch is this byte, not the limits this module writes. Both work; they are
 * simply not the same mechanism. */
#define STORAGE_MODE_DID    2239

/* The third field of 2226 and the trailing one of 2188 -- the same number in
 * both, in every message this installation's manager sends. Repeated rather
 * than invented, because nothing here knows what it means.
 *
 * It is NOT a protocol constant. A second VX3, reported on the pull request
 * that added the raw API, carries 15 there instead of 120, and the difference
 * lines up with DID 2239 ElectricEnergyStorageControlMode: 2 on the
 * installation this was measured against, 0 on the one reading 15, where the
 * battery had just been switched off in ViCare. Two installations is not a
 * rule, but it is enough to know the earlier reading -- "a validity period in
 * seconds" -- was a guess dressed up as a fact.
 *
 * Measured stable over 24 s of sampling, so whatever it is, it does not count
 * down. Echoing it back is therefore the safe move either way: a hold writes
 * what the manager itself last wrote in that field, not a number of ours. */
#define HOLD_VALIDITY       120

/* What the manager writes into the limits, and therefore what "no limit"
 * means here. Reads as 100.000 % if the scale is thousandths of a percent,
 * which is the best remaining explanation after the nominal inverter power
 * turned out to be 5880 W rather than the 10 kW the value would need to be
 * tenths of a watt. Nothing here depends on being right about that: only this
 * value and zero are ever written. */
#define HOLD_LIMIT_OPEN     100000

/* Beaten against the regulator's ~9.3 s, with room for a missed turn. */
#define HOLD_PERIOD_MS      2000

/* Caps, deliberately low. A wrong sign or a stray zero should cost minutes and
 * a few hundred watt-hours, not an afternoon. */
#define GRID_HOLD_MAX_W     6000
#define GRID_HOLD_MAX_S     3600

typedef enum {
    /* Not held at all: the manager decides, which is the normal state. */
    STORAGE_MODE_NORMAL = 0,
    STORAGE_MODE_IDLE,             /* neither charge nor discharge */
    STORAGE_MODE_CHARGE_ONLY,      /* charge from surplus, never give back */
    STORAGE_MODE_DISCHARGE_ONLY,   /* discharge, do not take any in */
} storage_mode_t;

typedef struct {
    bool     active;
    uint16_t ecu;
    int16_t  watts;        /* negative draws from the grid */
    uint32_t remaining_s;
    uint32_t writes;
    uint32_t failures;
    char     last_error[96];
} grid_hold_status_t;

typedef struct {
    bool           active;
    storage_mode_t mode;
    uint32_t       remaining_s;
    uint32_t       writes;
    uint32_t       failures;
} storage_hold_status_t;

/* `watts` is negative to draw from the grid, positive to feed in. Fails, with
 * a reason, on a value or duration beyond the caps, or when writing to the bus
 * is switched off in the system settings. Starting again replaces a running
 * hold rather than stacking on it. */
bool grid_hold_start(uint16_t ecu, int16_t watts, uint32_t seconds,
                     char *err, size_t err_sz);

/* Ends the hold and lets the setpoint go. Does not write a neutral value: the
 * manager restores it within ten seconds anyway, and writing one more time
 * would be one more chance to write it wrongly. */
void grid_hold_stop(void);

/* Start from the stored settings, or stop. This is what a Home Assistant
 * switch flips: the switch has no room to carry a power and a duration, so it
 * uses the ones in the system settings. */
bool grid_hold_switch(bool on, char *err, size_t err_sz);

void grid_hold_status(grid_hold_status_t *out);

/* STORAGE_MODE_NORMAL stops the hold; every other mode starts or replaces one.
 * The duration is capped like the grid hold's. */
bool storage_hold_start(uint16_t ecu, storage_mode_t mode, uint32_t seconds,
                        char *err, size_t err_sz);
void storage_hold_status(storage_hold_status_t *out);
const char *storage_mode_name(storage_mode_t mode);
bool storage_mode_parse(const char *name, storage_mode_t *out);

/* Tell the holder that the manager has just written this datapoint.
 *
 * Without it the two sides take turns on a timer: the manager sets its value,
 * and up to two seconds pass before the next scheduled rewrite takes it back.
 * The installation follows whichever is current, so the delivered power
 * wanders back and forth once every nine seconds -- small, but visible, and
 * pointless. The manager announces every write on its broadcast channel, which
 * the collect receiver already decodes, so the rewrite can follow it by a few
 * milliseconds instead of by a timer.
 *
 * Safe to call from the collect task; it only wakes the holder. */
void hold_note_foreign(uint16_t did);

/* Publish both holds to <base>/hold, retained. Called on every change and
 * regularly while one runs, so a subscriber that connects late still learns
 * that something is overriding the installation's own regulation. */
void hold_publish(void);

#endif /* O3E_HOLD_H */
