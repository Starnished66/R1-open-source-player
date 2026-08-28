#include "input.h"
#include "input_device_utils.h"
#include "fb_draw.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/* Three separate evdev devices on this exact hardware -- confirmed in
 * src/hardware/hw_buttons.c's own real-device comment: "Power and
 * Next/Prev live on md-gpio-keys, Volume and Play/Pause live on 'jz adc
 * keyboard'", plus the touchscreen ("hyn_ts") as a third, independent
 * device (src/main.c). A generic /dev/input/event0-and-event1 guess (an
 * earlier draft of this bootloader's design assumed exactly that) is both
 * wrong about which two devices matter and short by one. */
#define DEV_TOUCH 0
#define DEV_GPIO_KEYS 1
#define DEV_ADC_KEYBOARD 2
#define DEV_COUNT 3

static int fds[DEV_COUNT] = { -1, -1, -1 };

/* Last-known absolute touch position, updated as EV_ABS events stream in
 * and only turned into a BL_INPUT_TOUCH_TAP once release is detected --
 * same event shape lvgl/src/drivers/evdev/lv_evdev.c already relies on for
 * this exact touch controller (confirmed by reading that driver directly):
 * position from ABS_X/ABS_MT_POSITION_X and ABS_Y/ABS_MT_POSITION_Y
 * (interchangeable -- this controller's exact variant doesn't matter),
 * press/release from ABS_MT_TRACKING_ID or BTN_TOUCH/BTN_MOUSE, whichever
 * arrives. No calibration step: src/main.c never calls
 * lv_evdev_set_calibration() for this touch device, and lv_evdev.c's own
 * _evdev_calibrate() is a no-op (returns the raw value, just clamped) when
 * min==max==0 (the uninitialized default) -- i.e. this controller's raw
 * ABS values are already native screen-pixel coordinates on this device. */
static int touch_x = 0;
static int touch_y = 0;
static bool touch_down = false;

bool input_open(void) {
    /* O_CLOEXEC defensively, in addition to main.c's own explicit
     * input_close() before the fork/execve handoff -- belt and suspenders:
     * even if some future code path forgot the explicit close, these must
     * never leak into the player process across execve(). */
    char path[64];
    if (find_input_device_by_name("hyn_ts", path, sizeof(path)))
        fds[DEV_TOUCH] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (find_input_device_by_name("md-gpio-keys", path, sizeof(path)))
        fds[DEV_GPIO_KEYS] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (find_input_device_by_name("jz adc keyboard", path, sizeof(path)))
        fds[DEV_ADC_KEYBOARD] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    return fds[DEV_TOUCH] >= 0 || fds[DEV_GPIO_KEYS] >= 0 || fds[DEV_ADC_KEYBOARD] >= 0;
}

void input_close(void) {
    for (int i = 0; i < DEV_COUNT; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Drains every ready byte on one fd, updating touch_x/y/down and returning
 * the first navigation/confirm/tap event it produces, or BL_INPUT_NONE if
 * this fd's events didn't produce one (e.g. pure touch movement with no
 * release yet). O_NONBLOCK matters here for the exact reason
 * src/hardware/hw_buttons.c's own comment documents: a blocking read on
 * one device, once its queue empties, starves poll() from ever being
 * called again for the OTHER devices. */
static bl_input_event_t drain_fd(int fd, int which) {
    bl_input_event_t none = { BL_INPUT_NONE, 0, 0 };
    struct input_event ev;
    bl_input_event_t result = none;

    while (read(fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
        if (which == DEV_TOUCH) {
            bool new_down = touch_down;
            bool down_known = false; /* did this event actually carry a press/release signal? */

            if (ev.type == EV_ABS) {
                if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) touch_x = clampi(ev.value, 0, FB_WIDTH - 1);
                else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) touch_y = clampi(ev.value, 0, FB_HEIGHT - 1);
                else if (ev.code == ABS_MT_TRACKING_ID) {
                    new_down = (ev.value != -1);
                    down_known = true;
                }
            } else if (ev.type == EV_KEY && (ev.code == BTN_TOUCH || ev.code == BTN_MOUSE)) {
                new_down = (ev.value != 0);
                down_known = true;
            }

            /* Either source of a press/release signal can fire the tap --
             * this controller isn't guaranteed to send both for the same
             * lift-off (some multi-touch protocols only ever send the
             * tracking-ID release, never a BTN_TOUCH=0 to match), so both
             * paths must be able to trigger it, not just one. */
            if (down_known) {
                bool was_down = touch_down;
                touch_down = new_down;
                if (!was_down && touch_down) {
                    /* Press -- cancels the countdown immediately (see
                     * BL_INPUT_TOUCH_DOWN's own doc comment for why this
                     * can't wait for release). */
                    result.type = BL_INPUT_TOUCH_DOWN;
                    result.x = touch_x;
                    result.y = touch_y;
                } else if (was_down && !touch_down) {
                    result.type = BL_INPUT_TOUCH_TAP;
                    result.x = touch_x;
                    result.y = touch_y;
                }
            }
        } else if (which == DEV_GPIO_KEYS) {
            if (ev.type == EV_KEY && ev.value == 1 && ev.code == KEY_POWER) result.type = BL_INPUT_CONFIRM;
        } else if (which == DEV_ADC_KEYBOARD) {
            if (ev.type == EV_KEY && ev.value == 1) {
                if (ev.code == KEY_VOLUMEUP) result.type = BL_INPUT_MOVE_UP;
                else if (ev.code == KEY_VOLUMEDOWN) result.type = BL_INPUT_MOVE_DOWN;
                else if (ev.code == KEY_PLAYPAUSE) result.type = BL_INPUT_CONFIRM;
            }
        }
    }
    return result;
}

bool input_any_open(void) {
    for (int i = 0; i < DEV_COUNT; i++) {
        if (fds[i] >= 0) return true;
    }
    return false;
}

bl_input_event_t input_poll(int timeout_ms) {
    bl_input_event_t none = { BL_INPUT_NONE, 0, 0 };

    struct pollfd pfds[DEV_COUNT];
    int nfds = 0;
    int slot_to_dev[DEV_COUNT];
    for (int i = 0; i < DEV_COUNT; i++) {
        if (fds[i] < 0) continue;
        pfds[nfds].fd = fds[i];
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        slot_to_dev[nfds] = i;
        nfds++;
    }
    if (nfds == 0) return none;

    int pr = poll(pfds, (nfds_t) nfds, timeout_ms);
    if (pr <= 0) return none;

    /* Drain every ready fd this tick (not just the first), same
     * "poll() once, service every device with data" pattern
     * src/hardware/hw_buttons.c uses -- otherwise an unrelated device's
     * events queue up while only one fd's events ever get read. Only the
     * first fd that actually produces a recognized event wins the tick;
     * the others are still drained so they don't back up. */
    bl_input_event_t winner = none;
    for (int i = 0; i < nfds; i++) {
        /* POLLHUP/POLLERR/POLLNVAL are reported in revents regardless of
         * what was requested in events -- a device that disconnects/
         * errors sets one of these without POLLIN, poll() still returns
         * >0 because of it, and it stays set forever on every future call
         * unless this fd is actually removed. Without this check, once
         * the menu cancels its countdown and starts calling
         * input_poll(-1) (block indefinitely -- see main.c's own run_menu()),
         * a single wedged fd would make every one of those calls return
         * immediately forever: a 100%-CPU busy loop with the menu
         * otherwise appearing frozen, not a timeout anyone would notice
         * as "waiting for input" the way a genuine indefinite block would. */
        if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            close(fds[slot_to_dev[i]]);
            fds[slot_to_dev[i]] = -1;
            continue;
        }
        if (!(pfds[i].revents & POLLIN)) continue;
        bl_input_event_t r = drain_fd(pfds[i].fd, slot_to_dev[i]);
        if (winner.type == BL_INPUT_NONE) winner = r;
    }
    return winner;
}
