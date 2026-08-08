#include "led_control.h"
#include "battery.h"

#include <stdio.h>

#define LED_CLASS_DIR "/sys/class/leds"
/* max_brightness reads 100 on a real device for both LEDs, but real-device
 * feedback: brightness=100 didn't visibly light the LED, while the lower
 * value tested during development did -- these are simple PWM/current
 * driven indicator LEDs, not meant to be looked at directly, so a low,
 * confirmed-working value is the right "on" state, not literal max. */
#define LED_ON_BRIGHTNESS "1"

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
 * battery_is_charging()/battery_is_full() first. */
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

    bool full = battery_is_full();
    bool charging = !full && battery_is_charging();
    set_led("red", &red_state, charging);
    set_led("blue", &blue_state, full);
}
