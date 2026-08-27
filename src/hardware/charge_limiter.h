#ifndef CHARGE_LIMITER_H
#define CHARGE_LIMITER_H

#include <stdbool.h>

/* Caps charging at CHARGE_LIMITER_STOP_PERCENT (see charge_limiter.c) to
 * extend battery longevity, the same feature phones call "optimized" or
 * "limited" charging -- there is no dedicated state-of-charge cutoff knob on
 * this hardware, so this works by disabling the AXP2101 PMIC's own charger
 * outright (module_en/REG18H bit 1, "chg_en" -- i2c bus 0, address 0x34)
 * just before the target is reached, and re-enabling it (same bit) only
 * after a small discharge hysteresis window or when the feature is turned
 * off. This prevents noisy 84/85% readings from repeatedly restarting the
 * charger and overshooting the requested 85% ceiling.
 *
 * This is the SECOND register-level approach this feature has gone through:
 *
 * 1. Originally wrote to /sys/class/power_supply/usb/charge_control_limit_max.
 *    That sysfs node turned out to be wired to a completely different, wrong
 *    register -- confirmed on a real device by reading raw PMIC registers
 *    directly: REG16 (Input Current Limit, which caps how much the USB port
 *    is allowed to supply, not how much goes into the battery) read back
 *    exactly the value that sysfs write kept reverting to, and writing "0"
 *    to it was silently rejected outright (0mA isn't one of REG16's six
 *    legal input-current-limit codes).
 * 2. Switched to REG62 (ICC, "fast charge current" -- the constant-CURRENT
 *    phase target), zeroed once the threshold was reached. This DID reach
 *    the right register (confirmed against the official X-Powers AXP2101
 *    datasheet and a live read/write/read round-trip), but real-device
 *    testing (2026-08-07) found charging actually stopped around 91-92%,
 *    not the configured 85%. Per the datasheet (section 7.7.3.3), once
 *    VBAT reaches the target voltage the charger enters constant-VOLTAGE
 *    (CV) mode and holds output voltage constant while current tapers on
 *    its own -- REG62's CC-phase ceiling stops being the active constraint
 *    once CV phase has begun, which real-device behavior indicates happens
 *    well before 85% state-of-charge on this battery. Zeroing REG62 at that
 *    point does nothing; the charger just runs its own CV taper down to its
 *    own internal termination-current threshold (ITERM) and completes on
 *    its own, landing wherever that naturally lands (~91-92% here) rather
 *    than at the configured stop percent.
 *
 * chg_en (module_en bit 1) disables the WHOLE charger state machine, CC and
 * CV phases alike, not just a CC-phase current ceiling -- confirmed against
 * the same datasheet section, which describes both "the charger is enabled
 * when an adapter is inserted" and "charger enable bit is reset to 1" (to
 * exit battery-safe-mode) as controls over this exact bit. Always
 * read-modify-write this register (see charge_limiter.c) -- it also holds
 * gauge_en (the fuel gauge) and watchdog_en, which must never be touched by
 * this code.
 *
 * There is still no live way to observe the actual resulting battery charge
 * current from software -- axp_battery/usb's voltage_now/current_now sysfs
 * attributes are static passthroughs of configured maximums, not real ADC
 * readings (confirmed directly: current_now stayed frozen at the same value
 * regardless of what REG62 or the PMIC's own charger-enable bit were set
 * to). The one thing that IS live and trustworthy is REG01's bits[2:0]
 * charging-status field straight from the PMIC (tri-charge / pre-charge /
 * constant-current / constant-voltage / charge-done / not charging) --
 * charge_limiter.c DBG_LOGs this before/after every throttle/restore call
 * as the way to confirm the change actually took effect, not the sysfs
 * battery/status or usb/current_now attributes (both were caught reporting
 * stale/wrong values during this investigation).
 *
 * Call charge_limiter_poll(enabled, false) on every timer tick regardless
 * of the toggle state -- it self-throttles its own i2c reads/writes
 * internally (see CHARGE_LIMITER_REEVALUATE_SECONDS), and re-asserts the
 * desired state every interval rather than only on threshold-crossing
 * edges, so a charger unplug/replug always self-corrects within one
 * interval instead of needing extra edge-case handling at each call site.
 * It's also self-healing across an app crash mid-throttle: chg_en resets to
 * its POR default (1, enabled -- "System Reset" reset type per the
 * datasheet's own register table) on an actual power-on-reset, and every
 * call site already passes force=true once at startup, so a relaunch
 * re-evaluates and re-applies the correct state immediately either way.
 *
 * Pass force=true right after the user flips the settings toggle (and once
 * at startup) to bypass that interval and apply immediately -- otherwise
 * turning the feature off could leave charging throttled for up to
 * CHARGE_LIMITER_REEVALUATE_SECONDS after the user expected it to resume. */
void charge_limiter_poll(bool enabled, bool force);

/* True while the 85% limiter WANTS the charger disabled -- decided purely
 * from the percent thresholds, before disable_charging() is even attempted,
 * and stays true through a failed-write retry window regardless of whether
 * the i2c operation actually took effect. Fine for callers that only need
 * the app's own intent (e.g. gui.c's idle-shutdown eligibility check).
 * Used by the UI (that intent, not this device's kernel battery/status
 * node) because that kernel node can stay stuck on "Charging" even after
 * the PMIC confirms charging has stopped. For anything that needs to know
 * the charger is ACTUALLY off right now, use charge_limiter_is_confirmed_
 * off() below instead. */
bool charge_limiter_is_holding(void);

/* True only once a disable_charging() write has actually been issued AND
 * its own register-readback check confirmed it took effect -- stays at its
 * last confirmed value (not the newly-desired one) while a write is
 * failing/retrying. Distinct from charge_limiter_is_holding() above, which
 * can already read true before the very first disable attempt is even
 * made. Use this, not charge_limiter_is_holding(), for anything (like
 * led_control.c's blue "charging complete/capped" indicator) that would be
 * actively misleading if shown before the charger is confirmed off. */
bool charge_limiter_is_confirmed_off(void);

/* Enforces the 500mA charge-current cap while enabled. Disabled is a no-op
 * and does not restore or otherwise modify the current PMIC setting. */
void safe_charging_poll(bool enabled, bool force);

#endif /* CHARGE_LIMITER_H */
