#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "metadata_db.h"

typedef struct group_song_entry_s {
    char * path;
    char * title;
} group_song_entry_t;

void free_group_song_entries(group_song_entry_t * entries, int count);
bool copy_group_song_entries(group_song_entry_t ** out, const group_song_entry_t * entries, int count);
void show_group_songs(const char * title, const group_song_entry_t * entries, int count);

/* Screens accessors */
lv_obj_t * gui_library_get_music_screen(void);
lv_obj_t * gui_library_get_files_screen(void);
lv_obj_t * gui_library_get_all_songs_screen(void);
lv_obj_t * gui_library_get_recently_added_screen(void);
lv_obj_t * gui_library_get_artists_screen(void);
lv_obj_t * gui_library_get_albums_screen(void);
lv_obj_t * gui_library_get_album_artist_screen(void);
lv_obj_t * gui_library_get_group_songs_screen(void);
lv_obj_t * gui_library_get_playlists_screen(void);

void gui_library_init(void);

void start_library_rescan(void);
void poll_library_rescan(void);
void poll_sd_format(void);
void album_thumbnail_generation_poll(void);

void open_add_to_playlist_for(const char * path);
void on_cue_file_selected(const char * cue_path);
void show_artist_albums(const char * name, metadata_db_group_kind_t kind);
void refresh_library_screens_after_rescan(void);

void poll_az_index_drag(lv_timer_t * timer);
void plugin_stream_tile_click_cb(lv_event_t * e);
void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index);

bool search_close_if_active_for_screen(lv_obj_t * screen);
void refresh_now_playing_indicators(void);

void more_menu_list_cb(lv_event_t * e);

void poll_search_job(void);
void play_remote_control_song(const char * song_path, const char * playlist_name, const char * artist_filter,
                              const char * album_artist_filter, const char * album_filter);
void start_power_off_countdown(void);
void poll_power_off_countdown(void);
void build_power_off_countdown_popup(void);
void poll_sd_card_hotplug(void);

void gui_library_resume_fast_timers(void);
void gui_library_reset_drag_state(void);

bool gui_library_has_background_work(void);
void gui_library_cancel_background_work(void);
