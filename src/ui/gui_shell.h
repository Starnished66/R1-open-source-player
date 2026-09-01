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
void gui_shell_install_indev_hooks(lv_indev_t * indev);

bool gui_shell_has_background_work(void);
void gui_shell_cancel_background_work(void);


lv_obj_t * gui_shell_get_status_bar_band(void);
lv_obj_t * gui_shell_get_home_indicator_band(void);
void gui_shell_set_status_bar_screen_context(lv_obj_t * screen);
void gui_shell_set_home_indicator_visible(bool visible);
