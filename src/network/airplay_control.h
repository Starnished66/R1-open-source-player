#ifndef AIRPLAY_CONTROL_H
#define AIRPLAY_CONTROL_H

#include <stdbool.h>

/* AirPlay (1) receive mode via the stock firmware's /usr/bin/shairport
 * binary. "-M" sets the metadata/cover-art dump directory, "-b 160" is the
 * playback buffer fill threshold in frames, "-l" redirects shairport's own
 * stdout/stderr to a real log file (this app's own subprocess_spawn_daemon()
 * otherwise sends both to /dev/null, leaving no way to diagnose a shairport-
 * side failure). shairport has its own embedded mDNS responder
 * (tinysvcmdns), so no separate Avahi dependency is needed.
 *
 * Real-device investigation: this used to pass "-o ot", the stock
 * firmware's own Ingenic-specific output backend -- confirmed via `strings`
 * against the actual binary to be a Unix-domain-socket CLIENT expecting a
 * SERVER at /var/run/airplay_socket_server that only the stock hiby_player
 * binary (which this app fully replaces) ever provided. AirPlay would
 * negotiate and connect completely normally (mDNS + RTSP/RTP pairing don't
 * touch this at all) and then emit no audio at all, silently. Switched to
 * "-o pipe -- AIRPLAY_FIFO_PATH" (also confirmed compiled into the same
 * binary) plus airplay_bridge.c/h, a small in-process thread that reads raw
 * PCM off that FIFO and writes it through this app's own audio_output --
 * the same shape already used for USB DAC input (see usb_dac_bridge.c). See
 * airplay_bridge.h for the full investigation writeup. */

/* Starts shairport as a background daemon, advertised under `device_name`,
 * and starts the FIFO bridge (airplay_bridge_start()) that actually
 * produces sound from it. (Re)creates AIRPLAY_FIFO_PATH first so shairport
 * always finds a fresh FIFO node to open, regardless of what a previous
 * run left behind. Blocking only for the brief process-spawn itself; call
 * off the UI thread isn't required but kept consistent with everything
 * else here.
 *
 * Transactional with respect to everything THIS function controls directly:
 * if mkfifo() or airplay_bridge_start() fails, shairport is never spawned at
 * all (no point advertising a device that can't produce sound) and this
 * returns false without touching any of the module's running state.
 * airplay_metadata_start() failing is treated as non-fatal (logged only,
 * startup proceeds) since audio still works with no track info displayed.
 * If shairport itself then fails to spawn, whichever of the bridge/metadata
 * threads did start is stopped again before returning false, so a failed
 * call never leaves background threads running with nothing to feed them.
 *
 * NOT covered: airplay_bridge_start() returning true only means its thread
 * was successfully created -- the thread's own ~16KB PCM buffer allocation
 * happens after that, asynchronously, and this function has already
 * returned true and let shairport spawn by the time that could fail. See
 * airplay_bridge_start()'s own comment for why that's accepted as a real,
 * if vanishingly unlikely on this target, gap rather than something worth
 * synchronizing this call on. */
bool airplay_control_start(const char * device_name);

/* Kills shairport, stops the bridge thread, and removes the FIFO node. */
void airplay_control_stop(void);

/* True only across a fully successful airplay_control_start() until the
 * next airplay_control_stop() -- lets a caller that isn't itself the toggle
 * screen (which already reacts to airplay_control_start()'s own return
 * value) notice on its own that a respawn failed. Needed because airplay_
 * control_disconnect_active_stream() below ignores airplay_control_start()'s
 * return value internally (see its own comment): this is how the UI layer
 * can still detect that case and self-correct current_settings.wifi_dac_
 * mode_enabled, without this network-layer module needing to reach into
 * settings/UI code itself. */
bool airplay_control_is_active(void);

/* Real-device UX decision: AirPlay being enabled/discoverable and AirPlay
 * actively streaming are different things (see airplay_bridge.h's own
 * comment on airplay_bridge_start()) -- toggling AirPlay on must not
 * interrupt music already playing locally, only a genuine incoming stream
 * should, and symmetrically, the user starting local playback while a
 * phone is already actively streaming should win and disconnect that
 * stream rather than being silently blocked or fighting it for the shared
 * output device. No-op if AirPlay isn't currently streaming (checked via
 * airplay_bridge_is_streaming()).
 *
 * Implemented as kill-and-immediately-respawn shairport rather than trying
 * to gracefully end just the one RTSP session while leaving the process
 * running: shairport has no command channel of its own to ask it to drop
 * only the active connection (confirmed via `strings` -- no signal
 * handling, no control socket beyond the audio/metadata FIFOs), and simply
 * stopping this app's OWN FIFO reader out from under a still-running
 * shairport would leave it blocked mid-write (or eventually SIGPIPE'd)
 * rather than cleanly torn down. A fresh respawn is fast (sub-second, same
 * fork+exec this app already relies on everywhere else) and leaves AirPlay
 * fully discoverable again for the phone's next attempt, same as if
 * nothing had happened from the user's perspective except their local
 * track starting to play. Call from wherever local playback is actually
 * (re)started (gui_player.c's play_track_at_from_internal()), not from
 * every possible caller of that function individually.
 *
 * Returns true only if a stream was actually found active and disconnected
 * (i.e. airplay_bridge_is_streaming() was true when this was called) -- the
 * caller uses this to tell whether local playback was just displaced by
 * AirPlay taking over the shared output device and therefore needs to be
 * resumed explicitly, versus the common case of no stream having been
 * active at all. Deliberately does NOT reflect whether the respawn itself
 * (the airplay_control_start() half of kill-and-respawn) actually
 * succeeded: local playback resuming is correct either way, and this
 * function's caller (gui_player.c) has no business touching current_
 * settings.wifi_dac_mode_enabled or showing a toast about a module it
 * doesn't otherwise know about. If the respawn fails, airplay_control_
 * is_active() above will report it, and gui_network.c's per-tick overlay
 * poll (gui_network_poll_airplay_overlay()) is what actually notices and
 * corrects the setting. */
bool airplay_control_disconnect_active_stream(void);

#endif /* AIRPLAY_CONTROL_H */
