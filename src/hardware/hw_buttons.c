#include "hw_buttons.h"
#include "debug_log.h"
#include "input_device_utils.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* One physical button press = one percentage point (0-100 range). */
#define VOLUME_STEP_PERCENT 1

/* Typematic repeat for volume keys: initial delay followed by periodic repeats. */
#define VOLUME_REPEAT_INITIAL_DELAY_MS 350
#define VOLUME_REPEAT_INTERVAL_MS 60

/* Threshold to trigger power long-press (power-off menu) instead of a short tap (screen toggle). */
#define POWER_LONG_PRESS_MS 700

/* Hold-to-seek threshold/repeat for the Next button, matching LVGL's own
 * default long-press timing (LV_INDEV_DEF_LONG_PRESS_TIME/_REP_TIME) so
 * holding the physical button feels the same as holding the touch one. */
#define NEXT_SEEK_LONG_PRESS_MS 400
#define NEXT_SEEK_REPEAT_MS 100

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Tracks press counts between GUI poll intervals to reliably detect multi-clicks. */
static int play_pause_press_count = 0;
static bool next_requested = false;
static bool prev_requested = false;
static bool power_requested = false;
static bool power_long_press_requested = false;
static int volume_delta = 0;

/* Held-state + next-repeat-due tracking for volume up/down specifically --
 * the only two keys that repeat while held. Everything else (play/pause,
 * next/prev) stays single-shot: repeating a track skip while a finger
 * lingers on the button would be actively wrong, not just unnecessary. */
static bool volume_up_held = false;
static bool volume_down_held = false;
static uint32_t volume_up_next_repeat_ms = 0;
static uint32_t volume_down_next_repeat_ms = 0;

/* Power held-state + long-press-due tracking, same shape as the volume
 * repeat state above -- power_long_press_fired guards against firing the
 * long-press flag more than once per physical press, and (in
 * handle_key_event()'s value==0 branch) against also firing the short-tap
 * flag once the same press has already turned into a long-press. */
static bool power_held = false;
static bool power_long_press_fired = false;
static uint32_t power_long_press_due_ms = 0;

/* Next-button hold-to-seek state, same shape as the power long-press state
 * above: next_seek_fired guards against firing the "first step" flag more
 * than once per hold, and against also firing the short-press flag once the
 * hold has taken over. next_seek_step_count accumulates on this thread at
 * NEXT_SEEK_REPEAT_MS granularity; the GUI thread drains it independently of
 * its own (coarser) poll interval, exactly like volume_delta above. */
static bool next_held = false;
static bool next_seek_fired = false;
static uint32_t next_seek_due_ms = 0;
static int next_seek_step_count = 0;
static bool next_seek_step_is_first = false;

static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* value: 1 = key down, 0 = key up (repeat events, value 2, are ignored --
 * this device's own repeat pacing below replaces whatever the kernel might
 * otherwise synthesize, and only actually fires for the two keys that
 * should repeat at all). */
static void handle_key_event(unsigned short code, int value) {
    pthread_mutex_lock(&state_mutex);
    if (value == 1) {
        switch (code) {
            case KEY_POWER: {
                uint32_t now = monotonic_ms();
                power_held = true;
                power_long_press_fired = false;
                power_long_press_due_ms = now + POWER_LONG_PRESS_MS;
                DBG_LOG("hw_buttons: KEY_POWER down at t=%u\n", now);
                break;
            }
            case KEY_PLAYPAUSE:      play_pause_press_count++; break;
            case KEY_NEXTSONG: {
                uint32_t now = monotonic_ms();
                next_held = true;
                next_seek_fired = false;
                next_seek_due_ms = now + NEXT_SEEK_LONG_PRESS_MS;
                break;
            }
            case KEY_PREVIOUSSONG:   prev_requested = true; break;
            case KEY_VOLUMEUP:
                volume_delta += VOLUME_STEP_PERCENT;
                volume_up_held = true;
                volume_up_next_repeat_ms = monotonic_ms() + VOLUME_REPEAT_INITIAL_DELAY_MS;
                break;
            case KEY_VOLUMEDOWN:
                volume_delta -= VOLUME_STEP_PERCENT;
                volume_down_held = true;
                volume_down_next_repeat_ms = monotonic_ms() + VOLUME_REPEAT_INITIAL_DELAY_MS;
                break;
            default: break;
        }
    } else if (value == 0) {
        switch (code) {
            case KEY_POWER:
                /* Only a short tap (released before the long-press
                 * threshold fired) sets the screen-toggle flag -- if
                 * power_long_press_fired is already true, that flag
                 * (hw_buttons_consume_power_long_press()) already told
                 * gui.c to show the power-off countdown for this same
                 * press, and the release shouldn't also toggle the screen
                 * out from under it. */
                if (power_held && !power_long_press_fired) power_requested = true;
                power_held = false;
                break;
            case KEY_VOLUMEUP:   volume_up_held = false; break;
            case KEY_VOLUMEDOWN: volume_down_held = false; break;
            case KEY_NEXTSONG:
                /* Same suppression as KEY_POWER above: only a release that
                 * never crossed the seek threshold counts as a short press. */
                if (next_held && !next_seek_fired) next_requested = true;
                next_held = false;
                break;
            default: break;
        }
    }
    pthread_mutex_unlock(&state_mutex);
}

/* Called on every poll() timeout while a volume key is held (see the main
 * loop below) -- applies a repeat step for whichever key(s) are due,
 * independently, so both keys held at once (unusual, but not prevented)
 * repeat on their own separate schedules rather than one blocking the
 * other. */
static void apply_due_volume_repeats(void) {
    uint32_t now = monotonic_ms();
    pthread_mutex_lock(&state_mutex);
    if (volume_up_held && (int32_t) (now - volume_up_next_repeat_ms) >= 0) {
        volume_delta += VOLUME_STEP_PERCENT;
        volume_up_next_repeat_ms = now + VOLUME_REPEAT_INTERVAL_MS;
    }
    if (volume_down_held && (int32_t) (now - volume_down_next_repeat_ms) >= 0) {
        volume_delta -= VOLUME_STEP_PERCENT;
        volume_down_next_repeat_ms = now + VOLUME_REPEAT_INTERVAL_MS;
    }
    pthread_mutex_unlock(&state_mutex);
}

/* Called on every poll() timeout while the power button is held (see the
 * main loop below) -- fires the long-press flag exactly once per press, the
 * moment the hold crosses POWER_LONG_PRESS_MS, independent of when (or
 * whether) the button is ever released. */
static void apply_due_power_long_press(void) {
    uint32_t now = monotonic_ms();
    pthread_mutex_lock(&state_mutex);
    if (power_held && !power_long_press_fired && (int32_t) (now - power_long_press_due_ms) >= 0) {
        power_long_press_fired = true;
        power_long_press_requested = true;
    }
    pthread_mutex_unlock(&state_mutex);
}

/* Called on every poll() timeout while the Next button is held (see the main
 * loop below) -- accumulates one seek step every NEXT_SEEK_REPEAT_MS once
 * the hold has passed NEXT_SEEK_LONG_PRESS_MS, marking the first such step
 * per hold via next_seek_step_is_first. */
static void apply_due_next_seek(void) {
    uint32_t now = monotonic_ms();
    pthread_mutex_lock(&state_mutex);
    if (next_held && (int32_t) (now - next_seek_due_ms) >= 0) {
        next_seek_step_count++;
        if (!next_seek_fired) {
            next_seek_fired = true;
            next_seek_step_is_first = true;
        }
        next_seek_due_ms = now + NEXT_SEEK_REPEAT_MS;
    }
    pthread_mutex_unlock(&state_mutex);
}

static void * hw_buttons_thread_func(void * arg) {
    (void) arg;

    static const struct { const char * name; const char * label; } BUTTON_DEVICES[] = {
        { "md-gpio-keys", "md-gpio-keys" },
        { "jz adc keyboard", "jz adc keyboard" },
        /* Wired headphone inline remote (earpods_adc). */
        { "earpods_adc", "earpods_adc" },
    };

    /* O_NONBLOCK prevents empty read queues on one device from blocking poll
     * and starving inputs from the other button devices. */
    struct pollfd fds[3];
    int nfds = 0;
    int found = 0;
    for (size_t i = 0; i < sizeof(BUTTON_DEVICES) / sizeof(BUTTON_DEVICES[0]); i++) {
        char path[64];
        if (!find_input_device_by_name(BUTTON_DEVICES[i].name, path, sizeof(path))) continue;
        found++;
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            fprintf(stderr, "hw_buttons: failed to open %s\n", path);
            continue;
        }
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        DBG_LOG("hw_buttons: fd_index=%d -> %s (%s)\n", nfds, path, BUTTON_DEVICES[i].label);
        nfds++;
    }

    if (found == 0) {
        fprintf(stderr, "hw_buttons: no physical button input devices found, hardware keys disabled\n");
        return NULL;
    }

    if (nfds == 0) return NULL;

    printf("hw_buttons: listening for physical button presses\n");

    while (1) {
        pthread_mutex_lock(&state_mutex);
        bool volume_held = volume_up_held || volume_down_held;
        bool power_pending_long_press = power_held && !power_long_press_fired;
        bool next_pending_seek = next_held;
        pthread_mutex_unlock(&state_mutex);

        /* Blocks indefinitely except while a volume key, power button, or
         * Next button is held and awaiting a timed repeat step or long-press
         * threshold. */
        int ret = poll(fds, (nfds_t) nfds,
                        (volume_held || power_pending_long_press || next_pending_seek) ? VOLUME_REPEAT_INTERVAL_MS : -1);
        if (ret == 0) {
            apply_due_volume_repeats();
            apply_due_power_long_press();
            apply_due_next_seek();
            continue;
        }
        if (ret < 0) continue;

        for (int i = 0; i < nfds; i++) {
            if (!(fds[i].revents & POLLIN)) continue;

            struct input_event ev;
            while (read(fds[i].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
                DBG_LOG("hw_buttons: raw event fd_index=%d type=%u code=%u value=%d\n", i, ev.type, ev.code, ev.value);
                if (ev.type == EV_KEY && ev.value != 2) { /* key down/up, ignore kernel autorepeat */
                    handle_key_event(ev.code, ev.value);
                }
            }
        }
    }

    return NULL;
}

void hw_buttons_init(void) {
    pthread_t thread;
    pthread_create(&thread, NULL, hw_buttons_thread_func, NULL);
    pthread_detach(thread);
}

bool hw_buttons_consume_power(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = power_requested;
    power_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool hw_buttons_consume_power_long_press(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = power_long_press_requested;
    power_long_press_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

int hw_buttons_consume_play_pause(void) {
    pthread_mutex_lock(&state_mutex);
    int result = play_pause_press_count;
    play_pause_press_count = 0;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool hw_buttons_consume_next(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = next_requested;
    next_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

bool hw_buttons_consume_prev(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = prev_requested;
    prev_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

int hw_buttons_consume_next_seek_steps(bool * out_is_first) {
    pthread_mutex_lock(&state_mutex);
    int result = next_seek_step_count;
    next_seek_step_count = 0;
    if (out_is_first) *out_is_first = next_seek_step_is_first;
    next_seek_step_is_first = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

int hw_buttons_consume_volume_delta(void) {
    pthread_mutex_lock(&state_mutex);
    int result = volume_delta;
    volume_delta = 0;
    pthread_mutex_unlock(&state_mutex);
    return result;
}
