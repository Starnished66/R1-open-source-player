#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdbool.h>

/* Screen backlight brightness, via the standard Linux backlight sysfs class
 * (confirmed present on a real R1: /sys/class/backlight/backlight_pwm0,
 * writable as root).
 *
 * Every function below works in "logical" percent -- a clean 0-100 the UI
 * can show directly ("0%" and "100%" included). Internally, backlight.c
 * maps that to a narrower safe raw range before ever touching hardware:
 * never all the way down to a literal 0 (see BACKLIGHT_MIN_PERCENT below),
 * and never the literal max_brightness value either -- confirmed live on a
 * real R1 that writing that exact raw value makes the screen fully
 * invisible instead of "as bright as possible" (a PWM-at-max-duty-cycle
 * glitch, not something fixable from software beyond just never writing
 * that value). Callers never need to think about either edge themselves. */

/* Real-hardware feedback: letting the quick-drawer slider go all the way to
 * a literal 0 turns the screen fully black with no way to see what you're
 * doing to bring it back up. Never go lower than this (in raw/safe-percent
 * terms) in either direction -- logical 0 still maps here, not to a true
 * off; only backlight_set_screen_on(false) ever writes a true 0. */
#define BACKLIGHT_MIN_PERCENT 5

/* Current brightness as logical 0-100, or -1 if no backlight class device
 * is present (host, or a future unit with a different driver stack
 * entirely). */
int backlight_get_percent(void);

/* Sets brightness to logical `percent`, clamped to [0, 100] and mapped to
 * the safe raw range described above. No-op if no backlight class device
 * is present. */
void backlight_set_percent(int percent);

/* Screen power (fully off, not just dimmed to BACKLIGHT_MIN_PERCENT) -- the
 * single shared source of truth behind both the hardware power button
 * (hw_buttons.c) and the auto screen-timeout (gui.c), so the two can't
 * desync into two different ideas of whether the screen is on. Thread-safe:
 * hw_buttons.c calls this from its own dedicated button-reading thread,
 * gui.c calls it from the main/GUI thread. */
bool backlight_screen_is_on(void);

/* Turning off remembers the real current brightness (whatever the user last
 * set via the quick-drawer slider) and restores exactly that on the next
 * turn-on, rather than resetting to a fixed level. No-op if the screen is
 * already in the requested state (no redundant sysfs writes). */
void backlight_set_screen_on(bool on);

#endif /* BACKLIGHT_H */
