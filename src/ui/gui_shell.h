#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

lv_obj_t * gui_shell_get_home_screen(void);
lv_obj_t * gui_shell_get_dac_home_screen(void);
void gui_shell_refresh_home(void);

/* Effective Wi-Fi enabled state: wifi_control_is_enabled(), except an
 * in-flight toggle's target state wins while it's still settling (see the
 * .c definition). Read-only -- exposes no way to drive the toggle itself. */
bool gui_shell_wifi_effective_enabled(void);

void gui_shell_update_quick_drawer_track(const char * title, const char * artist);
/* Refreshes the drawer's own small cover thumbnail from whatever
 * gui_player_get_current_cover_dsc() currently returns (or the default
 * placeholder if NULL) -- call whenever that changes, not just on a title/
 * artist change, since cover decode can complete later than metadata. */
void gui_shell_refresh_quick_drawer_cover(void);
/* Re-decodes the drawer's four "on" toggle icons with the current accent
 * baked into their circle (they ship with a fixed #009FF6 that LVGL's own
 * image_recolor cannot retint without flattening the glyph too) and
 * re-points the widgets at the new buffers. Call after the accent changes. */
void gui_shell_refresh_quick_drawer_toggle_accent(void);
void gui_shell_update_quick_drawer_favorite(bool is_favorite);
void gui_shell_update_quick_drawer_play_state(bool is_playing);
void gui_shell_update_quick_drawer_play_mode(int mode);

void gui_shell_init(uint32_t screen_width, uint32_t screen_height);
/* The screen-construction half of gui_shell_init(), without the Bluetooth-
 * adjacent startup calls it also makes -- see its own comment. Used by
 * gui_reload.c's in-process UI reload. */
void gui_shell_build_screens(uint32_t screen_width, uint32_t screen_height);
/* Deletes every screen/top-layer object gui_shell.c owns (Home, DAC Home,
 * status bar, home indicator bar, quick drawer) -- see its own comment.
 * Used by gui_reload.c before calling gui_shell_build_screens() again. */
void gui_shell_teardown(void);
void gui_shell_refresh_static_assets(void);
void refresh_clock_label(void);
void refresh_battery_topbar(void);
void refresh_wifi_topbar(void);
void refresh_volume_topbar(int32_t percent);
void quick_drawer_mark_snapshot_dirty(void);
void register_swipe_dead_zone(lv_obj_t * obj);
void unregister_swipe_dead_zone(lv_obj_t * obj);
/* For gui_reload.c's in-process UI reload -- see its own comment. Must run
 * before any screen's sliders are actually deleted. */
void reset_swipe_dead_zones(void);
void poll_quick_drawer(void);
void open_quick_drawer(void);
void close_quick_drawer(void);

bool gui_shell_is_bt_audio_connected(void);
void gui_shell_notify_bt_audio_disconnected(void);
/* Cancels persisted-device auto-reconnect and suppresses it for the current
 * powered Bluetooth cycle. Manual connect/forget actions use this so an
 * automatic worker cannot race the user's explicit choice. */
void gui_shell_cancel_bt_reconnect(void);

void gui_shell_poll(void);
void refresh_quick_drawer_crossfade_icon(void);

/* Re-reads the quick drawer's expanded-row tiles (AirPlay/DLNA/Gapless/RC
 * and any plugin tiles) from their authoritative sources. Safe before the
 * drawer is built. Call after changing one of those settings from outside
 * the drawer, so its tile doesn't show a stale state next time it opens. */
void gui_shell_refresh_quick_drawer_expansion_toggles(void);
bool quick_drawer_sleep_timer_is_active(void);
void quick_drawer_sleep_timer_set_active(bool active);
int quick_drawer_sleep_timer_remaining_seconds(void);
void gui_shell_resume_connections(bool wifi_was_on, bool bt_was_on);

void gui_shell_suspend_connections(bool * wifi_was_on, bool * bt_was_on);
void refresh_headphone_icon(void);

void gui_shell_update_topbar(bool screen_just_woke);

bool point_in_swipe_dead_zone(lv_point_t p);
void gui_shell_resume_fast_timers(void);
void gui_shell_reset_drag_state(void);
/* Narrow counterpart to gui_shell_reset_drag_state() above -- resets only
 * the interactive slide-swipe tracking state (player_swipe_* and
 * back_swipe_*) without touching quick-drawer or home-gesture state,
 * and does NOT call slide_transition_cancel().
 * Called by gui_navigation.c on compositor-failure recovery
 * (slide_transition_anim_x_cb()) before freeing the slide_transition_ctx_t.
 * Takes void* so this header does not need gui_navigation.h's type definition. */
void gui_shell_player_swipe_recover(void * ctx);
/* True from press-down until release whenever that press was eligible for
 * the live-tracking back-swipe (regardless of whether its own deadzone ever
 * confirmed a direction) -- screen_gesture_event_cb() must stand down its
 * own LV_DIR_RIGHT handling for the whole press when this is true, since
 * LVGL's native gesture recognition can win the race against this poll-
 * based deadzone check for the exact same direction on a fast swipe. */
bool gui_shell_back_swipe_owns_press(void);
void gui_shell_install_indev_hooks(lv_indev_t * indev);
/* Not just lv_indev_get_next(NULL) -- the host simulator also registers a
 * keyboard indev, so which one enumerates first isn't guaranteed. Shared by
 * every raw-touch-polling timer in this codebase -- see the .c definition's
 * own comment. */
lv_indev_t * find_pointer_indev(void);

bool gui_shell_has_background_work(void);
void gui_shell_cancel_background_work(void);


lv_obj_t * gui_shell_get_status_bar_band(void);
lv_obj_t * gui_shell_get_home_indicator_band(void);
void gui_shell_set_status_bar_screen_context(lv_obj_t * screen);
void gui_shell_set_home_indicator_visible(bool visible);
