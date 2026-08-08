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

/* One physical button press = one percentage point, giving the full 100
 * discrete steps the slider/readout range (0-100) supports, rather than
 * coarsely jumping 5 at a time (real-device feedback: "should be 100 in
 * total, not jump from 5 to 5"). */
#define VOLUME_STEP_PERCENT 1

/* Long-press auto-repeat, typematic-style: the first repeat waits longer
 * than the ones after it, so a quick tap never accidentally free-runs and a
 * deliberate hold ramps up at a steady, predictable rate. 60ms (~16
 * steps/sec, full 0-100 sweep in ~6s) -- real-device feedback on an
 * earlier, slower 120ms interval was "works correctly, but a bit slow". */
#define VOLUME_REPEAT_INITIAL_DELAY_MS 350
#define VOLUME_REPEAT_INTERVAL_MS 60

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool play_pause_requested = false;
static bool next_requested = false;
static bool prev_requested = false;
static bool power_requested = false;
static int volume_delta = 0;

/* Held-state + next-repeat-due tracking for volume up/down specifically --
 * the only two keys that repeat while held. Everything else (power,
 * play/pause, next/prev) stays single-shot: repeating a track skip or a
 * power toggle while a finger lingers on the button would be actively
 * wrong, not just unnecessary. */
static bool volume_up_held = false;
static bool volume_down_held = false;
static uint32_t volume_up_next_repeat_ms = 0;
static uint32_t volume_down_next_repeat_ms = 0;

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
            case KEY_POWER:
                power_requested = true;
                DBG_LOG("hw_buttons: KEY_POWER down at t=%u\n", monotonic_ms());
                break;
            case KEY_PLAYPAUSE:      play_pause_requested = true; break;
            case KEY_NEXTSONG:       next_requested = true; break;
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
            case KEY_VOLUMEUP:   volume_up_held = false; break;
            case KEY_VOLUMEDOWN: volume_down_held = false; break;
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

static void * hw_buttons_thread_func(void * arg) {
    (void) arg;

    char gpio_keys_path[64];
    char adc_keyboard_path[64];
    bool have_gpio_keys = find_input_device_by_name("md-gpio-keys", gpio_keys_path, sizeof(gpio_keys_path));
    bool have_adc_keyboard = find_input_device_by_name("jz adc keyboard", adc_keyboard_path, sizeof(adc_keyboard_path));

    if (!have_gpio_keys && !have_adc_keyboard) {
        fprintf(stderr, "hw_buttons: no physical button input devices found, hardware keys disabled\n");
        return NULL;
    }

    /* O_NONBLOCK matters: the read loop below drains each ready fd with
     * `while (read(...) == sizeof(ev))`, and once one device's queue empties
     * a blocking read() on it just sits there instead of returning -- which
     * starves poll() from ever being called again, so the *other* device's
     * events queue up and never get read. Confirmed on real hardware: Power
     * and Next/Prev live on md-gpio-keys, Volume and Play/Pause live on "jz
     * adc keyboard", and without this, the very first gpio-keys press parked
     * the thread in a blocking read on that fd forever, silently starving
     * every subsequent volume/play-pause press on the other device. */
    struct pollfd fds[2];
    int nfds = 0;
    if (have_gpio_keys) {
        fds[nfds].fd = open(gpio_keys_path, O_RDONLY | O_NONBLOCK);
        if (fds[nfds].fd >= 0) {
            fds[nfds].events = POLLIN;
            nfds++;
        } else {
            fprintf(stderr, "hw_buttons: failed to open %s\n", gpio_keys_path);
        }
    }
    if (have_adc_keyboard) {
        fds[nfds].fd = open(adc_keyboard_path, O_RDONLY | O_NONBLOCK);
        if (fds[nfds].fd >= 0) {
            fds[nfds].events = POLLIN;
            nfds++;
        } else {
            fprintf(stderr, "hw_buttons: failed to open %s\n", adc_keyboard_path);
        }
    }

    if (nfds == 0) return NULL;

    printf("hw_buttons: listening for physical button presses\n");

    while (1) {
        pthread_mutex_lock(&state_mutex);
        bool volume_held = volume_up_held || volume_down_held;
        pthread_mutex_unlock(&state_mutex);

        /* Blocks indefinitely (no wasted wakeups) except while a volume key
         * is actually held, when a short timeout is the only way to notice
         * "still held, no new event" and fire the next repeat step -- see
         * apply_due_volume_repeats(). Reverts to -1 the moment both volume
         * keys are released, same idle efficiency as before this feature
         * existed. */
        int ret = poll(fds, (nfds_t) nfds, volume_held ? VOLUME_REPEAT_INTERVAL_MS : -1);
        if (ret == 0) {
            apply_due_volume_repeats();
            continue;
        }
        if (ret < 0) continue;

        for (int i = 0; i < nfds; i++) {
            if (!(fds[i].revents & POLLIN)) continue;

            struct input_event ev;
            while (read(fds[i].fd, &ev, sizeof(ev)) == (ssize_t) sizeof(ev)) {
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

bool hw_buttons_consume_play_pause(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = play_pause_requested;
    play_pause_requested = false;
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

int hw_buttons_consume_volume_delta(void) {
    pthread_mutex_lock(&state_mutex);
    int result = volume_delta;
    volume_delta = 0;
    pthread_mutex_unlock(&state_mutex);
    return result;
}
