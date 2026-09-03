#include "gui_lock_screen.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "gui_theme.h"
#include "gui_player.h"
#include "app_clock.h"
#include "assets.h"
#include "screen_builders.h"
#include "gesture_detector.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static lv_obj_t * lock_screen = NULL;
static lv_obj_t * lock_image_obj = NULL;
static lv_obj_t * lock_clock_label = NULL;
static lv_timer_t * lock_clock_timer = NULL;
static lv_timer_t * lock_touch_timer = NULL;

static gui_lock_screen_mode_t current_mode = LOCK_SCREEN_MODE_OFF;
static bool current_clock_24h = true;

/* Swipe-up-to-dismiss tracking -- reuses the same detector already driving
 * the home-indicator swipe gesture elsewhere (gui_shell.c) rather than
 * hand-rolling a second copy of the same press/track/threshold bookkeeping.
 * band_height is set to the full screen height at poll time (see
 * lock_touch_timer_cb()) so every press anywhere on the lock screen is
 * eligible, not just one starting in a narrow bottom band like the home
 * indicator's own gesture. */
static gesture_home_state_t lock_gesture_state;

lv_obj_t * gui_lock_screen_get_screen(void) {
    return lock_screen;
}

bool gui_lock_screen_is_showing(void) {
    return lock_screen != NULL && lv_screen_active() == lock_screen;
}

static void update_clock_display(void) {
    if (!lock_clock_label) return;
    struct tm tm_info;
    app_clock_localtime(&tm_info);
    char buf[16];
    strftime(buf, sizeof(buf), current_clock_24h ? "%H:%M" : "%I:%M", &tm_info);
    lv_label_set_text(lock_clock_label, buf);
}

static void lock_clock_timer_cb(lv_timer_t * timer) {
    (void) timer;
    update_clock_display();
}

static void lock_touch_timer_cb(lv_timer_t * timer) {
    (void) timer;
    if (!gui_lock_screen_is_showing()) return;

    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED);
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    gesture_home_config_t cfg;
    int32_t screen_height = lv_display_get_vertical_resolution(lv_display_get_default());
    cfg.swipe_up_home_enabled = true;
    cfg.quick_drawer_open = false;
    cfg.is_bt_dac_overlay = false;
    cfg.is_usb_dac_overlay = false;
    cfg.is_lyrics_screen = false;
    cfg.is_lock_screen = false;
    cfg.has_background_work = false;
    cfg.screen_height = screen_height;
    cfg.band_height = screen_height; /* the whole screen is the swipe surface, not just a bottom band */

    bool dismiss = gesture_home_state_poll(&lock_gesture_state, &cfg, pressed, p.y);
    if (dismiss) {
        /* Same real-device fix already applied to the home-swipe and
         * player-swipe gestures elsewhere (gui_navigation.c) -- without
         * this, the finger is still down when nav_pop() loads the screen
         * underneath, and the eventual release lands on whatever's at that
         * coordinate there (e.g. a Home tile), firing an unintended tap
         * right as the lock screen dismisses. */
        lv_indev_wait_release(indev);
        gui_lock_screen_hide();
    }
}

static void start_timers(void) {
    /* Symmetric, not just a conditional start -- gui_lock_screen_show() can
     * be called again with a DIFFERENT mode while already showing (e.g. a
     * second screen_woke fires before the user dismisses), and this must
     * leave lock_clock_timer matching the NEW mode either way. A one-sided
     * "start if clock" here previously left a stale timer running forever
     * (until the eventual hide/teardown) after switching away from clock
     * mode without an intervening hide(). */
    if (current_mode == LOCK_SCREEN_MODE_CLOCK) {
        if (!lock_clock_timer) {
            lock_clock_timer = lv_timer_create(lock_clock_timer_cb, 1000, NULL);
        }
    } else if (lock_clock_timer) {
        lv_timer_delete(lock_clock_timer);
        lock_clock_timer = NULL;
    }
    if (!lock_touch_timer) {
        lock_touch_timer = lv_timer_create(lock_touch_timer_cb, 20, NULL);
    }
}

static void stop_timers(void) {
    if (lock_clock_timer) {
        lv_timer_delete(lock_clock_timer);
        lock_clock_timer = NULL;
    }
    if (lock_touch_timer) {
        lv_timer_delete(lock_touch_timer);
        lock_touch_timer = NULL;
    }
    gesture_home_state_reset(&lock_gesture_state);
}

static void build_lock_screen_if_needed(void) {
    if (lock_screen) return;

    lock_screen = lv_obj_create(NULL);
    lv_obj_add_style(lock_screen, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(lock_screen, LV_OBJ_FLAG_SCROLLABLE);

    lock_image_obj = lv_image_create(lock_screen);
    lv_obj_align(lock_image_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);

    lock_clock_label = lv_label_create(lock_screen);
    lv_obj_add_style(lock_clock_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_align(lock_clock_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lock_clock_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align(lock_clock_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lock_clock_label, LV_OBJ_FLAG_HIDDEN);
}

bool gui_lock_screen_show(const gui_lock_screen_options_t * options) {
    if (!options || options->mode == LOCK_SCREEN_MODE_OFF) {
        return false;
    }

    build_lock_screen_if_needed();

    current_mode = options->mode;
    current_clock_24h = options->clock_24h;

    /* Reset elements */
    lv_obj_add_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lock_clock_label, LV_OBJ_FLAG_HIDDEN);

    if (current_mode == LOCK_SCREEN_MODE_ALBUM_ART) {
        const lv_image_dsc_t * cover = gui_player_get_current_cover_dsc();
        if (cover && cover->data) {
            lv_image_set_src(lock_image_obj, cover);
        } else {
            lv_image_set_src(lock_image_obj, asset_path("playing_plane/default_cover_565.png"));
        }
        lv_obj_remove_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);
    } else if (current_mode == LOCK_SCREEN_MODE_IMAGE) {
        /* LVGL's lv_fs_get_drv() picks a driver off src[0] -- a plain POSIX
         * path (what plugin.sd_root() and every plugin-supplied path use)
         * has no registered driver (only the 'S' POSIX driver is, see
         * lv_conf.h), so lv_fs_open() fails silently and the image never
         * loads without this prefix. Same "S:" convention every other
         * file-path image source in this codebase uses (assets.c's
         * asset_path()). lv_image_set_src() strdup()s file-path sources
         * internally, so this stack buffer doesn't need to outlive the call. */
        char prefixed_path[sizeof(options->image_path) + 2];
        snprintf(prefixed_path, sizeof(prefixed_path), "S:%s", options->image_path);
        lv_image_set_src(lock_image_obj, prefixed_path);
        lv_obj_remove_flag(lock_image_obj, LV_OBJ_FLAG_HIDDEN);
    } else if (current_mode == LOCK_SCREEN_MODE_CLOCK) {
        update_clock_display();
        lv_obj_remove_flag(lock_clock_label, LV_OBJ_FLAG_HIDDEN);
    }

    start_timers();

    if (lv_screen_active() != lock_screen) {
        nav_push(lock_screen);
    }

    return true;
}

void gui_lock_screen_hide(void) {
    stop_timers();
    if (lock_screen && lv_screen_active() == lock_screen) {
        nav_pop();
    }
}

void gui_lock_screen_init(void) {
    lock_screen = NULL;
    lock_image_obj = NULL;
    lock_clock_label = NULL;
    lock_clock_timer = NULL;
    lock_touch_timer = NULL;
    current_mode = LOCK_SCREEN_MODE_OFF;
    gesture_home_state_reset(&lock_gesture_state);
}

/* Called from gui_soft_reload() (gui_reload.c), after gui_navigation_teardown()
 * has already zeroed nav_stack/nav_depth -- calling nav_pop() here (as an
 * earlier version of this function did) would read nav_stack[-1], the exact
 * out-of-bounds class of crash already found and fixed once in this reload
 * path for the Plugin Manager. A reload rebuilds the whole screen stack from
 * scratch (gui_navigation_init() loads Home again further down the same
 * sequence), so the lock screen doesn't need to navigate anywhere on its way
 * out -- it only needs to release its own owned resources. */
void gui_lock_screen_teardown(void) {
    stop_timers();
    if (lock_screen) {
        lv_obj_delete(lock_screen);
    }
    gui_lock_screen_init();
}
