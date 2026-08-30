#include "app_clock.h"

#include <string.h>

static bool clock_automatic = true;
static time_t manual_base;
static struct timespec manual_anchor;

static void monotonic_now(struct timespec * ts) {
#ifdef CLOCK_BOOTTIME
    if (clock_gettime(CLOCK_BOOTTIME, ts) == 0) return;
#endif
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static int64_t elapsed_seconds(const struct timespec * from, const struct timespec * to) {
    int64_t seconds = (int64_t) to->tv_sec - (int64_t) from->tv_sec;
    if (to->tv_nsec < from->tv_nsec) seconds--;
    return seconds > 0 ? seconds : 0;
}

void app_clock_init(bool automatic, int64_t manual_epoch, int64_t system_reference) {
    time_t system_now = time(NULL);
    clock_automatic = automatic;
    if (manual_epoch <= 0) manual_epoch = (int64_t) system_now;
    int64_t delta = (int64_t) system_now - system_reference;
    /* A backward RTC correction or an implausible gap is not elapsed time.
     * Ten years still accommodates a device left unused for a long time. */
    if (system_reference > 0 && delta >= 0 && delta <= 10LL * 366 * 24 * 60 * 60)
        manual_epoch += delta;
    manual_base = (time_t) manual_epoch;
    monotonic_now(&manual_anchor);
}

static time_t manual_now(void) {
    struct timespec now;
    monotonic_now(&now);
    return manual_base + (time_t) elapsed_seconds(&manual_anchor, &now);
}

time_t app_clock_now(void) {
    return clock_automatic ? time(NULL) : manual_now();
}

void app_clock_localtime(struct tm * out) {
    time_t now = app_clock_now();
    if (clock_automatic) localtime_r(&now, out);
    else gmtime_r(&now, out);
}

bool app_clock_is_automatic(void) { return clock_automatic; }

void app_clock_set_automatic(bool automatic) {
    if (automatic == clock_automatic) return;
    if (!automatic) {
        time_t now = time(NULL);
        struct tm civil;
        localtime_r(&now, &civil);
        manual_base = timegm(&civil);
        monotonic_now(&manual_anchor);
    }
    clock_automatic = automatic;
}

void app_clock_set_local_time(int hour, int minute) {
    struct tm local;
    if (clock_automatic) {
        time_t current = time(NULL);
        localtime_r(&current, &local);
    } else {
        time_t current = manual_now();
        gmtime_r(&current, &local);
    }
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    local.tm_isdst = 0;
    manual_base = timegm(&local);
    monotonic_now(&manual_anchor);
    clock_automatic = false;
}

void app_clock_get_persistence(int64_t * manual_epoch, int64_t * system_reference) {
    if (manual_epoch) *manual_epoch = (int64_t) manual_now();
    if (system_reference) *system_reference = (int64_t) time(NULL);
}
