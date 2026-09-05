#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Shared PCM output device for the target build -- local hardware (via
 * tinyalsa) or a connected Bluetooth accessory (via a piped `aplay -D
 * bluealsa`, see audio_output.c's own top-of-file comment for why), used
 * by BOTH audio.c's playback thread and usb_dac_bridge.c's USB DAC
 * bridge thread. Extracted out of audio.c (where this logic originated,
 * fixing several real-device Bluetooth-output bugs one at a time -- see
 * git history / the comments still in audio_output.c) once
 * usb_dac_bridge.c needed the exact same routing/pacing/failure-handling
 * behavior for its own, separate output stream: re-deriving it a second
 * time risked quietly missing one of those already-fixed edge cases.
 *
 * Safe to share despite being plain file-scope state, not because it's
 * thread-safe for concurrent use, but because it never needs to be: only
 * one of the two callers is ever actually running at a time
 * (usb_dac_bridge_start() calls audio_stop() before touching this, and
 * usb_mode_control_apply() tears down DAC mode, including this bridge,
 * before any other mode's playback could resume) -- the same "single
 * owner at any given moment" shape audio.c's own playback thread already
 * relied on before this was ever shared.
 *
 * No HOST_BUILD variant: the host simulator has its own separate SDL
 * audio path (still directly inside audio.c, since usb_dac_bridge.c's
 * whole premise -- a real /dev/uac_sa USB gadget node -- doesn't exist on
 * a host build either). Callers on the target build only. */

/* Opens (or reopens, if the format, low_latency mode, or the requested
 * output target -- see audio_output_set_bt_requested() below -- no longer
 * matches what's actually open) the output device for this channel count/
 * sample rate. Cheap to call repeatedly with the same values (no-ops).
 * Returns false if opening failed (device busy, aplay failed to spawn, etc).
 *
 * low_latency only affects the OUTPUT_TARGET_LOCAL tinyalsa path (BT/USB go
 * through aplay's own ALSA config, unaffected either way) -- see audio_
 * output.c's open_device() for the actual period/buffer values. Real-time
 * PCM sources with no local decoder buffering ahead of audio_output_write()
 * may want true (AirPlay's own bridge does, having no decoder timing to
 * lean on); decoder-fed local file playback, and anything not yet verified
 * safe under its own live-device underrun testing (USB-DAC receive, as of
 * this writing), should pass false. Passed as a parameter rather than a
 * separate setter call specifically to close that one gap: a setter call
 * followed by a separate ensure() call left a window where a different,
 * concurrently-running caller's own setter call could land in between and
 * flip the mode this call ends up opening with. This does NOT make
 * audio_output_ensure() itself, or the shared output state it reads/writes,
 * safe under concurrent callers in general -- there is still no lock here
 * (see this file's own top comment on the "one owner at a time" convention
 * every caller is expected to honor instead). That's pre-existing
 * architectural debt this parameter doesn't attempt to fix, just one
 * specific, previously-avoidable gap within it. */
bool audio_output_ensure(unsigned int channels, unsigned int sample_rate, bool low_latency, bool want_s24);

/* Writes frames to whatever audio_output_ensure() last successfully opened.
 * Blocks until delivery completes (tinyalsa pcm_writei() for local hardware;
 * pipe back-pressure from aplay's ALSA write for BT/USB).
 *
 * If out_frames_written is non-NULL, stores the exact number of frames
 * successfully delivered before any error occurred (allowing callers to
 * retry only the remaining undelivered suffix without duplicating audio).
 *
 * Returns true if ALL requested frames were successfully delivered.
 * Returns false on partial delivery, write error, or if no device is open.
 *
 * When nothing is open, sleeps for the chunk's nominal playback duration and
 * returns false with *out_frames_written = 0. */
bool audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels, uint64_t * out_frames_written);

/* Writes 32-bit frames (right-justified in low 24-bits, PCM_FORMAT_S24_LE) to local hardware.
 * Only valid when active_target == OUTPUT_TARGET_LOCAL and tinyalsa opened in S24_LE.
 * Returns false immediately if called while active_target != OUTPUT_TARGET_LOCAL. */
bool audio_output_write_s24(const int32_t * buf, uint64_t frames, unsigned int channels, uint64_t * out_frames_written);

/* Returns true if OUTPUT_TARGET_LOCAL is the currently requested output target
 * (neither Bluetooth nor USB DAC output is requested). */
bool audio_output_is_local_requested(void);

/* Ground truth for whether the device open RIGHT NOW is actually local
 * hardware running at PCM_FORMAT_S24_LE -- not merely what a caller last
 * asked for. A caller that requested want_s24=true via audio_output_ensure()
 * can still end up here false: the route may have changed to Bluetooth/USB
 * between the request and this check (want_s24 is silently irrelevant to
 * those paths), or hw_params negotiation for S24_LE may have failed and
 * open_device() fell back to S16_LE (see its own comment). Callers that
 * decoded/processed a chunk assuming the wide path must re-check this
 * AFTER audio_output_ensure() returns and before choosing which write
 * function to call -- audio_output_write_s24() unconditionally refuses to
 * write while the active target isn't local, so trusting a pre-negotiation
 * prediction instead of this can turn a route change or a negotiation
 * failure into a hard write failure that aborts playback. */
bool audio_output_is_s24_active(void);

/* Clears any remembered "S24_LE hw_params negotiation failed at this
 * (channels, rate)" record (see audio_output.c's s24_unsupported_known for
 * why one exists and why it's deliberately not permanent). Call once per
 * new track becoming current -- audio.c does this from
 * publish_current_format_locked(), the single point every "a different
 * decoder is now the current one" transition already goes through (initial
 * open, gapless handoff, crossfade promotion). Safe/cheap to call even when
 * nothing was cached. */
void audio_output_reset_s24_probe(void);

/* Closes whatever's open (local or Bluetooth) and resets format tracking,
 * so the next audio_output_ensure() call always does a fresh open. */
void audio_output_close(void);

/* Routes subsequent audio_output_ensure()/_write() calls to a connected
 * Bluetooth accessory instead of local hardware, or back again -- see
 * audio_set_bt_output()'s own doc comment in audio.h (the original,
 * still-current call site: gui.c's poll_refresh_bt_icon(), mirroring
 * Bluetooth connection state) for the real-device history behind this.
 * usb_dac_bridge.c's own USB-DAC-mode output should be routed by the
 * exact same signal -- see usb_dac_bridge_set_bt_output() in
 * usb_dac_bridge.h, called from the same gui.c call site. Only takes
 * effect on the next audio_output_ensure() call, same lazy-reopen
 * behavior as a format change. */
void audio_output_set_bt_requested(bool requested);

/* Routes subsequent audio_output_ensure()/_write() calls to an external
 * USB audio device (DAC/amp) instead of local hardware, or back again --
 * mirrors audio_output_set_bt_requested() above exactly, just targeting a
 * resolved ALSA device string (from usb_audio_output_is_connected(),
 * usb_audio_output.h) instead of the fixed "bluealsa" literal. Takes
 * priority over Bluetooth if both are somehow requested at once (see
 * audio_output.c's open_device()) -- a user physically plugging something
 * in is a more deliberate, more recent signal than an already-standing
 * Bluetooth connection. alsa_device is copied internally (safe to pass a
 * stack buffer); ignored when requested is false. Only takes effect on
 * the next audio_output_ensure() call, same lazy-reopen behavior as a
 * format change. */
void audio_output_set_usb_requested(bool requested, const char * alsa_device);

/* Writes the codec's own hardware attenuation registers ("Left"/"Right
 * Playback Volume", raw 0-255) directly via tinyalsa's mixer API -- see
 * audio.c's volume_set_hw_raw() doc comment for the full real-device
 * investigation behind this (why it exists, what raw 0 vs increasing raw
 * values do, and the big caveat below).
 *
 * Real-device finding: this control's own .get() callback is broken -- it
 * always reports 0 regardless of what was last written, confirmed live
 * (write a non-zero value, read it back in the very same shell invocation,
 * still 0), even though the write demonstrably reaches the hardware and
 * changes real output level (also confirmed live, ear-to-speaker). This
 * function therefore never reads the control back to verify -- there is
 * nothing meaningful to read. Callers must track their own last-written
 * value if they need it. Safe to call from any thread/frequency; opens a
 * lazy, process-lifetime tinyalsa mixer handle on first use. */
void audio_output_set_hw_volume_raw(int raw_left, int raw_right);

/* R3 Pro II output-port routing. Picks a route (3.5mm headset vs. 4.4mm
 * balanced, headphone_status.h deciding which)
 * and writes it to the "Output Port Switch" mixer control -- a control
 * name and value scheme ported from a separate, already-working player for
 * this same hardware, not a guess. A genuine no-op on R1 and on host, since
 * neither switch_dev node nor this mixer control exist there. Safe to call
 * from any thread/frequency: the actual mixer write happens on the same
 * dedicated worker thread audio_output_set_hw_volume_raw() above already
 * uses, not synchronously on the caller's own thread. */
void audio_output_sync_balanced_output(void);

/* Coalesces hardware-volume writes on a dedicated process-lifetime worker,
 * keeping mixer I/O out of LVGL and playback callbacks. */
void audio_output_request_hw_volume_raw(int raw_left, int raw_right);

/* True only while audio_output_ensure() actually has a USB audio device
 * open (not local, not Bluetooth) -- audio.c's audio_set_volume() uses
 * this to fall back to real digital PCM gain for USB specifically, since
 * audio_output_set_hw_volume_raw() above is a no-op for it (USB PCM is
 * piped to a separate `aplay -D plughw:<card>,0` process, never touches
 * this device's own card-0 mixer -- see audio_output.c's own architecture
 * comment). Deliberately NOT also true for Bluetooth: BT volume is already
 * handled by a completely separate, working mechanism (AVRCP absolute
 * volume pushed to the connected accessory, bluetooth_control.c's
 * bt_source_vol_sync_thread_func()) that has nothing to do with this app's
 * own PCM gain -- applying digital gain there too was tried and reverted
 * after a real-device bug report of double-attenuated (too quiet)
 * Bluetooth audio. Reflects active_target, not requested_target -- what's
 * actually open right now, not merely asked for. */
bool audio_output_is_usb_active(void);

#endif /* AUDIO_OUTPUT_H */
