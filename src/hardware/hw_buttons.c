#include "hw_buttons.h"
#include "debug_log.h"
#include "input_device_utils.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
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
static bool screenshot_requested = false;
/* Developer Options arms the chord; written from the UI thread, read here
 * under state_mutex like the rest of the key state. */
static bool screenshot_combo_enabled = false;
static int volume_delta = 0;

/* Held-state + next-repeat-due tracking for volume up/down specifically --
 * the only two keys that repeat while held. Everything else (play/pause,
 * next/prev) stays single-shot: repeating a track skip while a finger
 * lingers on the button would be actively wrong, not just unnecessary. */
static bool volume_up_held = false;
static bool volume_down_held = false;
static uint32_t volume_up_next_repeat_ms = 0;
static uint32_t volume_down_next_repeat_ms = 0;
/* A Volume Down press can become the first half of the reverse-order
 * screenshot chord. Its step is undoable only until the UI drains the shared
 * volume accumulator; its press time remains available while the key is held. */
static bool volume_down_initial_step_pending = false;
static uint32_t volume_down_pressed_ms = 0;
static bool volume_down_screenshot_chorded = false;

/* Power held-state + long-press-due tracking, same shape as the volume
 * repeat state above -- power_long_press_fired guards against firing the
 * long-press flag more than once per physical press, and (in
 * handle_key_event()'s value==0 branch) against also firing the short-tap
 * flag once the same press has already turned into a long-press. */
static bool power_held = false;
static bool power_long_press_fired = false;
static bool power_screenshot_chorded = false;
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
                bool reverse_chord = screenshot_combo_enabled && volume_down_held &&
                    (uint32_t) (now - volume_down_pressed_ms) <= 250;
                power_held = true;
                power_long_press_fired = false;
                power_screenshot_chorded = reverse_chord;
                power_long_press_due_ms = reverse_chord ? 0 : now + POWER_LONG_PRESS_MS;
                if (reverse_chord) {
                    screenshot_requested = true;
                    volume_down_screenshot_chorded = true;
                    volume_down_held = false;
                    if (volume_down_initial_step_pending) {
                        volume_delta += VOLUME_STEP_PERCENT;
                        volume_down_initial_step_pending = false;
                    }
                    DBG_LOG("hw_buttons: screenshot chord VolDown -> Power at t=%u\n", now);
                } else {
                    DBG_LOG("hw_buttons: KEY_POWER down at t=%u\n", now);
                }
                break;
            }
            /* Accessory remotes do not all speak the same code for the same
             * button: HID consumer-page transport controls arrive as any of
             * these depending on the cable's descriptor. */
            case KEY_PLAYPAUSE:
            case KEY_PLAY:
            case KEY_PLAYCD:
            case KEY_PAUSECD:        play_pause_press_count++; break;
            case KEY_FASTFORWARD:
            case KEY_NEXTSONG: {
                uint32_t now = monotonic_ms();
                next_held = true;
                next_seek_fired = false;
                next_seek_due_ms = now + NEXT_SEEK_LONG_PRESS_MS;
                break;
            }
            case KEY_REWIND:
            case KEY_PREVIOUSSONG:   prev_requested = true; break;
            case KEY_VOLUMEUP:
                volume_delta += VOLUME_STEP_PERCENT;
                volume_up_held = true;
                volume_up_next_repeat_ms = monotonic_ms() + VOLUME_REPEAT_INITIAL_DELAY_MS;
                break;
            case KEY_VOLUMEDOWN: {
                uint32_t now = monotonic_ms();
                if (screenshot_combo_enabled && power_held && !power_long_press_fired) {
                    if (!power_screenshot_chorded) {
                        screenshot_requested = true;
                        power_screenshot_chorded = true;
                        DBG_LOG("hw_buttons: screenshot chord Power -> VolDown at t=%u\n", now);
                    }
                    volume_down_screenshot_chorded = true;
                    volume_down_initial_step_pending = false;
                    volume_down_held = false;
                } else {
                    volume_delta -= VOLUME_STEP_PERCENT;
                    volume_down_held = true;
                    volume_down_pressed_ms = now;
                    volume_down_initial_step_pending = true;
                    volume_down_screenshot_chorded = false;
                    volume_down_next_repeat_ms = now + VOLUME_REPEAT_INITIAL_DELAY_MS;
                }
                break;
            }
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
                if (power_held && !power_long_press_fired && !power_screenshot_chorded) power_requested = true;
                power_held = false;
                power_screenshot_chorded = false;
                break;
            case KEY_VOLUMEUP:   volume_up_held = false; break;
            case KEY_VOLUMEDOWN:
                volume_down_held = false;
                volume_down_initial_step_pending = false;
                volume_down_screenshot_chorded = false;
                break;
            case KEY_FASTFORWARD:
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
    if (volume_down_held && !volume_down_screenshot_chorded &&
        (int32_t) (now - volume_down_next_repeat_ms) >= 0) {
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
    if (power_held && !power_long_press_fired && !power_screenshot_chorded &&
        (int32_t) (now - power_long_press_due_ms) >= 0) {
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

/* Media keys are what an accessory remote sends: a USB DSP cable or dongle
 * carries its own transport buttons, and its device name is the vendor's, so
 * these are found by what they can send rather than by what they are called. */
static bool device_sends_media_keys(int fd) {
    unsigned long bits[KEY_MAX / (8 * sizeof(unsigned long)) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return false;
#define HW_BUTTONS_HAS_KEY(code) \
    ((bits[(code) / (8 * sizeof(unsigned long))] >> ((code) % (8 * sizeof(unsigned long)))) & 1UL)
    return HW_BUTTONS_HAS_KEY(KEY_PLAYPAUSE) || HW_BUTTONS_HAS_KEY(KEY_NEXTSONG) ||
           HW_BUTTONS_HAS_KEY(KEY_PREVIOUSSONG) || HW_BUTTONS_HAS_KEY(KEY_PLAY) ||
           HW_BUTTONS_HAS_KEY(KEY_PLAYCD) || HW_BUTTONS_HAS_KEY(KEY_PAUSECD) ||
           HW_BUTTONS_HAS_KEY(KEY_FASTFORWARD) || HW_BUTTONS_HAS_KEY(KEY_REWIND);
#undef HW_BUTTONS_HAS_KEY
}

#define HW_BUTTONS_MAX_FDS 12
/* Cheap: a readdir plus one ioctl per unopened event node, and only when no
 * key was pressed in that window. */
#define MEDIA_KEY_RESCAN_INTERVAL_MS 2000

/* Adds any event device with transport keys that is not already open. Called
 * again on a cadence because an accessory can be plugged in long after boot,
 * which the original one-shot enumeration could never see. */
static int scan_media_key_devices(struct pollfd * fds, int nfds, char paths[][64]) {
    DIR * dir = opendir("/dev/input");
    if (!dir) return nfds;

    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL && nfds < HW_BUTTONS_MAX_FDS) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        bool already_open = false;
        for (int i = 0; i < nfds; i++)
            if (strcmp(paths[i], path) == 0) { already_open = true; break; }
        if (already_open) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (!device_sends_media_keys(fd)) {
            close(fd);
            continue;
        }
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        snprintf(paths[nfds], 64, "%s", path);
        DBG_LOG("hw_buttons: media-key device %s opened at fd_index=%d\n", path, nfds);
        nfds++;
    }
    closedir(dir);
    return nfds;
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
    struct pollfd fds[HW_BUTTONS_MAX_FDS];
    char paths[HW_BUTTONS_MAX_FDS][64];
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
        snprintf(paths[nfds], sizeof(paths[nfds]), "%s", path);
        DBG_LOG("hw_buttons: fd_index=%d -> %s (%s)\n", nfds, path, BUTTON_DEVICES[i].label);
        nfds++;
    }

    /* The built-in keys are named and known; an accessory's remote is not, so
     * it is picked up by capability here and on every rescan below. */
    nfds = scan_media_key_devices(fds, nfds, paths);

    if (found == 0 && nfds == 0) {
        fprintf(stderr, "hw_buttons: no physical button input devices found, hardware keys disabled\n");
        return NULL;
    }

    if (nfds == 0) return NULL;

    printf("hw_buttons: listening for physical button presses\n");

    while (1) {
        pthread_mutex_lock(&state_mutex);
        bool volume_held = volume_up_held || volume_down_held;
        bool power_pending_long_press = power_held && !power_long_press_fired && !power_screenshot_chorded;
        bool next_pending_seek = next_held;
        pthread_mutex_unlock(&state_mutex);

        /* Blocks indefinitely except while a volume key, power button, or
         * Next button is held and awaiting a timed repeat step or long-press
         * threshold. */
        /* Never blocks indefinitely any more: an accessory plugged in after
         * boot only becomes visible on a rescan, and the old -1 meant the
         * thread sat in poll() forever waiting on keys that did not exist
         * yet. The wait still shortens while a key is held for its repeat. */
        int ret = poll(fds, (nfds_t) nfds,
                        (volume_held || power_pending_long_press || next_pending_seek)
                            ? VOLUME_REPEAT_INTERVAL_MS : MEDIA_KEY_RESCAN_INTERVAL_MS);
        if (ret == 0) {
            apply_due_volume_repeats();
            apply_due_power_long_press();
            apply_due_next_seek();
            nfds = scan_media_key_devices(fds, nfds, paths);
            continue;
        }
        if (ret < 0) continue;

        for (int i = 0; i < nfds; i++) {
            /* An unplugged accessory reports an error rather than data. Its
             * slot is closed and the last one moved down, so a cable can come
             * and go without leaking descriptors or wedging the poll set. */
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                DBG_LOG("hw_buttons: %s went away, closing fd_index=%d\n", paths[i], i);
                close(fds[i].fd);
                nfds--;
                if (i != nfds) {
                    fds[i] = fds[nfds];
                    memcpy(paths[i], paths[nfds], sizeof(paths[i]));
                }
                i--;
                continue;
            }
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

void hw_buttons_set_screenshot_combo_enabled(bool enabled) {
    pthread_mutex_lock(&state_mutex);
    screenshot_combo_enabled = enabled;
    pthread_mutex_unlock(&state_mutex);
}

bool hw_buttons_consume_screenshot(void) {
    pthread_mutex_lock(&state_mutex);
    bool result = screenshot_requested;
    screenshot_requested = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

int hw_buttons_consume_volume_delta(void) {
    pthread_mutex_lock(&state_mutex);
    int result = volume_delta;
    volume_delta = 0;
    volume_down_initial_step_pending = false;
    pthread_mutex_unlock(&state_mutex);
    return result;
}
