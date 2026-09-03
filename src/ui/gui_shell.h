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

void gui_shell_poll(void);
void refresh_quick_drawer_crossfade_icon(void);
void gui_shell_resume_connections(bool wifi_was_on, bool bt_was_on);

void gui_shell_suspend_connections(bool * wifi_was_on, bool * bt_was_on);
void refresh_headphone_icon(void);

void gui_shell_update_topbar(bool screen_just_woke);

bool point_in_swipe_dead_zone(lv_point_t p);
void gui_shell_resume_fast_timers(void);
void gui_shell_reset_drag_state(void);
/* Narrow counterpart to gui_shell_reset_drag_state() above -- resets only
 * this file's own real interactive player-swipe tracking state
 * (player_swipe_tracking/candidate/ctx), not the unrelated quick-drawer/
 * home-gesture state that function also touches, and does NOT call
 * slide_transition_cancel() on a still-live context the way that function
 * does. For gui_navigation.c's own compositor-failure recovery
 * (slide_transition_anim_x_cb()) to call on the SAME slide_transition_ctx_t
 * it is itself about to lv_free() -- MUST be called before that free, and
 * takes void* rather than slide_transition_ctx_t* so this header doesn't
 * need gui_navigation.h's own type definition just for one declaration.
 * Real-device review finding: before this existed, that recovery path only
 * reset gui_navigation.c's OWN same-named statics, which the file's own
 * gui_navigation_transition_in_progress() comment already documents as
 * dead/never-set-true there -- leaving gui_shell.c's real player_swipe_
 * tracking true and player_swipe_ctx dangling after a compositor failure,
 * so the very next touch-poll tick (finger still down) called
 * slide_transition_anim_x_cb() again with a freed ctx. */
void gui_shell_player_swipe_recover(void * ctx);
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
