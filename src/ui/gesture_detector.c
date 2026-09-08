#include "gesture_detector.h"

void gesture_home_state_reset(gesture_home_state_t * state) {
    if (!state) return;
    state->was_pressed = false;
    state->tracking = false;
    state->triggered = false;
    state->start_y = 0;
}

bool gesture_home_state_is_eligible(const gesture_home_config_t * cfg, int32_t touch_y) {
    if (!cfg) return false;

    bool in_band = (touch_y >= cfg->screen_height - cfg->band_height);
    return cfg->swipe_up_home_enabled &&
           !cfg->quick_drawer_open &&
           !cfg->is_bt_dac_overlay &&
           !cfg->is_usb_dac_overlay &&
           !cfg->is_lyrics_screen &&
           !cfg->is_lock_screen &&
           !cfg->has_background_work &&
           in_band;
}

bool gesture_home_state_poll(gesture_home_state_t * state,
                             const gesture_home_config_t * cfg,
                             bool pressed,
                             int32_t touch_y) {
    if (!state || !cfg) return false;
    bool trigger_nav = false;

    if (pressed && !state->was_pressed) {
        /* Press-down edge: evaluate eligibility for home gesture tracking */
        state->tracking = gesture_home_state_is_eligible(cfg, touch_y);
        state->start_y = touch_y;
        state->triggered = false;
    }

    if (pressed && state->tracking && !state->triggered) {
        int32_t delta_y = state->start_y - touch_y;
        if (delta_y >= HOME_SWIPE_UP_THRESHOLD) {
            state->triggered = true;
            trigger_nav = true;
        }
    }

    if (!pressed && state->was_pressed) {
        state->tracking = false;
        state->triggered = false;
    }

    state->was_pressed = pressed;
    return trigger_nav;
}
