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
    DIR * dir = opendir(BACKLIGHT_CLASS_DIR);
    if (!dir) return false;

    bool found = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        snprintf(out, out_size, "%s", entry->d_name);
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

int backlight_get_percent(void) {
    char device_name[64];
    if (!find_backlight_device(device_name, sizeof(device_name))) return -1;

    int max = read_int_attr(device_name, "max_brightness");
    int cur = read_int_attr(device_name, "brightness");
    if (max <= 0 || cur < 0) return -1;
    return (int) (((long) cur * 100) / max);
}

void backlight_set_percent(int percent) {
    if (percent < BACKLIGHT_MIN_PERCENT) percent = BACKLIGHT_MIN_PERCENT;
    if (percent > 100) percent = 100;

    char device_name[64];
    if (!find_backlight_device(device_name, sizeof(device_name))) return;

    int max = read_int_attr(device_name, "max_brightness");
    if (max <= 0) return;
    int value = (int) (((long) percent * max) / 100);

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
/* Restored on the next turn-on -- seeded to a sensible default matching the
 * old hw_buttons.c hardcoded BACKLIGHT_ON_LEVEL, only actually used if we
 * never captured a real prior brightness (e.g. backlight_get_percent()
 * failed the first time we turned the screen off). */
static int restore_percent = 80;

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
        int current = backlight_get_percent();
        if (current >= BACKLIGHT_MIN_PERCENT) restore_percent = current;
    }
    int target_percent = on ? restore_percent : 0;
    pthread_mutex_unlock(&screen_power_mutex);

    /* backlight_set_percent() clamps to BACKLIGHT_MIN_PERCENT, which can
     * never reach a true 0 -- necessary for the normal brightness slider
     * (see its own doc comment: a slider at 0 looks broken, not off), but
     * screen-off genuinely needs 0. Written directly here instead, same
     * find_backlight_device()/max_brightness scaling as backlight_set_percent(). */
    char device_name[64];
    if (!find_backlight_device(device_name, sizeof(device_name))) return;
    int max = read_int_attr(device_name, "max_brightness");
    if (max <= 0) return;
    int value = on ? (int) (((long) target_percent * max) / 100) : 0;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s/brightness", BACKLIGHT_CLASS_DIR, device_name);
    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d", value);
    fclose(f);
}
