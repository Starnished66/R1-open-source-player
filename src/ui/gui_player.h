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
extern lv_obj_t * song_title_label;
extern lv_obj_t * favorite_icon;
extern lv_obj_t * prev_btn;
extern lv_obj_t * next_btn;
extern lv_obj_t * progress_slider;
extern lv_obj_t * progress_label;
extern lv_obj_t * volume_slider;

extern lv_obj_t * volume_popup;

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
/* Playback state accessors and mutators */
extern char ** playlist;
extern int playlist_count;
extern int playlist_index;
extern int * playlist_lazy_sort_order;
extern char now_playing_path[600];
extern int queued_pending_count;

const char * playlist_path_at(int index);
void free_playlist(void);
void on_file_selected(char ** new_playlist, int count, int selected_index);
void on_file_selected_at(char ** new_playlist, int count, int selected_index, double start_seconds);
void on_file_selected_lazy_all_songs(int selected_index);
void on_file_selected_lazy_recently_added(int selected_index);
void on_file_browser_selected(char ** new_playlist, int count, int selected_index);
void on_track_auto_advanced(int index);
void play_track_at(int index);
void play_track_at_from(int index, double start_seconds);
void toggle_play_pause(void);
int compute_manual_step_index(int index, int direction);
void arm_next_track_for_audio(int current_index);
void commit_auto_advance(void);

void queue_add_song(const char * path);
void queue_remove_song_at_offset(int offset);
void queue_clear_pending(void);

void clear_player_source(void);
void set_player_source_file_browser(const char * dir, int row);
void set_player_source_all_songs(int selected_index);
void set_player_source_recently_added(int selected_index);
struct group_song_entry_s;
typedef struct group_song_entry_s group_song_entry_t;
void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index);

bool gui_player_has_background_work(void);
void gui_player_cancel_background_work(void);


int compute_auto_advance_index(int index);
extern bool user_seeking;
extern bool deferred_resume_pending;
extern double deferred_resume_position;
bool build_saved_resume_playlist(char *** out_playlist, int * out_count, int * out_index);
void install_saved_resume_playlist(char ** resume_playlist, int resume_count);
void prepare_deferred_resume(int index, double start_seconds);


