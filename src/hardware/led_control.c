#include "led_control.h"
#include "battery.h"
#include "charge_limiter.h"
#include "debug_log.h"

#include <stdio.h>

#define LED_CLASS_DIR "/sys/class/leds"
/* Uses brightness 50 for a stable visible midpoint level. */
#define LED_ON_BRIGHTNESS "50"

static void write_led_attr(const char * led_name, const char * attr, const char * value) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s/%s", LED_CLASS_DIR, led_name, attr);

    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s", value);
    fclose(f);
}

/* -1 so the very first led_control_poll() call after led_control_apply()
 * always actually writes, regardless of what state happens to come out of
 * battery_is_charging()/battery_is_full()/charge_limiter_is_holding() first. */
static int red_state = -1;
static int blue_state = -1;

static void set_led(const char * led_name, int * cached_state, bool on) {
    if (*cached_state == (on ? 1 : 0)) return; /* already in this state, skip the redundant write */
    write_led_attr(led_name, "brightness", on ? LED_ON_BRIGHTNESS : "0");
    *cached_state = on ? 1 : 0;
}

void led_control_apply(bool enabled) {
    /* Reasserts trigger=none every time (cheap, and the only way to be
     * sure we still own brightness even if something else re-attached a
     * kernel trigger in between) rather than assuming it stuck from a
     * previous call. */
    write_led_attr("red", "trigger", "none");
    write_led_attr("blue", "trigger", "none");
    red_state = blue_state = -1; /* force the poll below to actually write, not skip as "unchanged" */

    if (!enabled) {
        set_led("red", &red_state, false);
        set_led("blue", &blue_state, false);
        return;
    }

    led_control_poll(true);
}

void led_control_poll(bool enabled) {
    if (!enabled) return;

    /* Gated by charge_limiter_is_confirmed_off() so intentional charging caps
     * are treated as completed even if power supply status reports charging. */
    bool capped = charge_limiter_is_confirmed_off();

    /* Also gate on physical external power so the full/capped LED is not lit
     * when unplugged and running on battery. */
    bool power_disconnected = battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_DISCONNECTED;
    bool full = !power_disconnected && (battery_is_full() || capped);
    bool charging = !full && !power_disconnected && battery_is_charging();
    DBG_LOG("led_control: capped=%d power_state=%d full=%d charging=%d is_full=%d is_charging=%d red_state=%d blue_state=%d\n",
            capped, (int) battery_get_external_power_state(), full, charging,
            battery_is_full(), battery_is_charging(), red_state, blue_state);
    set_led("red", &red_state, charging);
    set_led("blue", &blue_state, full);
}
