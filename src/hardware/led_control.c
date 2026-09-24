#include "led_control.h"
#include "battery.h"
#include "charge_limiter.h"
#include "debug_log.h"
#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define LED_CLASS_DIR "/sys/class/leds"
/* Keep the charge-status indicator at its existing raw midpoint brightness. */
#define LED_STATUS_RAW_BRIGHTNESS 50
#define LED_RED_RAW_BRIGHTNESS_MAX 100
/* Verified on the device: blue dims or turns off above raw brightness 50. */
#define LED_BLUE_RAW_BRIGHTNESS_MAX 50
#define LED_EFFECT_TIMER_MS 40
#define LED_BLINK_MIN_MS 50
#define LED_BLINK_MAX_MS 60000
#define LED_BREATHE_MIN_MS 100
#define LED_BREATHE_MAX_MS 120000

static const char * const led_names[] = { "red", "blue" };

static const int led_raw_max[] = {
    LED_RED_RAW_BRIGHTNESS_MAX,
    LED_BLUE_RAW_BRIGHTNESS_MAX,
};

typedef struct {
    led_control_mode_t mode;
    int level;
    int current_level;
    int on_ms;
    int off_ms;
    int period_ms;
    int breaths_per_min;
    bool blink_on;
    bool kernel_breathe;
    lv_timer_t * timer;
    uint32_t effect_start_tick;
} led_state_t;

static led_state_t led_states[2] = {
    { .mode = LED_CONTROL_MODE_STATUS },
    { .mode = LED_CONTROL_MODE_STATUS },
};
static int brightness_cache[2] = { -1, -1 };
static char trigger_cache[2][16];
static bool override_active = false;
static bool effects_suspended = false;
static bool status_enabled = false;

static int led_index(led_control_color_t color) {
    if (color == LED_CONTROL_RED) return 0;
    if (color == LED_CONTROL_BLUE) return 1;
    return -1;
}

static bool write_led_attr(int index, const char * attr, const char * value) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s/%s", LED_CLASS_DIR, led_names[index], attr);

    FILE * f = fopen(path, "w");
    if (!f) return false;
    bool ok = fprintf(f, "%s", value) >= 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool set_trigger(int index, const char * trigger, bool force) {
    if (!force && strcmp(trigger_cache[index], trigger) == 0) return true;
    if (!write_led_attr(index, "trigger", trigger)) return false;
    snprintf(trigger_cache[index], sizeof(trigger_cache[index]), "%s", trigger);
    /* Kernel triggers own brightness while attached, so the next manual
     * brightness command must be written even if it matches the old cache. */
    brightness_cache[index] = -1;
    return true;
}

static int user_level_to_raw(int index, int level) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    return (level * led_raw_max[index] + 50) / 100;
}

static int raw_to_user_level(int index, int raw) {
    if (raw < 0) return 0;
    if (raw > led_raw_max[index]) raw = led_raw_max[index];
    return (raw * 100 + led_raw_max[index] / 2) / led_raw_max[index];
}

static bool set_raw_brightness(int index, int raw) {
    char value[16];
    if (raw < 0) raw = 0;
    if (raw > led_raw_max[index]) raw = led_raw_max[index];
    if (brightness_cache[index] == raw) return true;
    snprintf(value, sizeof(value), "%d", raw);
    if (!write_led_attr(index, "brightness", value)) return false;
    brightness_cache[index] = raw;
    return true;
}

static bool set_user_brightness(int index, int level) {
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    led_states[index].current_level = level;
    return set_raw_brightness(index, user_level_to_raw(index, level));
}

static void delete_effect_timer(int index) {
    if (led_states[index].timer) {
        lv_timer_delete(led_states[index].timer);
        led_states[index].timer = NULL;
    }
}

static void cancel_color_effect(int index) {
    delete_effect_timer(index);
    set_trigger(index, "none", false);
    led_states[index].kernel_breathe = false;
    led_states[index].blink_on = false;
}

static void begin_override(void) {
    if (override_active) return;
    override_active = true;
    for (int i = 0; i < 2; i++) {
        led_states[i].mode = LED_CONTROL_MODE_STATUS;
        set_trigger(i, "none", false);
    }
}

static void apply_status_colors(bool enabled) {
    bool wants_status[2] = {
        led_states[0].mode == LED_CONTROL_MODE_STATUS,
        led_states[1].mode == LED_CONTROL_MODE_STATUS,
    };
    if (!wants_status[0] && !wants_status[1]) return;

    int red_raw = 0;
    int blue_raw = 0;
    if (enabled) {
        /* Keep charge-status behavior at raw 50 for both LEDs. */
        bool capped = charge_limiter_is_confirmed_off();
        battery_external_power_state_t power_state = battery_get_external_power_state();
        bool power_disconnected = power_state == BATTERY_EXTERNAL_POWER_DISCONNECTED;
        bool full = !power_disconnected && (battery_is_full() || capped);
        bool charging = !full && !power_disconnected && battery_is_charging();
        red_raw = charging ? LED_STATUS_RAW_BRIGHTNESS : 0;
        blue_raw = full ? LED_STATUS_RAW_BRIGHTNESS : 0;
        DBG_LOG("led_control: capped=%d power_state=%d full=%d charging=%d red_raw=%d blue_raw=%d\n",
                capped, (int) power_state, full, charging, red_raw, blue_raw);
    }

    if (wants_status[0]) {
        led_states[0].current_level = raw_to_user_level(0, red_raw);
        set_raw_brightness(0, red_raw);
    }
    if (wants_status[1]) {
        led_states[1].current_level = raw_to_user_level(1, blue_raw);
        set_raw_brightness(1, blue_raw);
    }
}

static void led_effect_timer_cb(lv_timer_t * timer) {
    led_state_t * state = (led_state_t *) lv_timer_get_user_data(timer);
    if (!state || !override_active || effects_suspended || state->timer != timer) return;
    int index = (int) (state - led_states);
    if (index < 0 || index > 1) return;

    if (state->mode == LED_CONTROL_MODE_BLINK) {
        state->blink_on = !state->blink_on;
        int level = state->blink_on ? state->level : 0;
        set_user_brightness(index, level);
        lv_timer_set_period(timer, state->blink_on ? state->on_ms : state->off_ms);
    } else if (state->mode == LED_CONTROL_MODE_BREATHE && !state->kernel_breathe) {
        uint32_t elapsed = lv_tick_elaps(state->effect_start_tick);
        uint32_t phase = elapsed % (uint32_t) state->period_ms;
        uint32_t half = (uint32_t) state->period_ms / 2;
        int level;
        if (phase < half) {
            level = half ? (int) ((uint64_t) state->level * phase / half) : state->level;
        } else {
            uint32_t falling = (uint32_t) state->period_ms - phase;
            level = half ? (int) ((uint64_t) state->level * falling / half) : 0;
        }
        if (level < 0) level = 0;
        if (level > state->level) level = state->level;
        set_user_brightness(index, level);
    }
}

static bool start_software_breathe(int index) {
    led_state_t * state = &led_states[index];
    state->kernel_breathe = false;
    state->current_level = 0;
    state->effect_start_tick = lv_tick_get();
    set_user_brightness(index, 0);
    state->timer = lv_timer_create(led_effect_timer_cb, LED_EFFECT_TIMER_MS, state);
    return state->timer != NULL;
}

bool led_control_available(void) {
    char red_path[128];
    char blue_path[128];
    snprintf(red_path, sizeof(red_path), "%s/red/brightness", LED_CLASS_DIR);
    snprintf(blue_path, sizeof(blue_path), "%s/blue/brightness", LED_CLASS_DIR);
    return access(red_path, F_OK) == 0 && access(blue_path, F_OK) == 0;
}

void led_control_apply(bool enabled) {
    status_enabled = enabled;
    /* A plugin's explicit modes survive changes to the user's indicator
     * preference; only colors returned to status mode follow that setting. */
    /* Forced writes: reasserting trigger=none (which also drops the brightness
     * cache) is the only way to be sure we still own brightness after
     * something else attached a kernel trigger or a suspend reset the PWM. */
    if (override_active) {
        for (int i = 0; i < 2; i++) {
            if (led_states[i].mode == LED_CONTROL_MODE_STATUS) set_trigger(i, "none", true);
        }
        apply_status_colors(enabled);
        return;
    }

    for (int i = 0; i < 2; i++) {
        delete_effect_timer(i);
        led_states[i].mode = LED_CONTROL_MODE_STATUS;
        led_states[i].level = 0;
        led_states[i].kernel_breathe = false;
        set_trigger(i, "none", true);
    }
    apply_status_colors(enabled);
}

void led_control_set_override(led_control_color_t color, int level) {
    int index = led_index(color);
    if (index < 0) return;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    begin_override();
    cancel_color_effect(index);
    led_states[index].level = level;
    led_states[index].mode = level == 0 ? LED_CONTROL_MODE_OFF : LED_CONTROL_MODE_ON;
    set_user_brightness(index, level);
}

bool led_control_blink(led_control_color_t color, int on_ms, int off_ms, int level) {
    int index = led_index(color);
    if (index < 0) return false;
    if (on_ms < LED_BLINK_MIN_MS) on_ms = LED_BLINK_MIN_MS;
    if (on_ms > LED_BLINK_MAX_MS) on_ms = LED_BLINK_MAX_MS;
    if (off_ms < LED_BLINK_MIN_MS) off_ms = LED_BLINK_MIN_MS;
    if (off_ms > LED_BLINK_MAX_MS) off_ms = LED_BLINK_MAX_MS;
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    begin_override();
    cancel_color_effect(index);
    led_states[index].mode = level == 0 ? LED_CONTROL_MODE_OFF : LED_CONTROL_MODE_BLINK;
    led_states[index].level = level;
    led_states[index].on_ms = on_ms;
    led_states[index].off_ms = off_ms;
    led_states[index].blink_on = true;
    if (level == 0) {
        set_user_brightness(index, 0);
        return true;
    }
    set_user_brightness(index, level);
    if (effects_suspended) return true;
    led_states[index].timer = lv_timer_create(led_effect_timer_cb, on_ms, &led_states[index]);
    return led_states[index].timer != NULL;
}

bool led_control_breathe(led_control_color_t color, int period_ms, int level) {
    int index = led_index(color);
    if (index < 0) return false;
    if (period_ms < LED_BREATHE_MIN_MS) period_ms = LED_BREATHE_MIN_MS;
    if (period_ms > LED_BREATHE_MAX_MS) period_ms = LED_BREATHE_MAX_MS;
    if (level < 0) level = 0;
    if (level > 100) level = 100;

    begin_override();
    cancel_color_effect(index);
    led_states[index].mode = level == 0 ? LED_CONTROL_MODE_OFF : LED_CONTROL_MODE_BREATHE;
    led_states[index].level = level;
    led_states[index].period_ms = period_ms;
    led_states[index].breaths_per_min = (60000 + period_ms / 2) / period_ms;
    if (led_states[index].breaths_per_min < 1) led_states[index].breaths_per_min = 1;
    if (led_states[index].breaths_per_min > 600) led_states[index].breaths_per_min = 600;
    if (level == 0) {
        set_user_brightness(index, 0);
        return true;
    }

    /* The red LED can safely use the full hardware range. Blue's verified
     * raw ceiling is 50, so its kernel trigger (which ramps to raw 100) is
     * never used. Partial peaks also use software stepping. */
    if (index == 0 && level == 100 && !effects_suspended) {
        set_user_brightness(index, 100);
        char bpm[8];
        snprintf(bpm, sizeof(bpm), "%d", led_states[index].breaths_per_min);
        if (set_trigger(index, "breathing", false) &&
            write_led_attr(index, "breaths_per_min", bpm)) {
            led_states[index].kernel_breathe = true;
            led_states[index].current_level = 100;
            return true;
        }
        set_trigger(index, "none", true);
    }
    return start_software_breathe(index);
}

void led_control_set_status(led_control_color_t color) {
    int index = led_index(color);
    if (index < 0) return;
    begin_override();
    cancel_color_effect(index);
    led_states[index].mode = LED_CONTROL_MODE_STATUS;
    led_states[index].level = 0;
    apply_status_colors(status_enabled);
}

void led_control_set_all_status(void) {
    begin_override();
    for (int i = 0; i < 2; i++) {
        cancel_color_effect(i);
        led_states[i].mode = LED_CONTROL_MODE_STATUS;
        led_states[i].level = 0;
    }
    apply_status_colors(status_enabled);
}

void led_control_clear_override(void) {
    for (int i = 0; i < 2; i++) {
        cancel_color_effect(i);
        led_states[i].mode = LED_CONTROL_MODE_STATUS;
        led_states[i].level = 0;
    }
    override_active = false;
}

void led_control_suspend(void) {
    if (effects_suspended) return;
    effects_suspended = true;
    for (int i = 0; i < 2; i++) {
        if (led_states[i].timer) lv_timer_pause(led_states[i].timer);
        if (led_states[i].kernel_breathe) set_trigger(i, "none", false);
    }
}

static void resume_color_effect(int index) {
    led_state_t * state = &led_states[index];
    set_trigger(index, "none", true);
    if (state->mode == LED_CONTROL_MODE_OFF) {
        state->current_level = 0;
        set_user_brightness(index, 0);
    } else if (state->mode == LED_CONTROL_MODE_ON) {
        set_user_brightness(index, state->level);
    } else if (state->mode == LED_CONTROL_MODE_BLINK) {
        state->blink_on = true;
        set_user_brightness(index, state->level);
        if (!state->timer) state->timer = lv_timer_create(led_effect_timer_cb, state->on_ms, state);
        if (state->timer) {
            lv_timer_set_period(state->timer, state->on_ms);
            lv_timer_reset(state->timer);
            lv_timer_resume(state->timer);
        }
    } else if (state->mode == LED_CONTROL_MODE_BREATHE) {
        if (state->kernel_breathe && index == 0 && state->level == 100) {
            set_user_brightness(index, 100);
            char bpm[8];
            snprintf(bpm, sizeof(bpm), "%d", state->breaths_per_min);
            if (set_trigger(index, "breathing", false) &&
                write_led_attr(index, "breaths_per_min", bpm)) {
                state->current_level = 100;
                return;
            }
            set_trigger(index, "none", true);
            state->kernel_breathe = false;
        }
        state->effect_start_tick = lv_tick_get();
        state->current_level = 0;
        set_user_brightness(index, 0);
        if (!state->timer) state->timer = lv_timer_create(led_effect_timer_cb, LED_EFFECT_TIMER_MS, state);
        if (state->timer) {
            lv_timer_set_period(state->timer, LED_EFFECT_TIMER_MS);
            lv_timer_reset(state->timer);
            lv_timer_resume(state->timer);
        }
    } else if (state->mode == LED_CONTROL_MODE_STATUS) {
        state->current_level = 0;
    }
}

void led_control_resume(bool enabled) {
    if (!effects_suspended) return;
    effects_suspended = false;
    if (!override_active) {
        led_control_apply(enabled);
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (led_states[i].mode != LED_CONTROL_MODE_STATUS) resume_color_effect(i);
        else set_trigger(i, "none", true);
    }
    apply_status_colors(enabled);
}

void led_control_poll(bool enabled) {
    status_enabled = enabled;
    if (!override_active && !enabled) return;
    apply_status_colors(enabled);
}

led_control_mode_t led_control_get_mode(led_control_color_t color) {
    int index = led_index(color);
    return index < 0 ? LED_CONTROL_MODE_OFF : led_states[index].mode;
}

int led_control_get_level(led_control_color_t color) {
    int index = led_index(color);
    if (index < 0) return 0;
    return led_states[index].mode == LED_CONTROL_MODE_STATUS
               ? led_states[index].current_level
               : led_states[index].level;
}
