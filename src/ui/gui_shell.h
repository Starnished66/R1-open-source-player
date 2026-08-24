#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

extern lv_obj_t * home_screen;
extern lv_obj_t * dac_home_screen;
extern lv_obj_t * status_bar_band;
extern lv_obj_t * home_indicator_band;

extern lv_obj_t * quick_drawer_title_label;
extern lv_obj_t * quick_drawer_artist_label;
extern lv_obj_t * quick_drawer_favorite_icon;
extern lv_obj_t * quick_drawer_play_btn;
extern lv_obj_t * quick_drawer_order_icon;

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
