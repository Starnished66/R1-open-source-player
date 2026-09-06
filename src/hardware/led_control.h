#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdbool.h>

/* R1 Pro charge-status LEDs (/sys/class/leds/{red,blue}).
 * Uses brightness 50 for a stable visible level.
 * Gated by charge_limiter_is_confirmed_off() and external power state to
 * ensure red indicates active charging and blue indicates full or capped charging
 * while connected to power. */

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
