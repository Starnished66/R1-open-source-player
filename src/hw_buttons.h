#ifndef HW_BUTTONS_H
#define HW_BUTTONS_H

#include <stdbool.h>

/* Starts a background thread reading the R1's physical volume/skip/play-pause
 * buttons. They're exposed as two separate evdev nodes (md-gpio-keys and
 * "jz adc keyboard"), already decoded by the kernel driver into standard key
 * codes (KEY_VOLUMEUP/DOWN, KEY_PLAYPAUSE, KEY_NEXTSONG/PREVIOUSSONG).
 *
 * These act as direct hardware shortcuts, not keypad navigation for the UI,
 * so this bypasses LVGL's indev/keypad abstraction entirely: the reader
 * thread only sets flags/counters here, and the GUI's own periodic timer
 * (already running on the one thread allowed to touch LVGL widgets) polls
 * and applies them via the consume_* functions below. */
void hw_buttons_init(void);

bool hw_buttons_consume_play_pause(void);
bool hw_buttons_consume_next(void);
bool hw_buttons_consume_prev(void);

/* Power button, short tap: true once a press-and-release completes without
 * having crossed the long-press threshold (see hw_buttons_consume_power_
 * long_press() below) -- the existing "toggle the screen on/off" gesture.
 * Fires on release, not on press-down, specifically so it can be suppressed
 * when the same press turns into a long-press instead (see
 * handle_key_event()'s own comment in the .c file). Must be applied on the
 * GUI thread (not the reader thread that detects it) so the backlight
 * toggle and the LVGL inactivity clock it shares with auto screen-timeout
 * stay in sync -- see gui.c's update_timer_cb. */
bool hw_buttons_consume_power(void);

/* Power button, long press: true once, edge-triggered, the moment a hold
 * crosses the long-press threshold -- while still held, not on release --
 * so the consumer (gui.c) can bring up a power-off countdown immediately
 * rather than waiting for the finger to lift. Mutually exclusive with
 * hw_buttons_consume_power() for the same physical press: once this fires,
 * that press's eventual release does not also set the short-tap flag. */
bool hw_buttons_consume_power_long_press(void);

/* Net accumulated volume step (in percent) since the last call, then reset
 * to 0. Positive for volume up, negative for volume down. */
int hw_buttons_consume_volume_delta(void);

#endif /* HW_BUTTONS_H */
