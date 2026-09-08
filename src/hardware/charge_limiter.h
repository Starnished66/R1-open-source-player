#ifndef CHARGE_LIMITER_H
#define CHARGE_LIMITER_H

#include <stdbool.h>

/* Caps the AXP2101's charge-termination voltage to 4.2V (register 0x64,
 * I2C bus 0 address 0x34) while enabled, extending battery longevity by
 * capping the target voltage the same way phone/laptop "80% battery
 * health" features typically do -- rather than literally disabling the
 * charger once a percentage threshold is crossed, letting CC/CV charging
 * taper off and complete naturally at the lower ceiling. The charger
 * itself (module_en/REG18H bit 1, "chg_en") is always kept enabled; this
 * module only ever moves the voltage target, never the charger on/off
 * state, so a charge can never get stuck disabled across a restart.
 *
 * On the R3 Pro II (BOARD_R3PROII), the AXP2101's cap has no effect on the
 * physical charge path -- the dedicated MP2731 charger IC (I2C address
 * 0x4b) is capped to its own matching 4.2V encoding at the same time.
 *
 * Disabling restores each chip's true stock voltage target -- a known
 * per-board constant (AXP_VOLTAGE_BASELINE/MP_VOLTAGE_BASELINE in
 * charge_limiter.c, pulled directly from each board's own stock firmware
 * boot script), not a guessed or live-captured value.
 *
 * charge_limiter_poll(enabled, false) is called periodically. It self-throttles
 * I2C accesses via CHARGE_LIMITER_REEVALUATE_SECONDS and re-asserts the desired
 * state periodically.
 *
 * Pass force=true to bypass the interval and apply state immediately. */
void charge_limiter_poll(bool enabled, bool force);

/* True while the limiter intends the 4.2V voltage cap to be in effect,
 * regardless of whether the last register write actually succeeded.
 * Note this is NOT tied to any specific battery percentage -- it becomes
 * true as soon as the cap is applied, however far into charging that is.
 * Suitable for callers tracking software intent (e.g. idle-shutdown checks). */
bool charge_limiter_is_holding(void);

/* True once the voltage cap is in effect AND charging has actually tapered
 * off/completed under it (AXP2101 REG01 charge-done status on R1; on R3
 * Pro II, battery_is_full() instead, since the MP2731 -- not the AXP2101 --
 * is the chip actually driving the charge cycle there) -- i.e. "capped and
 * no longer actively charging", not merely "cap applied". Used where physical
 * confirmation is required (e.g. LED indicators). */
bool charge_limiter_is_confirmed_off(void);

/* Enforces the 500mA charge-current cap while enabled, on both the AXP2101
 * and the MP2731 (the AXP2101's cap has no effect on the R3Pro II, so the
 * MP2731 register is set to its closest equivalent, 480mA). The pre-cap
 * register values are captured the first time the cap is applied each run
 * and written back when disabled, restoring whatever charge-current setting
 * was in place before this module touched it. */
void safe_charging_poll(bool enabled, bool force);

#endif /* CHARGE_LIMITER_H */
