#pragma once
#include <lvgl/lvgl.h>

/* Screens owned by gui_settings */
extern lv_obj_t * settings_screen;
extern lv_obj_t * settings_playback_screen;
extern lv_obj_t * settings_display_screen;
extern lv_obj_t * settings_power_screen;
extern lv_obj_t * settings_system_screen;
extern lv_obj_t * about_screen;
extern lv_obj_t * accent_color_screen;
extern lv_obj_t * screen_timeout_screen;
extern lv_obj_t * startup_volume_screen;
extern lv_obj_t * sleep_timer_screen;
extern lv_obj_t * idle_shutdown_screen;
extern lv_obj_t * eq_screen;
extern lv_obj_t * eq_profiles_screen;

void gui_settings_init(void);
void show_font_size_reboot_popup(void);
void build_font_size_reboot_popup(void);

lv_obj_t * build_home_screen(void);
lv_obj_t * build_dac_home_screen(void);
