#include "backlight.h"

#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define BACKLIGHT_CLASS_DIR "/sys/class/backlight"

/* Doesn't hardcode "backlight_pwm0" -- same reasoning as battery.c's dynamic
 * power_supply scan, in case a different unit/firmware names its backlight
 * device differently. Unlike power_supply, everything under this class
 * genuinely is a backlight, so no type filtering is needed -- first entry
 * found wins. Returns false (leaving `out` untouched) if the class has no
 * entries at all, normal on host. */
static bool find_backlight_device(char * out, size_t out_size) {
    static char cached_device[64];
    if (cached_device[0]) {
        snprintf(out, out_size, "%s", cached_device);
        return true;
    }
    DIR * dir = opendir(BACKLIGHT_CLASS_DIR);
    if (!dir) return false;

    bool found = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        snprintf(out, out_size, "%s", entry->d_name);
        snprintf(cached_device, sizeof(cached_device), "%s", entry->d_name);
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

static int read_int_attr(const char * device_name, const char * attr) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/%s", BACKLIGHT_CLASS_DIR, device_name, attr);

    FILE * f = fopen(path, "r");
    if (!f) return -1;
    int value;
    bool ok = fscanf(f, "%d", &value) == 1;
    fclose(f);
    return ok ? value : -1;
}

/* Real-device bug report: writing the literal max_brightness raw value (101
 * on a real R1 -- not a round number) turned the screen fully invisible
 * instead of "as bright as possible" -- confirmed live by writing 101
 * directly to brightness over adb with the app not even involved, and by
 * the user finding any slider position below 100% turned the screen back
 * on. A PWM backlight hitting its exact max duty cycle glitching dark like
 * this is a known class of embedded panel/PWM quirk, not something fixable
 * from here at the value level -- so the fix is to simply never write that
 * exact raw value. Every public function in this file works in "logical"
 * percent instead (a clean 0-100 the UI can show directly, "0%" and "100%"
 * included), mapped directly to/from the safe raw range
 * [BACKLIGHT_MIN_PERCENT, BACKLIGHT_SAFE_MAX_PERCENT] of max_brightness --
 * never the literal top or an unreadably-dim bottom -- so callers never
 * need to think about either edge themselves.
 *
 * Real-device bug report #2: an earlier version of this converted through
 * an intermediate "safe percent" (logical -> safe percent -> raw for
 * writes, raw -> safe percent -> logical for reads) -- two independent
 * floor-divisions each way, which don't necessarily round-trip back to the
 * same value with this device's own non-round max_brightness (101):
 * logical 100 -> safe percent 99 -> raw 99, but raw 99 read back -> safe
 * percent 98, one short of the 99 the read side's own "snap to 100" check
 * expected -- confirmed live as the slider topping out at 98%/99% no
 * matter how far right it was dragged, once the drawer was closed and
 * reopened. Converting directly between logical and raw in one step below,
 * using the exact same min/max raw bounds on both the write and read side,
 * keeps them consistent regardless of how max_brightness happens to
 * divide. */
#define BACKLIGHT_SAFE_MAX_PERCENT 99

static int logical_to_raw(int logical, int max) {
    if (logical < 0) logical = 0;
    if (logical > 100) logical = 100;
    int min_raw = (int) (((long) BACKLIGHT_MIN_PERCENT * max) / 100);
    int max_raw = (int) (((long) BACKLIGHT_SAFE_MAX_PERCENT * max) / 100);
    return min_raw + (int) (((long) (max_raw - min_raw) * logical) / 100);
}

static int raw_to_logical(int raw, int max) {
    int min_raw = (int) (((long) BACKLIGHT_MIN_PERCENT * max) / 100);
    int max_raw = (int) (((long) BACKLIGHT_SAFE_MAX_PERCENT * max) / 100);
    if (raw <= min_raw) return 0;
    if (raw >= max_raw) return 100;
    return (int) (((long) (raw - min_raw) * 100) / (max_raw - min_raw));
}

int backlight_get_percent(void) {
    char device_name[64];
    if (!find_backlight_device(device_name, sizeof(device_name))) return -1;

    int max = read_int_attr(device_name, "max_brightness");
    int cur = read_int_attr(device_name, "brightness");
    if (max <= 0 || cur < 0) return -1;
    return raw_to_logical(cur, max);
}

void backlight_set_percent(int percent) {
    char device_name[64];
    if (!find_backlight_device(device_name, sizeof(device_name))) return;

    int max = read_int_attr(device_name, "max_brightness");
    if (max <= 0) return;
    int value = logical_to_raw(percent, max);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s/brightness", BACKLIGHT_CLASS_DIR, device_name);
    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d", value);
    fclose(f);
}

/* Guards screen_on/restore_percent below -- accessed from both hw_buttons.c's
 * dedicated button-reading thread (power button) and the main/GUI thread
 * (auto screen-timeout, see gui.c's update_timer_cb), unlike every other
 * function in this file which only the GUI thread ever calls. */
static pthread_mutex_t screen_power_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool screen_on = true;
static bool screen_dimmed = false;
/* Restored on the next turn-on -- seeded to a sensible default matching the
 * old hw_buttons.c hardcoded BACKLIGHT_ON_LEVEL, only actually used if we
 * never captured a real prior brightness (e.g. backlight_get_percent()
 * failed the first time we turned the screen off). Logical percent, same
 * scale as backlight_get_percent()'s own return value. */
static int restore_percent = 80;

void backlight_set_normal_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    pthread_mutex_lock(&screen_power_mutex);
    restore_percent = percent;
    screen_dimmed = false;
    bool write_now = screen_on;
    pthread_mutex_unlock(&screen_power_mutex);
    if (write_now) backlight_set_percent(percent);
}

void backlight_set_dimmed(bool dimmed) {
    pthread_mutex_lock(&screen_power_mutex);
    if (!screen_on || screen_dimmed == dimmed) {
        pthread_mutex_unlock(&screen_power_mutex);
        return;
    }
    screen_dimmed = dimmed;
    int normal = restore_percent;
    pthread_mutex_unlock(&screen_power_mutex);

    /* Use an absolute ~5% ceiling rather than a fraction of the selected
     * brightness. Forty percent of a high normal setting was still far too
     * bright for the intended low-power/read-asleep state. Never brighten a
     * user-selected level that is already below the dim target. */
    int target = dimmed ? (normal < 5 ? normal : 5) : normal;
    backlight_set_percent(target);
}

bool backlight_screen_is_on(void) {
    pthread_mutex_lock(&screen_power_mutex);
    bool result = screen_on;
    pthread_mutex_unlock(&screen_power_mutex);
    return result;
}

void backlight_set_screen_on(bool on) {
    pthread_mutex_lock(&screen_power_mutex);
    if (on == screen_on) {
        pthread_mutex_unlock(&screen_power_mutex);
        return;
    }
    screen_on = on;

    if (!on) {
        /* Real-device bug report: brightness reset to the hardcoded 80
         * default (not the user's real prior level) every time the screen
         * turned off, whenever that level was low. Root cause, confirmed
         * live with debug logging: the old percent<->raw round trip lost
         * enough precision that a real, deliberately-set low value could
         * read back a few points lower than what was actually set, and
         * comparing that reading against a "looks like it might be a
         * failed read" threshold (rather than checking for the actual -1
         * failure sentinel) rejected valid low readings, leaving
         * restore_percent stuck at whatever it was before. >= 0 alone
         * still excludes the real failure case. */
        if (!screen_dimmed) {
            int current = backlight_get_percent();
            if (current >= 0) restore_percent = current;
        }
        screen_dimmed = false;
    }
    int target_percent = restore_percent;
    pthread_mutex_unlock(&screen_power_mutex);

    if (on) {
        /* backlight_set_percent() already maps this logical percent through
         * the same safe range backlight_get_percent() reported it from, so
         * this can't round-trip back into the invisible-at-max-brightness
         * bug either -- no separate clamping needed here. */
        backlight_set_percent(target_percent);
        return;
    }

    /* Screen truly off needs a literal 0, not just dimmed to
     * BACKLIGHT_MIN_PERCENT -- backlight_set_percent() can never reach that
     * (see its own safe-range mapping), so this writes directly instead. */
    char device_name[64];
    if (!find_backlight_device(device_name, sizeof(device_name))) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s/brightness", BACKLIGHT_CLASS_DIR, device_name);
    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "0");
    fclose(f);
}
