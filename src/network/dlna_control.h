#ifndef DLNA_CONTROL_H
#define DLNA_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

/* DLNA/UPnP-AV MediaRenderer receive mode.
 *
 * Uses the stock /usr/bin/dmrd daemon for SSDP discovery and UPnP-AV protocol
 * handling. dmrd relays commands over a Unix domain socket at
 * /data/dmr_streamer to this companion module.
 *
 * Casting a track (SetAVTransportURI + Play) downloads the media via HTTP and
 * plays it through the local audio pipeline. Stop halts playback, and track
 * title, artist, and album metadata are extracted from DIDL-Lite. */

/* Starts dmrd as a background daemon and this module's own dmr_streamer
 * listener thread. Idempotent -- safe to call again while already
 * running. */
void dlna_control_start(void);

/* Stops dmrd and this module's listener thread/socket. Idempotent. */
void dlna_control_stop(void);

/* Poll from the LVGL/main thread only (update_timer_cb).
 *
 * Returns true once when a cast track has finished downloading and is ready to
 * play. Populates caller-provided buffers with the local file path and DIDL-Lite
 * metadata (empty string if omitted). */
bool dlna_control_consume_ready_track(char * out_path, size_t path_size,
                                       char * out_title, size_t title_size,
                                       char * out_artist, size_t artist_size,
                                       char * out_album, size_t album_size);

/* Poll from the LVGL/main thread only. Returns true once when a Stop command
 * was relayed from the DLNA controller. */
bool dlna_control_consume_stop_requested(void);

/* Push the current playback state so GET state@ queries report the active
 * transport state to the DLNA controller. Call once per tick from
 * update_timer_cb. */
void dlna_control_notify_status(bool playing, bool paused);

#endif /* DLNA_CONTROL_H */
