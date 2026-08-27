#include "led_control.h"
#include "battery.h"
#include "charge_limiter.h"
#include "debug_log.h"

#include <stdio.h>

#define LED_CLASS_DIR "/sys/class/leds"
/* Real-device confirmation (2026-08-26): although max_brightness reports
 * 100 and sysfs accepts that value, writing 100 makes the physical LED only
 * flash briefly and then remain dark while brightness still reads back as
 * 100.  A direct write of 50 remains visibly lit.  Use that proven-stable
 * midpoint instead of trusting the driver's advertised maximum. */
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

    /* battery_is_charging()/battery_is_full() read this device's kernel
     * power_supply status string, which is confirmed stale specifically
     * around this app's OWN charge_limiter.c: it can keep reporting
     * "Charging" for as long as the app keeps polling, even well after the
     * PMIC's real charger-enable bit was actually cleared (see charge_
     * limiter.h's own comment on the identical staleness, and gui.c's
     * existing use of charge_limiter_is_holding() for the same reason).
     * Trusting battery_is_charging() alone here would keep the red
     * "actively charging" LED lit long after real charging had already
     * stopped at the configured cap.
     *
     * charge_limiter_is_confirmed_off() -- NOT charge_limiter_is_holding()
     * -- is folded in as the second source: is_holding() reflects the
     * app's DESIRED state, set the instant the percent threshold is
     * crossed, before disable_charging() is even attempted, and it stays
     * true through a failed-write retry window regardless of whether the
     * charger is actually off yet. is_confirmed_off() only flips once the
     * i2c write's own register-readback has verified it took effect, which
     * is what this LED must wait for -- showing blue based on intent alone
     * would light it during that retry window even if the charger were
     * still genuinely enabled. */
    bool capped = charge_limiter_is_confirmed_off();

    /* Also gate on physical external power, not just capped/full status:
     * capped stays true (charge_limiter.c's own hysteresis, see its
     * CHARGE_LIMITER_RESUME_PERCENT) until the battery discharges back down
     * to 82%, which comfortably outlives a real unplug at 84-85% -- without
     * this check, unplugging right after the limiter capped charging would
     * leave the blue "done charging" LED lit while the device is actually
     * running on battery. UNKNOWN (a transient sysfs read failure, not a
     * confirmed cable removal -- see battery.h's own comment) is treated
     * the same as CONNECTED here: a briefly-wrong blue during a transient
     * glitch is far less misleading than briefly turning it off while
     * still genuinely plugged in, same conservative bias battery.h's own
     * documented UNKNOWN-vs-DISCONNECTED distinction already exists for. */
    bool power_disconnected = battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_DISCONNECTED;
    bool full = !power_disconnected && (battery_is_full() || capped);
    bool charging = !full && !power_disconnected && battery_is_charging();
    DBG_LOG("led_control: capped=%d power_state=%d full=%d charging=%d is_full=%d is_charging=%d red_state=%d blue_state=%d\n",
            capped, (int) battery_get_external_power_state(), full, charging,
            battery_is_full(), battery_is_charging(), red_state, blue_state);
    set_led("red", &red_state, charging);
    set_led("blue", &blue_state, full);
}
