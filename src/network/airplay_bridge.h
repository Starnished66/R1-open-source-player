#ifndef AIRPLAY_BRIDGE_H
#define AIRPLAY_BRIDGE_H

#include <stdbool.h>

/* Bridges shairport's own "-o pipe" output (see airplay_control.c/h) into
 * the shared audio_output module (see audio_output.h) -- the same
 * local-hardware-or-Bluetooth output audio.c's own playback thread uses --
 * so AirPlay receive mode actually produces sound instead of negotiating a
 * connection and then playing nothing.
 *
 * Real-device investigation: shairport was originally launched with
 * "-o ot", the stock firmware's own Ingenic-specific output backend.
 * Confirmed via `strings` against the actual stock shairport binary that
 * "-o ot" is a Unix-domain-socket CLIENT (audio_ot.c/socket_client.c)
 * expecting a SERVER at /var/run/airplay_socket_server -- and confirmed
 * via `strings` against the actual stock hiby_player binary (a genuinely
 * different firmware dump than the one this app replaces) that it, not
 * shairport, is what serves that socket. Since open_hiby_player fully
 * replaces hiby_player and never reimplemented that server, "-o ot"'s
 * connect() failed with ENOENT every time -- silently, since shairport
 * still fully completes mDNS advertisement and RTSP/RTP pairing
 * regardless, matching the exact reported symptom (device visible and
 * connectable, no sound). "-o pipe" is confirmed compiled into the same
 * binary (its own embedded help/error strings) and needs no protocol
 * beyond raw PCM bytes on a FIFO -- no server to reimplement, no wire
 * protocol to reverse-engineer.
 *
 * AirPlay 1/RAOP's audio format is fixed by the protocol itself (ALAC
 * decoded back to raw interleaved 16-bit signed-LE stereo PCM at 44100 Hz)
 * -- not a shairport choice, so not configurable here either. */

/* Named FIFO shairport's "-o pipe -- AIRPLAY_FIFO_PATH" writes raw PCM to.
 * airplay_control_start() (re)creates this node before each shairport
 * spawn; this bridge only ever opens it for reading. */
#define AIRPLAY_FIFO_PATH "/tmp/airplay_audio.fifo"

/* Starts the bridge thread in LISTENING state -- does NOT touch local
 * playback or the shared audio_output device yet. This only makes the
 * device ready to receive; it is deliberately decoupled from actually
 * being discoverable meaning "actively streaming" (real-device UX
 * decision: toggling AirPlay on should not interrupt whatever is already
 * playing locally, only an actual incoming stream should). The thread
 * itself waits for shairport to open the FIFO's write end AND start
 * sending real bytes -- only THEN does it call audio_stop() (see
 * airplay_bridge_is_streaming()) and claim the device, symmetrically with
 * how starting local playback while a stream is already active calls
 * airplay_control_disconnect_active_stream() (airplay_control.h) to give
 * local playback priority instead. Safe to call again while already
 * running (no-op, returns true). Re-opens the FIFO fresh after every
 * writer disconnect, going back to LISTENING (releasing audio_output --
 * see is_streaming()'s own comment) rather than ending the bridge, so
 * AirPlay stays discoverable across multiple connect/play/disconnect
 * cycles without needing the setting re-toggled.
 *
 * Returns false only if the thread itself could not be created
 * (pthread_create() failure) -- airplay_control_start() uses this to
 * decide whether it's worth spawning shairport at all (no reader, no
 * point). Real-device note: this is not a failure mode expected to ever
 * actually trigger on this target (thread creation failing implies the
 * system is already in serious trouble), but it's cheap to check and
 * avoids the specific "bridge_state stuck at RUNNING forever with no
 * thread to ever clear it" bug this guards against.
 *
 * Distinct, NOT covered by this return value: once the thread exists, it
 * still has to malloc() its own ~16KB PCM read buffer (bridge_thread_func())
 * before it can do anything -- that happens asynchronously, after this
 * function has already returned true. If it fails, the thread logs and
 * exits immediately back to BRIDGE_STOPPED with no reader ever having
 * existed, but airplay_control_start() has by then already used this
 * function's true return to decide it's worth spawning shairport, and may
 * already have done so. Not synchronized against on purpose: waiting here
 * for the new thread to confirm its own allocation succeeded would add a
 * handshake for a failure mode this target is not expected to ever actually
 * hit (this call site has no other allocation of this size fail in
 * practice); documented as a known gap rather than closed. */
bool airplay_bridge_start(void);

/* True only while shairport is actively delivering real PCM data right
 * now, not merely while AirPlay is enabled/discoverable -- see airplay_
 * bridge_start()'s own comment for why these are different things. Poll
 * from the LVGL/main thread to decide whether to show the AirPlay overlay
 * (gui_network.c's gui_network_poll_airplay_overlay()). Cheap, thread-safe. */
bool airplay_bridge_is_streaming(void);

/* Signals the bridge thread to stop and returns immediately -- does NOT
 * block until it has actually exited (unlike usb_dac_bridge_stop()'s own
 * bounded wait, which is safe only because its one caller already runs on
 * a background thread). Most airplay_control_stop() call sites run
 * directly on the UI/LVGL thread and don't need the output device back
 * synchronously, so this trades a strict exit guarantee for never blocking
 * the UI on those. The thread still closes its FIFO fd, calls audio_
 * output_close(), and clears its own running state shortly after this
 * returns. Safe to call when not running (no-op).
 *
 * The one caller that DOES need the device back synchronously (airplay_
 * control_disconnect_active_stream(), about to hand the device straight to
 * local playback) polls airplay_bridge_is_stopped() itself afterward
 * instead of this function blocking -- see that getter's own comment. */
void airplay_bridge_stop(void);

/* True once the bridge thread has fully stopped touching audio_output --
 * i.e. bridge_state has reached BRIDGE_STOPPED, which by construction only
 * happens after run_session() has already closed the device itself if it
 * had claimed it (see the "if (session_active)" block in run_session()).
 * airplay_bridge_stop() itself is fire-and-forget (see its own comment);
 * this lets a caller that specifically needs the device back -- airplay_
 * control_disconnect_active_stream(), which is about to hand it straight
 * to local playback -- wait for that with a short bounded poll instead of
 * racing the still-retiring bridge thread's last audio_output_write() call.
 * Same shape as run_session()'s own symmetric wait when AirPlay takes the
 * device the OTHER direction (waiting on audio_is_playing()/_is_paused()
 * to clear before claiming it). Cheap, thread-safe, safe to call anytime. */
bool airplay_bridge_is_stopped(void);

#endif /* AIRPLAY_BRIDGE_H */
