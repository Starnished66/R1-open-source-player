#pragma once
#include <lvgl/lvgl.h>

typedef struct {
    char url[1536]; /* the exact playlist[i] this entry describes */
    char title[128];
    char artist[128];
    char album[128];
    char cover_url[1536];
    bool verify_tls;
    char suffix[16];
    int track;
    int disc;
    int duration_seconds;
    unsigned int sample_rate;
    unsigned int bit_depth;
    unsigned int channels;
    unsigned int bitrate_kbps;
} subsonic_stream_song_meta_t;


void gui_subsonic_init(void);
/* Deletes every screen this module owns so gui_reload.c's in-process UI
 * reload can call gui_subsonic_init() again from a clean slate. */
void gui_subsonic_teardown(void);
extern subsonic_stream_song_meta_t * subsonic_stream_meta;
extern int subsonic_stream_meta_count;
extern bool subsonic_library_download_active;
extern bool subsonic_connect_active;
extern void subsonic_tile_cb(lv_event_t * e);
lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list);


extern subsonic_stream_song_meta_t * subsonic_stream_meta;

void poll_subsonic_download(void);
void poll_subsonic_library_download(void);
void poll_subsonic_connect(void);
void poll_subsonic_browse(void);

bool gui_subsonic_has_background_work(void);
void gui_subsonic_cancel_background_work(void);
