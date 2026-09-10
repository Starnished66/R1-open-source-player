#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Shared PCM output device for the target build -- local hardware (via tinyalsa),
 * a connected Bluetooth accessory, or USB DAC (via piped aplay). Used by both
 * audio.c's playback thread and usb_dac_bridge.c's USB DAC bridge thread.
 *
 * The implementation enforces one owner thread from a successful ensure() until
 * its close(). Calls to ensure()/write()/close() from another thread fail or
 * no-op immediately (writes return false with zero frames), including when the
 * owner is blocked in device I/O; this keeps a timed-out USB DAC bridge writer
 * from sharing or destroying handles used by a later playback owner. Any failed
 * ensure() releases its reservation so another caller can retry. The owner
 * must close before exiting its thread. Route setters and state queries may
 * still be called from other threads; they never wait for device I/O.
 *
 * Target build only; host simulator uses SDL. */

/* Opens (or reopens) the output device for the given format, target, and latency mode.
 * Returns false if opening failed.
 * If another thread currently owns the output, returns false immediately.
 *
 * low_latency configures smaller period/buffer sizes on the local tinyalsa path
 * (e.g. for real-time sources like AirPlay). */
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
 * A call from a thread that does not own the current ensure() reservation
 * returns false immediately with zero frames.
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
 * so the next audio_output_ensure() call always does a fresh open. A call from
 * a non-owner thread is a no-op. */
void audio_output_close(void);

/* Routes subsequent audio_output_ensure()/_write() calls to a connected
 * Bluetooth accessory instead of local hardware. Takes effect on the next
 * audio_output_ensure() call. */
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

/* Writes the codec's hardware attenuation registers ("Left"/"Right Playback Volume",
 * raw 0-255) via tinyalsa. The control is write-only as driver reads do not reflect
 * written values; callers track written values independently if needed. */
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

/* True only while audio_output_ensure() actually has a USB audio device open.
 * Used to apply digital PCM gain for USB output (which bypasses the local codec mixer).
 * Bluetooth volume is handled separately via AVRCP absolute volume rather than PCM gain. */
bool audio_output_is_usb_active(void);

#endif /* AUDIO_OUTPUT_H */
