#include "input.h"
#include "input_device_utils.h"
#include "fb_draw.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/* Monitored evdev input devices: touchscreen ("hyn_ts"), GPIO keys ("md-gpio-keys"),
 * and ADC keyboard ("jz adc keyboard"). */
#define DEV_TOUCH 0
#define DEV_GPIO_KEYS 1
#define DEV_ADC_KEYBOARD 2
#define DEV_COUNT 3

static int fds[DEV_COUNT] = { -1, -1, -1 };

/* Tracks touch position and down/up state. Coordinates map directly to
 * screen pixels. */
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

/* Drains available events from fd, updating touch state or mapping button presses
 * to navigation/confirm events. Non-blocking to prevent starving poll(). */
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

            /* Either tracking ID or button touch signals can trigger press/release. */
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

    /* Drain all ready file descriptors, returning the first recognized input event. */
    bl_input_event_t winner = none;
    for (int i = 0; i < nfds; i++) {
        /* Close file descriptors reporting errors/hangup to avoid busy loops. */
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
