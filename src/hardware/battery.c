#include "battery.h"
#include "board_config.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POWER_SUPPLY_DIR "/sys/class/power_supply"

/* On the R3Pro II, the "battery" power-supply node's own "status" attribute
 * is stuck reporting "Discharging" even while actually charging (its
 * "capacity" attribute is unaffected and stays accurate). The MP2731
 * charger IC exposes its own power-supply node with a correct "status"
 * attribute, so that node is used for status specifically on this board --
 * see charge_limiter.c's own BOARD_R3PROII gating for the same charger. */
#if defined(BOARD_R3PROII)
#define MP2731_CHARGER_DEVICE "mp2731-charger"
#endif

/* Reads a single-line sysfs attribute (e.g. ".../battery/capacity") into
 * `out`, trimming the trailing newline. Returns false if the file doesn't
 * exist or is empty -- normal on host, where none of this exists at all. */
static bool read_sysfs_attr(const char * device_name, const char * attr, char * out, size_t out_size) {
    /* dirent names can legally be up to 255 bytes; leave enough room for
     * the fixed power-supply prefix and attribute instead of silently
     * truncating a path into a different/nonexistent node. */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", POWER_SUPPLY_DIR, device_name, attr);

    FILE * f = fopen(path, "r");
    if (!f) return false;

    bool ok = fgets(out, (int) out_size, f) != NULL;
    fclose(f);
    if (!ok) return false;

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return len > 0;
}

/* Same dynamic scan the stock hiby_player itself uses (confirmed via
 * strings on the real binary: it enumerates every entry under
 * /sys/class/power_supply, reads each one's "type", and treats whichever
 * one reports "Battery" as
 * the battery -- rather than hardcoding a driver-specific directory name
 * like "battery" or "axp2101-battery", which could change across firmware
 * updates if the fuel-gauge driver is ever swapped).
 *
 * Prefers an entry named "battery" (the dedicated fuel gauge driver) over "axp_battery"
 * (raw PMIC node), falling back to any Battery-typed entry if not found. */
static pthread_mutex_t battery_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static char cached_battery_device[64];
static int cached_capacity = -1;
static char cached_status[24];
static struct timespec cached_at;
static bool cache_valid = false;

#define BATTERY_CACHE_TTL_MS 5000L
#define BATTERY_DISPLAY_STABLE_MS 15000L
#define BATTERY_DISPLAY_STEP_MS 60000L

/* A gap this large or bigger, once stable, is treated as a real change
 * rather than gauge jitter -- see its own use below. */
#define BATTERY_DISPLAY_JUMP_THRESHOLD 5

/* gui.c only calls battery_get_display_percent() while the screen is on
 * (every ~2s -- see refresh_battery_topbar()'s own caller), so a gap this
 * much larger than that between two calls can only mean the screen/app was
 * asleep in between, not a slow tick. */
#define BATTERY_DISPLAY_RESYNC_GAP_MS 60000L

static int display_capacity = -1;
static int display_candidate = -1;
static bool display_powered = false;
static struct timespec display_candidate_since;
static struct timespec display_last_step;
static struct timespec display_last_poll;
static bool display_last_poll_valid = false;

static long elapsed_ms(struct timespec now, struct timespec then) {
    return (now.tv_sec - then.tv_sec) * 1000L + (now.tv_nsec - then.tv_nsec) / 1000000L;
}

/* CLOCK_MONOTONIC does not advance across Linux suspend-to-RAM (confirmed
 * kernel/POSIX behavior, not device-specific); CLOCK_BOOTTIME is identical
 * except it DOES. battery_get_display_percent()'s own long_gap detection
 * (below) exists specifically to notice "the screen/app was asleep for a
 * while" and trust the fresh reading immediately instead of applying its
 * usual jitter-smoothing -- using CLOCK_MONOTONIC there would silently
 * defeat that: elapsed_ms() across an 8-hour real suspend would report only
 * the handful of milliseconds the CPU was actually awake around the call,
 * never crossing BATTERY_DISPLAY_RESYNC_GAP_MS. Present in the Linux kernel
 * since 2.6.39 (2011) -- expected on any kernel this device could plausibly
 * run -- but probed once and cached rather than assumed, falling back to
 * CLOCK_MONOTONIC (the previous behavior) if it's ever unavailable, so a
 * call site here can't hard-fail either way. */
static clockid_t battery_clock_id(void) {
    static clockid_t cached = CLOCK_MONOTONIC;
    static bool probed = false;
    if (!probed) {
        struct timespec ts;
        cached = (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) ? CLOCK_BOOTTIME : CLOCK_MONOTONIC;
        probed = true;
    }
    return cached;
}

static bool discover_battery_device(char * out, size_t out_size) {
    DIR * dir = opendir(POWER_SUPPLY_DIR);
    if (!dir) return false;

    char fallback[64] = "";
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char type[32];
        if (!read_sysfs_attr(entry->d_name, "type", type, sizeof(type))) continue;
        if (strcmp(type, "Battery") != 0) continue;
        if (strcmp(entry->d_name, "battery") == 0) {
            snprintf(out, out_size, "%s", entry->d_name);
            closedir(dir);
            return true;
        }
        if (!fallback[0]) snprintf(fallback, sizeof(fallback), "%s", entry->d_name);
    }
    closedir(dir);
    if (!fallback[0]) return false;
    snprintf(out, out_size, "%s", fallback);
    return true;
}

battery_external_power_state_t battery_get_external_power_state(void) {
#ifdef HOST_BUILD
    return BATTERY_EXTERNAL_POWER_UNKNOWN;
#else
    DIR * dir = opendir(POWER_SUPPLY_DIR);
    if (!dir) return BATTERY_EXTERNAL_POWER_UNKNOWN;

    bool saw_offline = false;
    bool saw_unreadable = false;
    struct dirent * entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char type[32];
        if (!read_sysfs_attr(entry->d_name, "type", type, sizeof(type))) {
            saw_unreadable = true;
            continue;
        }
        if (strcmp(type, "Battery") == 0) continue;

        char online[8];
        if (!read_sysfs_attr(entry->d_name, "online", online, sizeof(online))) {
            /* A non-battery supply whose state cannot be sampled makes an
             * otherwise-all-off result ambiguous.  A positive sample from
             * any other supply below still proves power is connected. */
            saw_unreadable = true;
            continue;
        }
        if (strcmp(online, "1") == 0) {
            closedir(dir);
            return BATTERY_EXTERNAL_POWER_CONNECTED;
        }
        if (strcmp(online, "0") == 0) saw_offline = true;
        else saw_unreadable = true;
    }
    if (errno != 0) saw_unreadable = true;
    closedir(dir);

    if (saw_offline && !saw_unreadable) return BATTERY_EXTERNAL_POWER_DISCONNECTED;
    return BATTERY_EXTERNAL_POWER_UNKNOWN;
#endif
}

static bool refresh_battery_cache_locked(void) {
    struct timespec now;
    clock_gettime(battery_clock_id(), &now);
    long age_ms = (now.tv_sec - cached_at.tv_sec) * 1000L + (now.tv_nsec - cached_at.tv_nsec) / 1000000L;
    if (cache_valid && age_ms >= 0 && age_ms < BATTERY_CACHE_TTL_MS) return true;

    if (!cached_battery_device[0] &&
        !discover_battery_device(cached_battery_device, sizeof(cached_battery_device))) return false;

    char capacity_str[16];
    char status[24];
#if defined(MP2731_CHARGER_DEVICE)
    bool have_status = read_sysfs_attr(MP2731_CHARGER_DEVICE, "status", status, sizeof(status));
#else
    bool have_status = read_sysfs_attr(cached_battery_device, "status", status, sizeof(status));
#endif
    if (!read_sysfs_attr(cached_battery_device, "capacity", capacity_str, sizeof(capacity_str)) ||
        !have_status) {
        /* A driver can disappear/reappear across suspend. Rediscover once
         * on the next call instead of pinning a stale sysfs name forever. */
        cached_battery_device[0] = '\0';
        cache_valid = false;
        return false;
    }
    cached_capacity = atoi(capacity_str);
    snprintf(cached_status, sizeof(cached_status), "%s", status);
    cached_at = now;
    cache_valid = true;
    return true;
}

int battery_get_percent(void) {
    pthread_mutex_lock(&battery_cache_mutex);
    int result = refresh_battery_cache_locked() ? cached_capacity : -1;
    pthread_mutex_unlock(&battery_cache_mutex);
    return result;
}

int battery_get_display_percent(void) {
    pthread_mutex_lock(&battery_cache_mutex);
    if (!refresh_battery_cache_locked()) {
        pthread_mutex_unlock(&battery_cache_mutex);
        return -1;
    }

    int raw = cached_capacity;
    if (raw < 0) raw = 0;
    if (raw > 100) raw = 100;
    bool powered = strcmp(cached_status, "Charging") == 0 || strcmp(cached_status, "Full") == 0;
    struct timespec now;
    clock_gettime(battery_clock_id(), &now);

    /* If there is a large gap since the last poll (e.g. waking from suspend),
     * accept the raw value immediately without smoothing lag. */
    bool long_gap = display_last_poll_valid && elapsed_ms(now, display_last_poll) >= BATTERY_DISPLAY_RESYNC_GAP_MS;
    display_last_poll = now;
    display_last_poll_valid = true;

    if (display_capacity < 0 || long_gap) {
        display_capacity = raw;
        display_candidate = raw;
        display_powered = powered;
        display_candidate_since = now;
        display_last_step = now;
    } else {
        if (powered != display_powered) {
            /* Do not accept the voltage-relaxation jump commonly emitted
             * immediately after plugging/unplugging. Start a fresh stable
             * observation window in the new physical direction. */
            display_powered = powered;
            display_candidate = raw;
            display_candidate_since = now;
            display_last_step = now;
        } else if (raw != display_candidate) {
            display_candidate = raw;
            display_candidate_since = now;
        }

        if (!powered && raw <= 5) {
            /* Never hide a genuinely critical reading behind smoothing. */
            display_capacity = raw;
            display_last_step = now;
        } else if (strcmp(cached_status, "Full") == 0 && raw >= 99) {
            display_capacity = 100;
            display_last_step = now;
        } else if (elapsed_ms(now, display_candidate_since) >= BATTERY_DISPLAY_STABLE_MS) {
            /* Opposite-direction candidates are deliberately ignored below:
             * battery state cannot physically rise while discharging or
             * fall while charging, and those reversals are the reported
             * gauge noise. */
            bool valid_direction = (powered && display_candidate > display_capacity) ||
                                   (!powered && display_candidate < display_capacity);
            int gap = powered ? display_candidate - display_capacity : display_capacity - display_candidate;

            if (valid_direction && gap >= BATTERY_DISPLAY_JUMP_THRESHOLD) {
                /* If a large difference is sustained across the stability window,
                 * snap directly to the candidate value rather than stepping slowly. */
                display_capacity = raw;
                display_last_step = now;
            } else if (valid_direction && elapsed_ms(now, display_last_step) >= BATTERY_DISPLAY_STEP_MS) {
                if (powered) display_capacity++;
                else display_capacity--;
                display_last_step = now;
            }
        }
    }

    int result = display_capacity;
    pthread_mutex_unlock(&battery_cache_mutex);
    return result;
}

static bool read_best_battery_status(char * out_status, size_t out_size) {
    pthread_mutex_lock(&battery_cache_mutex);
    bool ok = refresh_battery_cache_locked();
    if (ok) snprintf(out_status, out_size, "%s", cached_status);
    pthread_mutex_unlock(&battery_cache_mutex);
    return ok;
}

bool battery_is_charging(void) {
    char status[24];
    if (!read_best_battery_status(status, sizeof(status))) return false;
    return strcmp(status, "Charging") == 0 || strcmp(status, "Full") == 0;
}

/* Distinct from battery_is_charging() (which also counts "Full" as
 * charging, since external power is still present either way) -- this is
 * specifically for callers that need to tell "actively charging, not
 * topped up yet" apart from "charging complete", like led_control.c's
 * red-while-charging/blue-when-full split. */
bool battery_is_full(void) {
    char status[24];
    if (!read_best_battery_status(status, sizeof(status))) return false;
    return strcmp(status, "Full") == 0;
}
