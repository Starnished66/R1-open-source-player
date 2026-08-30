#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "metadata.h"
#include "settings.h"
#include "audio.h"



void gui_player_init(uint32_t screen_width, uint32_t screen_height);
/* Deletes every screen/popup this module owns (including gui_track_info's,
 * via gui_track_info_teardown()) so gui_reload.c's in-process UI reload can
 * call gui_player_init() again from a clean slate. */
void gui_player_teardown(void);
void sync_player_topbar_visibility(lv_obj_t * screen);
void apply_track_metadata_to_ui(int index, track_metadata_t * out_meta);
void poll_cover_decode(void);
void gui_player_update_progress(void);
void gui_format_time(double seconds, char * buf, size_t buf_size);
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
/* Playback state semantic accessors and operations */
int gui_player_get_playlist_count(void);
int gui_player_get_playlist_index(void);
bool gui_player_has_active_track(void);
const char * gui_player_get_current_track_path(void);
const char * gui_player_get_track_path_at(int index);
int gui_player_get_queued_count(void);
const char * gui_player_get_queued_path_at(int offset);
void gui_player_queue_add(const char * path);
void gui_player_queue_remove_at(int offset);
void gui_player_queue_clear(void);
void gui_player_play_at(int index);
void gui_player_play_at_from(int index, double start_seconds);
void gui_player_step_manual(int direction);
lv_obj_t * gui_player_get_screen(void);
lv_obj_t * gui_player_get_cover_img(void);
/* Copies the currently decoded RGB565 cover for `for_index` into `out`.
 * Keeping ownership inside gui_player avoids exposing a mutable buffer that
 * is freed and replaced on track changes. */
bool gui_player_copy_cover_rgb565(int for_index, uint8_t * out, size_t out_size);
bool gui_player_is_seeking(void);

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
void reset_decoder_failure_tracking(void);
extern bool user_seeking;
extern bool deferred_resume_pending;
extern double deferred_resume_position;
bool build_saved_resume_playlist(char *** out_playlist, int * out_count, int * out_index);
bool install_saved_resume_playlist(char ** resume_playlist, int resume_count);
void prepare_deferred_resume(int index, double start_seconds);


int32_t gui_player_get_volume_percent(void);
void gui_player_set_volume_percent(int32_t percent);
const char * gui_player_get_now_playing_title(void);
const char * gui_player_get_now_playing_folder(void);


void gui_player_handle_auto_advance(void);
void gui_player_handle_track_finished(void);
void gui_player_handle_playback_error(audio_error_t err);
void gui_player_handle_playback_error_ex(audio_error_t err, uint64_t err_generation);
void gui_player_poll_confirmed_playback(void);

void gui_player_sync_topbar_visibility(lv_obj_t * screen);
lv_obj_t * gui_player_get_dismiss_btn(void);
