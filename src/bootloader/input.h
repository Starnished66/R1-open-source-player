#ifndef BOOTLOADER_INPUT_H
#define BOOTLOADER_INPUT_H

#include <stdbool.h>

typedef enum {
    BL_INPUT_NONE = 0,
    BL_INPUT_MOVE_UP,
    BL_INPUT_MOVE_DOWN,
    BL_INPUT_CONFIRM,
    /* Fired on the press (down) transition -- "any input cancels the
     * countdown" must not wait for release, or a finger already resting
     * on a card near the deadline lets the default boot out from under
     * it before the eventual lift-off ever registers. Cancels the
     * countdown only; does not select or confirm anything by itself. */
    BL_INPUT_TOUCH_DOWN,
    /* Fired on the release (up) transition -- this is what actually
     * selects/confirms a card (see main.c's own point_in_card() use). */
    BL_INPUT_TOUCH_TAP,
} bl_input_type_t;

typedef struct {
    bl_input_type_t type;
    /* Meaningful for BL_INPUT_TOUCH_DOWN and BL_INPUT_TOUCH_TAP -- raw
     * touch coordinates, already in native 0..FB_WIDTH-1 / 0..FB_HEIGHT-1
     * screen pixels (see input.c's own doc comment on why no calibration
     * step is needed on this exact hardware). */
    int x;
    int y;
} bl_input_event_t;

/* Opens whichever of hyn_ts / md-gpio-keys / jz adc keyboard are actually
 * present (by name, via find_input_device_by_name() -- see input.c's own
 * doc comment for why this device's hardware needs all three, not a
 * hardcoded event0/event1 pair). Returns false only if NONE of the three
 * are found -- main.c should still proceed with a menu in that case
 * (countdown-only, no navigation), not treat it as fatal. */
bool input_open(void);
void input_close(void);

/* True if at least one device opened by input_open() is still usable.
 * input_poll() itself closes and drops any fd that reports POLLHUP/
 * POLLERR/POLLNVAL (a disconnected/errored device) -- callers must
 * re-check this rather than trusting input_open()'s own one-time return
 * value for the whole run, or a poll(-1) (indefinite block, used once the
 * countdown is cancelled) called with zero fds left open would return
 * immediately forever instead of actually blocking. */
bool input_any_open(void);

/* Polls every open device for up to timeout_ms (or indefinitely, if
 * negative) and returns the first event recognized, or BL_INPUT_NONE on
 * timeout. Never blocks longer than timeout_ms regardless of how many
 * devices are open. Safe to call with zero devices open (returns
 * BL_INPUT_NONE immediately without blocking) -- callers that need to
 * actually wait in that case should check input_any_open() first. */
bl_input_event_t input_poll(int timeout_ms);

#endif /* BOOTLOADER_INPUT_H */
