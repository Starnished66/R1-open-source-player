#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>
#include "metadata.h"
#include "settings.h"

extern lv_obj_t * player_screen;
extern lv_obj_t * player_dismiss_btn;
extern lv_obj_t * player_overlay_panel;
extern lv_obj_t * cover_img;
extern lv_obj_t * song_folder_label;
extern lv_obj_t * song_quality_label;
extern lv_obj_t * song_bitrate_label;
extern lv_obj_t * song_track_label;
extern lv_obj_t * song_count_label;
extern lv_obj_t * song_title_label;
extern lv_obj_t * format_badge_label;
extern lv_obj_t * play_mode_img;
extern lv_obj_t * favorite_icon;
extern lv_obj_t * play_btn;
extern lv_obj_t * prev_btn;
extern lv_obj_t * next_btn;
extern lv_obj_t * progress_slider;
extern lv_obj_t * progress_label;
extern lv_obj_t * duration_label;
extern lv_obj_t * volume_slider;

extern lv_obj_t * volume_popup;
extern lv_obj_t * volume_popup_backdrop;
extern lv_obj_t * more_menu_popup;
extern lv_obj_t * more_menu_popup_backdrop;

void gui_player_init(uint32_t screen_width, uint32_t screen_height);
void sync_player_topbar_visibility(lv_obj_t * screen);
void apply_track_metadata_to_ui(int index, track_metadata_t * out_meta);
void poll_cover_decode(void);
void gui_player_update_progress(void);
void show_volume_popup(int32_t percent);
void hide_volume_popup(void);
void poll_volume_popup_timeout(void);
void refresh_play_btn_icon(void);
void refresh_format_badge(void);
void set_play_button_state(bool is_playing);
void hide_more_menu_popup(void);

void configure_native_slider_rail(lv_obj_t * slider);
void cycle_play_mode(void);
void resolve_replaygain(const track_metadata_t * meta, bool * out_has_gain, double * out_gain_db, bool * out_has_peak, double * out_peak);

void favorite_icon_event_cb(lv_event_t * e);
