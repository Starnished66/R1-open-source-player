#ifndef GESTURE_DETECTOR_H
#define GESTURE_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_SWIPE_UP_THRESHOLD 40
#define HOME_SWIPE_HIT_EXTRA_PX 7

typedef struct {
    bool swipe_up_home_enabled;
    bool quick_drawer_open;
    bool is_bt_dac_overlay;
    bool is_usb_dac_overlay;
    bool is_lyrics_screen;
    bool is_lock_screen;
    bool has_background_work;
    int32_t screen_height;
    int32_t band_height;
} gesture_home_config_t;

typedef struct {
    bool was_pressed;
    bool tracking;
    bool triggered;
    int32_t start_y;
} gesture_home_state_t;

/* Resets all state for a clean released baseline */
void gesture_home_state_reset(gesture_home_state_t * state);

/* Evaluates a press event (e.g. called each poll tick).
 * Returns true if Home navigation should be triggered on this tick (fired at most once per press). */
bool gesture_home_state_poll(gesture_home_state_t * state,
                             const gesture_home_config_t * cfg,
                             bool pressed,
                             int32_t touch_y);

#ifdef __cplusplus
}
#endif

#endif /* GESTURE_DETECTOR_H */
