#ifndef DLNA_CONTROL_H
#define DLNA_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

/* DLNA/UPnP-AV MediaRenderer receive mode -- reuses the stock firmware's
 * own /usr/bin/dmrd binary (a customized build of the open-source
 * gmrender-resurrect project, confirmed via its embedded
 * "uuid:GMediaRender-1_0-000-000-002" string) for the actual SSDP
 * discovery/SOAP/UPnP-AV protocol handling, the same "reuse the stock
 * daemon, don't reimplement a whole protocol" approach as
 * airplay_control.h's shairport.
 *
 * dmrd doesn't decode or play audio itself -- confirmed by reading its
 * exported symbols (output_play/output_set_uri/output_set_meta/...) and by
 * live-capturing real traffic on a real device: it relays a small,
 * undocumented plain-text command protocol over a Unix domain socket at
 * /data/dmr_streamer to whatever companion process is listening there
 * (originally the stock closed-source hiby_player). This module IS that
 * companion now.
 *
 * Real-device investigation found dmrd itself has genuine bugs in this
 * particular build, independent of anything a companion implements:
 * Pause fails with a SOAP fault ("Transition not allowed") and
 * GetMute/SetMute fail with ("Missing action request argument
 * (CurrentMute)") -- both confirmed to error out inside dmrd's own SOAP
 * layer, never even reaching this module's socket. GetVolume's response
 * also doesn't reliably reflect a real value back to the controller,
 * silently, with no logged error. None of that is fixable from this side.
 * Scope here is deliberately just what was confirmed to work end-to-end:
 * casting a track (SetAVTransportURI + Play) plays it through this app's
 * own existing decoder/output pipeline (download-then-play, same as
 * subsonic_client.h's own reasoning -- dmrd hands over a plain HTTP URL,
 * not a raw stream), Stop stops it, and title/artist/album metadata are
 * shown. Seek was never reachable to test (the DLNA controller apps tried
 * had no way to scrub without a real position/duration being reported
 * back, which this module doesn't implement) and is not supported. */

/* Starts dmrd as a background daemon and this module's own dmr_streamer
 * listener thread. Idempotent -- safe to call again while already
 * running. */
void dlna_control_start(void);

/* Stops dmrd and this module's listener thread/socket. Idempotent. */
void dlna_control_stop(void);

/* Poll from the LVGL/main thread only (update_timer_cb), same "background
 * thread sets a flag, this is the one thread allowed to touch LVGL/audio
 * playback state" pattern as every other background-thread flag in this
 * codebase (hw_buttons, bt_media_player, the Subsonic download flag, ...).
 *
 * Returns true once when a cast track has finished downloading and is
 * ready to play -- *out_path is the local temp file path (caller should
 * pass it straight to audio_play_file_at(), then remove() it once no
 * longer needed the same way the Subsonic download flow already does).
 * *out_title, *out_artist, and *out_album are the DIDL-Lite metadata relayed
 * alongside the track (empty string if a given field wasn't provided).
 * All four buffers are caller-provided, plain fixed-size (no heap
 * ownership to manage). */
bool dlna_control_consume_ready_track(char * out_path, size_t path_size,
                                       char * out_title, size_t title_size,
                                       char * out_artist, size_t artist_size,
                                       char * out_album, size_t album_size);

/* Poll from the LVGL/main thread only. Returns true once when a Stop
 * command was relayed from the DLNA controller -- caller should call
 * audio_stop() and update the play button state itself (this module never
 * touches audio.c/LVGL directly, same layering as everywhere else). */
bool dlna_control_consume_stop_requested(void);

/* Push the current playback state so a GET state@ command (relayed from
 * dmrd's own UPnP AVTransport GetTransportInfo) can report the real
 * transport state back to the phone's DLNA controller, instead of the
 * hardcoded "STOPPED" this used to always reply with regardless of what was
 * actually happening -- real-device bug report: a cast played audio
 * correctly on the device but the phone's own UI never reflected it, since
 * it was polling this same state@ command to know. playing/paused should
 * mirror audio_is_playing()/audio_is_paused() exactly. Call once per tick
 * from update_timer_cb, same "background-thread-readable snapshot the main
 * thread refreshes" shape as remote_control_notify_status()/bt_media_
 * player.c's own status pushes -- cheap, and safe even when the listener
 * isn't running (the snapshot is just unread in that case). */
void dlna_control_notify_status(bool playing, bool paused);

#endif /* DLNA_CONTROL_H */
