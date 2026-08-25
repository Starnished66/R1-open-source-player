#include "gesture_detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SCREEN_H 320
#define BAND_H 24

static gesture_home_config_t make_default_config(void) {
    gesture_home_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.swipe_up_home_enabled = true;
    cfg.quick_drawer_open = false;
    cfg.is_bt_dac_overlay = false;
    cfg.is_usb_dac_overlay = false;
    cfg.is_lyrics_screen = false;
    cfg.has_background_work = false;
    cfg.screen_height = SCREEN_H;
    cfg.band_height = BAND_H;
    return cfg;
}

/* 1. idle -> press in bottom band -> upward movement >= threshold -> exactly one Home action */
static void test_home_swipe_basic_success(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    /* Idle / released */
    assert(!gesture_home_state_poll(&state, &cfg, false, 0));

    /* Press down in band: y = 300 (band is 320 - 24 = 296..320) */
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == true);
    assert(state.triggered == false);

    /* Move up slightly (delta = 20 < 40) */
    assert(!gesture_home_state_poll(&state, &cfg, true, 280));
    assert(state.tracking == true);
    assert(state.triggered == false);

    /* Move up past threshold (delta = 50 >= 40) -> should trigger Home */
    assert(gesture_home_state_poll(&state, &cfg, true, 250));
    assert(state.triggered == true);

    /* Further motion while still pressed should NOT trigger Home again */
    assert(!gesture_home_state_poll(&state, &cfg, true, 200));
    assert(!gesture_home_state_poll(&state, &cfg, true, 100));

    /* Release */
    assert(!gesture_home_state_poll(&state, &cfg, false, 100));
    assert(state.tracking == false);
    assert(state.triggered == false);
    assert(state.was_pressed == false);

    printf("  -> Home swipe basic success and single-shot trigger passed.\n");
}

/* Production adds two invisible pixels above the unchanged visual band. */
static void test_home_swipe_extra_hit_area(void) {
    gesture_home_state_t state;
    gesture_home_config_t cfg = make_default_config();
    cfg.band_height = BAND_H + HOME_SWIPE_HIT_EXTRA_PX;

    /* The uppermost newly-added pixel is eligible. */
    gesture_home_state_reset(&state);
    assert(!gesture_home_state_poll(&state, &cfg, true, SCREEN_H - BAND_H - 2));
    assert(state.tracking);
    assert(gesture_home_state_poll(&state, &cfg, true, SCREEN_H - BAND_H - 42));
    assert(!gesture_home_state_poll(&state, &cfg, false, 0));

    /* One pixel above the expanded target remains excluded. */
    gesture_home_state_reset(&state);
    assert(!gesture_home_state_poll(&state, &cfg, true, SCREEN_H - BAND_H - 3));
    assert(!state.tracking);
    assert(!gesture_home_state_poll(&state, &cfg, true, SCREEN_H - BAND_H - 50));
    assert(!gesture_home_state_poll(&state, &cfg, false, 0));

    printf("  -> Invisible 2px home-swipe hit-area expansion passed.\n");
}

/* 2. Repeated successful Home swipes separated by releases */
static void test_repeated_home_swipes(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    for (int iter = 0; iter < 50; iter++) {
        /* Press down */
        assert(!gesture_home_state_poll(&state, &cfg, true, 310));
        assert(state.tracking == true);

        /* Swipe up */
        assert(gesture_home_state_poll(&state, &cfg, true, 260));
        assert(state.triggered == true);

        /* Extra motion */
        assert(!gesture_home_state_poll(&state, &cfg, true, 200));

        /* Release */
        assert(!gesture_home_state_poll(&state, &cfg, false, 200));
        assert(!state.tracking);
        assert(!state.triggered);
    }

    printf("  -> Repeated successful Home swipes passed.\n");
}

/* 3. Short press / no movement does not navigate */
static void test_short_press_no_movement(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    /* Press down in band */
    assert(!gesture_home_state_poll(&state, &cfg, true, 305));
    /* Tiny jitter */
    assert(!gesture_home_state_poll(&state, &cfg, true, 303));
    assert(!gesture_home_state_poll(&state, &cfg, true, 307));
    /* Release without reaching threshold */
    assert(!gesture_home_state_poll(&state, &cfg, false, 305));

    printf("  -> Short press without movement does not navigate passed.\n");
}

/* 4. Start outside the band does not navigate even if it later enters the band */
static void test_start_outside_band(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    /* Press down at y = 200 (well above bottom band 296..320) */
    assert(!gesture_home_state_poll(&state, &cfg, true, 200));
    assert(state.tracking == false);

    /* Move down into the band */
    assert(!gesture_home_state_poll(&state, &cfg, true, 310));
    assert(state.tracking == false);

    /* Move up by large distance */
    assert(!gesture_home_state_poll(&state, &cfg, true, 100));
    assert(!gesture_home_state_poll(&state, &cfg, false, 100));

    printf("  -> Start outside band rejection passed.\n");
}

/* 5. Disabled setting and each intentional exclusion reject the gesture */
static void test_exclusions(void) {
    gesture_home_state_t state;
    gesture_home_config_t cfg;

    /* A. Setting disabled */
    gesture_home_state_reset(&state);
    cfg = make_default_config();
    cfg.swipe_up_home_enabled = false;
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == false);
    assert(!gesture_home_state_poll(&state, &cfg, true, 200));
    assert(!gesture_home_state_poll(&state, &cfg, false, 200));

    /* B. Quick drawer open */
    gesture_home_state_reset(&state);
    cfg = make_default_config();
    cfg.quick_drawer_open = true;
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == false);
    assert(!gesture_home_state_poll(&state, &cfg, true, 200));
    assert(!gesture_home_state_poll(&state, &cfg, false, 200));

    /* C. Bluetooth DAC overlay active */
    gesture_home_state_reset(&state);
    cfg = make_default_config();
    cfg.is_bt_dac_overlay = true;
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == false);
    assert(!gesture_home_state_poll(&state, &cfg, true, 200));
    assert(!gesture_home_state_poll(&state, &cfg, false, 200));

    /* D. USB DAC overlay active */
    gesture_home_state_reset(&state);
    cfg = make_default_config();
    cfg.is_usb_dac_overlay = true;
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == false);
    assert(!gesture_home_state_poll(&state, &cfg, true, 200));
    assert(!gesture_home_state_poll(&state, &cfg, false, 200));

    /* E. Lyrics screen active */
    gesture_home_state_reset(&state);
    cfg = make_default_config();
    cfg.is_lyrics_screen = true;
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == false);
    assert(!gesture_home_state_poll(&state, &cfg, true, 200));
    assert(!gesture_home_state_poll(&state, &cfg, false, 200));

    /* F. Background rescan / format work active */
    gesture_home_state_reset(&state);
    cfg = make_default_config();
    cfg.has_background_work = true;
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == false);
    assert(!gesture_home_state_poll(&state, &cfg, true, 200));
    assert(!gesture_home_state_poll(&state, &cfg, false, 200));

    printf("  -> Behavioral exclusions passed.\n");
}

/* 6. Timer paused -> raw press wake -> first poll establishes a new press */
static void test_raw_press_wake_sequence(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    /* Simulated fast timer paused at idle */
    bool timer_paused = true;

    /* Raw indev read observes pressed: wakes timer */
    timer_paused = false;

    /* First poll tick of the awakened timer */
    assert(!gesture_home_state_poll(&state, &cfg, true, 305));
    assert(state.tracking == true);
    assert(state.start_y == 305);

    /* Second poll tick: motion */
    assert(gesture_home_state_poll(&state, &cfg, true, 260));

    /* Release pauses timer again */
    assert(!gesture_home_state_poll(&state, &cfg, false, 260));
    timer_paused = true;
    assert(timer_paused == true);

    printf("  -> Raw press wake and initial poll sequence passed.\n");
}

/* 7. Missed / cancelled press followed by reset does not poison the next gesture */
static void test_cancelled_press_recovery(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    /* Gesture starts */
    assert(!gesture_home_state_poll(&state, &cfg, true, 310));
    assert(state.tracking == true);

    /* Abrupt cancellation / state reset mid-press */
    gesture_home_state_reset(&state);
    assert(!state.was_pressed);
    assert(!state.tracking);
    assert(!state.triggered);

    /* Next fresh press immediately works cleanly */
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == true);
    assert(gesture_home_state_poll(&state, &cfg, true, 250));
    assert(!gesture_home_state_poll(&state, &cfg, false, 250));

    printf("  -> Cancelled press recovery and state reset passed.\n");
}

/* 8. Screen off/on produces a clean state and first deliberate post-wake gesture works */
static void test_screen_off_on_lifecycle(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    /* Finger down before screen goes off */
    assert(!gesture_home_state_poll(&state, &cfg, true, 310));

    /* Screen turns off: reset_drag_state called */
    gesture_home_state_reset(&state);

    /* While screen is off: touch events are forced to released in read wrapper */
    assert(!gesture_home_state_poll(&state, &cfg, false, 0));

    /* Screen turns on: reset_drag_state called again for clean baseline */
    gesture_home_state_reset(&state);

    /* First deliberate press after wake */
    assert(!gesture_home_state_poll(&state, &cfg, true, 315));
    assert(state.tracking == true);
    assert(gesture_home_state_poll(&state, &cfg, true, 270));
    assert(!gesture_home_state_poll(&state, &cfg, false, 270));

    printf("  -> Screen off/on lifecycle and post-wake gesture passed.\n");
}

/* 9. Transition completion / cancellation restores gesture availability */
static void test_transition_recovery(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    /* During transition: suppose home swipe was triggered */
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(gesture_home_state_poll(&state, &cfg, true, 250));
    assert(state.triggered == true);

    /* Finger lifts */
    assert(!gesture_home_state_poll(&state, &cfg, false, 250));

    /* Transition settles -> new screen is active, gesture is immediately available */
    assert(!gesture_home_state_poll(&state, &cfg, true, 300));
    assert(state.tracking == true);
    assert(gesture_home_state_poll(&state, &cfg, true, 250));
    assert(!gesture_home_state_poll(&state, &cfg, false, 250));

    printf("  -> Transition completion and gesture availability passed.\n");
}

/* 10. Touch held across screen wake is suppressed until physical release */
static void test_touch_held_across_wake_suppression(void) {
    gesture_home_state_t state;
    gesture_home_state_reset(&state);
    gesture_home_config_t cfg = make_default_config();

    /* Simulated wake state machine */
    bool require_release_after_wake = true;

    /* Touch is held down at wake */
    bool physical_pressed = true;
    int32_t raw_y = 310;

    /* Wake evaluation helper simulation matching wrapped_pointer_read_cb */
    bool reported_pressed = physical_pressed;
    if (require_release_after_wake) {
        if (physical_pressed) {
            reported_pressed = false; /* suppressed */
        } else {
            require_release_after_wake = false;
        }
    }

    /* Poller receives suppressed state (false) */
    assert(!gesture_home_state_poll(&state, &cfg, reported_pressed, raw_y));
    assert(state.tracking == false);
    assert(state.triggered == false);

    /* Finger moves while still held from before wake */
    raw_y = 250;
    reported_pressed = physical_pressed;
    if (require_release_after_wake) {
        if (physical_pressed) {
            reported_pressed = false; /* still suppressed */
        } else {
            require_release_after_wake = false;
        }
    }
    assert(!gesture_home_state_poll(&state, &cfg, reported_pressed, raw_y));
    assert(state.tracking == false);
    assert(state.triggered == false);

    /* Finger finally lifts */
    physical_pressed = false;
    reported_pressed = physical_pressed;
    if (require_release_after_wake) {
        if (physical_pressed) {
            reported_pressed = false;
        } else {
            require_release_after_wake = false;
        }
    }
    assert(require_release_after_wake == false);
    assert(!gesture_home_state_poll(&state, &cfg, reported_pressed, raw_y));

    /* Next deliberate press-down cleanly starts a valid gesture */
    physical_pressed = true;
    raw_y = 305;
    assert(!gesture_home_state_poll(&state, &cfg, physical_pressed, raw_y));
    assert(state.tracking == true);

    /* Upward motion triggers Home */
    raw_y = 255;
    assert(gesture_home_state_poll(&state, &cfg, physical_pressed, raw_y));
    assert(state.triggered == true);

    /* Release cleans up */
    physical_pressed = false;
    assert(!gesture_home_state_poll(&state, &cfg, physical_pressed, raw_y));
    assert(state.tracking == false);

    printf("  -> Held touch suppression across wake passed.\n");
}

int main(void) {
    printf("=== Starting Gesture Subsystem Self-Tests ===\n");
    test_home_swipe_basic_success();
    test_home_swipe_extra_hit_area();
    test_repeated_home_swipes();
    test_short_press_no_movement();
    test_start_outside_band();
    test_exclusions();
    test_raw_press_wake_sequence();
    test_cancelled_press_recovery();
    test_screen_off_on_lifecycle();
    test_transition_recovery();
    test_touch_held_across_wake_suppression();
    printf("=== ALL GESTURE SELF-TESTS PASSED ===\n");
    return 0;
}
