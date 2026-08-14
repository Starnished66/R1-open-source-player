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

/* Opens (or reopens, if the format changed or the requested output target
 * -- see audio_output_set_bt_requested() below -- no longer matches
 * what's actually open) the output device for this channel count/sample
 * rate. Cheap to call repeatedly with the same values (no-ops). Returns
 * false if opening failed (device busy, aplay failed to spawn, etc). */
bool audio_output_ensure(unsigned int channels, unsigned int sample_rate);

/* Writes frames to whatever audio_output_ensure() last successfully
 * opened. Paced like a real device: blocks until there's room (local
 * hardware via tinyalsa's own blocking pcm_writei(), or backpressure from
 * aplay's own ALSA write on its end of the pipe for Bluetooth) -- or, if
 * nothing is actually open right now (a failed reopen, aplay having died
 * out from under a live connection), sleeps for approximately this
 * chunk's real playback duration instead of returning instantly, so a
 * caller's own decode loop can't race ahead of real time while output is
 * unavailable. Silently drops the chunk in that case -- callers don't
 * need their own fallback. */
void audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels);

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

#endif /* AUDIO_OUTPUT_H */
