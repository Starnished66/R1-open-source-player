#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdbool.h>

/* R1 Pro charge-status LEDs, confirmed present via /sys/class/leds/{red,blue}
 * on a real device. Originally driven via the kernel's own LED trigger
 * mechanism (red -> "battery-charging", blue -> "battery-full"), matching
 * the same sysfs paths the stock closed-source binary writes to (confirmed
 * via `strings`) -- but real-device testing found EVERY charge-related
 * trigger on this board's kernel build is non-functional: attaching
 * "battery-charging", "axp_battery-charging", "battery-charging-or-full",
 * even "ac-online"/"usb-online", never sets brightness above 0 regardless
 * of actual charge state (confirmed live: brightness stayed 0 for over 5s
 * while genuinely charging). This is the same category of issue as
 * axp_battery's dead voltage_now/current_now (see charge_limiter.h) --
 * sysfs surface exists, the real wiring behind it doesn't on this kernel.
 *
 * Direct brightness writes DO work (confirmed: trigger=none + brightness=1
 * physically lit the LED), so this now drives both LEDs itself instead of
 * delegating to the kernel: red while battery_is_charging() && not full,
 * blue once battery_is_full(), neither otherwise. Requires actual polling
 * now (unlike the old trigger-based approach) since nothing else updates
 * the LEDs as charge state changes -- see led_control_poll(). */

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
