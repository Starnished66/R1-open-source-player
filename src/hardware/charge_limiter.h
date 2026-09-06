#ifndef CHARGE_LIMITER_H
#define CHARGE_LIMITER_H

#include <stdbool.h>

/* Caps charging at CHARGE_LIMITER_STOP_PERCENT (see charge_limiter.c) to
 * extend battery longevity. Controls the AXP2101 PMIC's charger
 * (module_en/REG18H bit 1, "chg_en" on I2C bus 0, address 0x34).
 * Disabling chg_en turns off the charger state machine across both CC and CV
 * phases, avoiding overshoots. Register REG18H is read-modified-written to
 * preserve gauge_en and watchdog_en.
 *
 * Charging state is verified via REG01 bits[2:0] (charging status).
 *
 * charge_limiter_poll(enabled, false) is called periodically. It self-throttles
 * I2C accesses via CHARGE_LIMITER_REEVALUATE_SECONDS and re-asserts the desired
 * state periodically.
 *
 * Pass force=true to bypass the interval and apply state immediately. */
void charge_limiter_poll(bool enabled, bool force);

/* True while the limiter intends for the charger to be disabled based on
 * battery percentage thresholds, regardless of whether register writes succeeded.
 * Suitable for callers tracking software intent (e.g. idle-shutdown checks). */
bool charge_limiter_is_holding(void);

/* True once a disable_charging() write has succeeded and register readback has
 * confirmed the charger is off. Used where physical confirmation is required
 * (e.g. LED indicators). */
bool charge_limiter_is_confirmed_off(void);

/* Enforces the 500mA charge-current cap while enabled. Disabled is a no-op
 * and does not restore or otherwise modify the current PMIC setting. */
void safe_charging_poll(bool enabled, bool force);

#endif /* CHARGE_LIMITER_H */
