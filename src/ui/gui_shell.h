#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

lv_obj_t * gui_shell_get_home_screen(void);
lv_obj_t * gui_shell_get_dac_home_screen(void);

void gui_shell_update_quick_drawer_track(const char * title, const char * artist);
void gui_shell_update_quick_drawer_favorite(bool is_favorite);
void gui_shell_update_quick_drawer_play_state(bool is_playing);
void gui_shell_update_quick_drawer_play_mode(int mode);

void gui_shell_init(uint32_t screen_width, uint32_t screen_height);
void refresh_clock_label(void);
void refresh_battery_topbar(void);
void refresh_wifi_topbar(void);
void refresh_volume_topbar(int32_t percent);
void quick_drawer_mark_snapshot_dirty(void);
void register_swipe_dead_zone(lv_obj_t * obj);
void unregister_swipe_dead_zone(lv_obj_t * obj);
void poll_quick_drawer(void);
void open_quick_drawer(void);
void close_quick_drawer(void);

extern bool refresh_bt_icon_result_a2dp_connected;
extern bool wifi_toggle_active;
extern bool bt_toggle_active;

void gui_shell_poll(void);
void refresh_quick_drawer_crossfade_icon(void);
void gui_shell_resume_connections(bool wifi_was_on, bool bt_was_on);

void gui_shell_suspend_connections(bool * wifi_was_on, bool * bt_was_on);
void refresh_headphone_icon(void);

void gui_shell_update_topbar(bool screen_just_woke);

bool point_in_swipe_dead_zone(lv_point_t p);
bool active_press_is_over_drag_adjust_widget(void);
void gui_shell_resume_fast_timers(void);

bool gui_shell_has_background_work(void);
void gui_shell_cancel_background_work(void);


lv_obj_t * gui_shell_get_status_bar_band(void);
lv_obj_t * gui_shell_get_home_indicator_band(void);
void gui_shell_set_home_indicator_visible(bool visible);
