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

/* Screens owned by gui_library */
extern lv_obj_t * music_screen;
extern lv_obj_t * files_screen;
extern lv_obj_t * files_list;
extern lv_obj_t * files_title_label;
extern lv_obj_t * files_search_list;
extern lv_obj_t * files_search_title_label;

extern lv_obj_t * all_songs_screen;
extern lv_obj_t * all_songs_list;
extern lv_obj_t * all_songs_title_label;

extern lv_obj_t * recently_added_screen;
extern lv_obj_t * recently_added_list;
extern lv_obj_t * recently_added_title_label;

extern lv_obj_t * artists_screen;
extern lv_obj_t * artists_list;
extern lv_obj_t * artists_title_label;

extern lv_obj_t * albums_screen;
extern lv_obj_t * albums_list;
extern lv_obj_t * albums_title_label;

extern lv_obj_t * album_artist_screen;
extern lv_obj_t * album_artist_list;
extern lv_obj_t * album_artist_title_label;

extern lv_obj_t * group_songs_screen;
extern lv_obj_t * group_songs_list;
extern lv_obj_t * group_songs_title_label;

extern lv_obj_t * artist_albums_screen;
extern lv_obj_t * artist_albums_list;
extern lv_obj_t * artist_albums_title_label;

extern lv_obj_t * playlists_screen;
extern lv_obj_t * playlists_list;
extern lv_obj_t * playlists_title_label;

extern lv_obj_t * cue_tracks_screen;
extern lv_obj_t * cue_tracks_list;
extern lv_obj_t * cue_tracks_title_label;

extern lv_obj_t * add_to_playlist_screen;
extern lv_obj_t * add_to_playlist_list;

void gui_library_init(void);

void start_library_rescan(void);
void poll_library_rescan(void);
void poll_sd_format(void);
void album_thumbnail_generation_poll(void);

void open_add_to_playlist_for(const char * path);
void on_cue_file_selected(const char * cue_path);
void show_artist_albums(const char * name, metadata_db_group_kind_t kind);
void refresh_library_screens_after_rescan(void);

extern lv_timer_t * az_index_drag_timer;
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

extern bool library_rescan_active;
extern bool library_rescan_success_pending;
extern bool album_thumbnail_active;

void gui_library_resume_fast_timers(void);
