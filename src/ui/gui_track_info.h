#ifndef GUI_TRACK_INFO_H
#define GUI_TRACK_INFO_H

#include "audio.h"
#include <lvgl/lvgl.h>
#include <stdbool.h>

typedef enum {
    GUI_TRACK_SOURCE_LOCAL = 0,
    GUI_TRACK_SOURCE_SUBSONIC,
    GUI_TRACK_SOURCE_PLUGIN,
    GUI_TRACK_SOURCE_RADIO,
} gui_track_source_t;

/* Owned, bounded copy of the metadata already available when a track is
 * selected. This deliberately contains no stream URL: signed/authenticated
 * provider URLs must never appear on the Information screen. */
typedef struct {
    char path[2048];
    gui_track_source_t source;
    char container[16];
    char provider[64];
    char track_id[128];
    audio_codec_t declared_codec;
    unsigned int declared_sample_rate;
    unsigned int declared_bit_depth;
    unsigned int declared_channels;
    unsigned int declared_bitrate_kbps;
    double declared_duration_seconds;
    int track_number;
    int disc_number;
    bool has_track_number;
    bool has_disc_number;
    int replaygain_mode;
    bool has_replaygain_track;
    double replaygain_track_db;
    bool has_replaygain_album;
    double replaygain_album_db;
} gui_track_info_context_t;

void gui_track_info_init(void);
/* Deletes the Information screen and nulls the static it's guarded on, so
 * gui_track_info_init() can rebuild it after gui_reload.c's in-process UI
 * reload instead of silently no-opping. */
void gui_track_info_teardown(void);
lv_obj_t * gui_track_info_get_screen(void);
void gui_track_info_set_current(const gui_track_info_context_t * context);
void gui_track_info_open(void);
void gui_track_info_poll(void);

#endif
