#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdbool.h>

/* R1 Pro charge-status LEDs, confirmed present via /sys/class/leds/{red,blue}
 * on a real device.
 *
 * The driver's advertised max_brightness is 100, but real-device testing
 * found that trigger=none + brightness=100 only flashes the physical LED
 * briefly and then leaves it dark even though sysfs continues to read back
 * 100. A direct brightness=50 write remains visibly lit, so manual control
 * deliberately uses that confirmed-working midpoint rather than the
 * misleading advertised maximum. See led_control.c.
 *
 * Real, still-relevant issue (unrelated to brightness): the kernel's own
 * charge-status LED triggers ("battery-charging", "battery-full", etc.)
 * were tried and abandoned early on because this device's kernel power_
 * supply status can go stale specifically around this app's own charge_
 * limiter.c -- chg_en can be cleared via a raw i2c write the kernel's
 * power_supply core never observes, so anything (a trigger or this app's
 * own battery_is_charging()/battery_is_full()) that reads that status can
 * keep reporting "Charging" long after real charging actually stopped (see
 * charge_limiter.h's own comment on the identical staleness). This module
 * still polls and decides state itself for that reason -- battery_is_
 * charging()/battery_is_full() PLUS charge_limiter_is_confirmed_off() (NOT
 * charge_limiter_is_holding() -- that reflects intent, not a confirmed i2c
 * write, see its own comment) as the override for "charging was
 * intentionally capped, treat it as done", further gated on battery_get_
 * external_power_state() so an unplug shortly after capping doesn't leave
 * the blue "done charging" LED lit while running on battery -- see led_
 * control_poll()'s own comment for the exact logic. */

/* Call once at startup and whenever the user flips the settings toggle --
 * forces trigger=none on both LEDs (taking exclusive manual control away
 * from the kernel) and applies the current state immediately rather than
 * waiting for the next poll tick. enabled=false turns both LEDs off and
 * leaves them off regardless of charge state until re-enabled. */
void led_control_apply(bool enabled);

/* Call on every timer tick regardless of enabled state (matches
 * charge_limiter_poll()'s own calling convention) -- re-reads real charge
 * state and updates brightness only when it actually changed, so this is
 * cheap to call unconditionally. No-op whenever enabled is false (the LEDs
 * were already forced off by the most recent led_control_apply(false)). */
void led_control_poll(bool enabled);

#endif /* LED_CONTROL_H */
