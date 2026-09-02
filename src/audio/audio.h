#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "decoder_result.h"

typedef enum {
    AUDIO_CODEC_UNKNOWN = 0,
    AUDIO_CODEC_FLAC,
    AUDIO_CODEC_MP3,
    AUDIO_CODEC_PCM,
    AUDIO_CODEC_DSD,
    AUDIO_CODEC_AAC,
    AUDIO_CODEC_ALAC,
    AUDIO_CODEC_APE,
    AUDIO_CODEC_WMA,
    AUDIO_CODEC_OPUS,
    AUDIO_CODEC_VORBIS,
} audio_codec_t;

/* Immutable snapshot of the decoder currently feeding playback. The source
 * fields describe the encoded media; the output fields describe what this
 * player actually hands to ALSA/SDL after decoding. source_bit_depth is 0
 * when a lossy codec has no meaningful bit depth or the provider did not
 * declare one. path is copied under audio.c's mutex so callers never borrow
 * active_path or a live decoder pointer. */
typedef struct {
    bool valid;
    char path[2048];
    audio_codec_t codec;
    unsigned int source_sample_rate;
    unsigned int source_bit_depth;
    unsigned int output_sample_rate;
    unsigned int output_bit_depth;
    unsigned int channels;
    unsigned int bitrate_kbps;
    double duration_seconds;
    bool is_stream;
    bool is_dsd;
    bool replaygain_applied;
    double replaygain_applied_db;
    uint64_t generation;
} audio_current_format_info_t;

/* One-time setup of the audio backend (SDL2 audio on host, ALSA/tinyalsa on
 * target) and the single playback thread, which lives for the app's whole
 * lifetime (see audio.c) rather than being recreated per track -- that's
 * what lets gapless/crossfade transitions avoid tearing down the output
 * device between tracks. Safe to call more than once; only the first call
 * does anything. */
void audio_init(void);

/* Interrupts whatever's currently playing (if anything) and immediately
 * starts decoding/playing path from start_seconds, with no fade -- this is
 * for explicit user actions (tapping a track, prev/next, initial pick), as
 * opposed to the audio thread's own automatic advance into a queued next
 * track (see audio_set_next_track()), which can gapless-handoff or
 * crossfade instead. Format is picked from the file extension -- except a
 * path starting with "http://" or "https://", which is instead opened as a
 * live network stream (internet radio) and always decoded as MP3
 * regardless of what the URL looks like, the only decoder with a
 * callback-based streaming API wired up so far (see decoder_open() in
 * audio.c). A live stream reports a duration of 0 (see
 * audio_get_duration_seconds()), can't be seeked, and never reaches true
 * EOF, so auto-advance and gapless/crossfade into/out of it never engage --
 * all by construction of total_frames staying 0, not a special case
 * elsewhere. start_seconds is ignored for a stream. Clears any
 * previously staged next track -- the caller should call
 * audio_set_next_track() again right after, for whatever comes after path.
 * has_replaygain/replaygain_gain_db/has_replaygain_peak/replaygain_peak are
 * this track's tags (see metadata.h); pass has_replaygain=false for tracks
 * with no ReplayGain tag. */
void audio_play_file_at(const char * path, double start_seconds,
                         bool has_replaygain, double replaygain_gain_db,
                         bool has_replaygain_peak, double replaygain_peak);

/* Tells the playback thread what comes after the current track, so it can
 * gapless-handoff or crossfade into it near the current track's natural
 * end, entirely on its own (no GUI round-trip). Call this right after
 * audio_play_file_at() (for the track after the one just started) and
 * again whenever audio_consume_track_advanced() reports the thread moved
 * on by itself (for the track after *that* one) -- otherwise the chain of
 * automatic transitions stops after one hop. Pass path=NULL to mean "end
 * of playlist, nothing queued" (a true EOF with nothing queued sets
 * audio_consume_track_finished() instead of advancing). */
void audio_set_next_track(const char * path, bool has_replaygain, double replaygain_gain_db,
                           bool has_replaygain_peak, double replaygain_peak);

/* Enables crossfading into the queued next track during the last few
 * seconds of the current one (only when both tracks share the same
 * channel count and sample rate -- crossfading across a format change
 * would need a resampler this project doesn't have, so that case always
 * falls back to a gapless handoff instead). When disabled, transitions
 * into a queued next track are gapless (no fade, but still no gap/reopen
 * for same-format tracks). */
void audio_set_crossfade_enabled(bool enabled);

/* Uses larger decoder/output batches while the display is off, reducing
 * audio-thread wakeups. Normal batches are restored immediately on wake. */
void audio_set_low_power_mode(bool enabled);

/* Routes playback to a connected Bluetooth a2dp-source sink (headphones/
 * speaker this device is streaming TO) instead of the local hardware
 * output, or back to local when disabled. Real-device bug: pairing/
 * connecting Bluetooth headphones worked (bluealsa already runs in
 * a2dp-source profile from boot, and the AVDTP connection itself completed
 * fine), but no audio ever played -- this app's output device was
 * hardcoded to the local ALSA card via tinyalsa, which can only address
 * numbered hw: cards directly and has no way to reach bluealsa's PCM at
 * all (that's only reachable through the full ALSA library's plugin
 * system, see audio.c). The GUI calls this whenever its own Bluetooth
 * connection-state poll changes (see poll_refresh_bt_icon() in gui.c),
 * gated on Bluetooth DAC mode being off (DAC mode swaps bluealsa to
 * a2dp-sink -- receiving audio, not sending -- so there's no source
 * profile for this device's own playback to route into while it's on).
 * Cheap to call repeatedly with the same value: only actually reopens the
 * output device (see ensure_device() in audio.c) if the requested target
 * differs from what's currently open, same as a sample-rate change does. */
void audio_set_bt_output(bool enabled);

/* Same shape as audio_set_bt_output() above, for an externally connected
 * USB audio device (DAC/amp) instead of Bluetooth -- alsa_device is the
 * resolved ALSA target string from usb_audio_output_is_connected()
 * (usb_audio_output.h), e.g. "plughw:1,0". The GUI calls this whenever its
 * own USB audio hotplug poll changes (see poll_usb_audio_output() in
 * gui.c) -- unlike Bluetooth, there is no manual toggle for this; it's
 * meant to feel exactly like the wired headphone jack, fully automatic.
 * ignored when enabled is false. */
void audio_set_usb_output(bool enabled, const char * alsa_device);

/* Toggle between playing and paused. No-op if nothing is loaded */
void audio_toggle_pause(void);

/* Stop playback and release the decoder/output device */
void audio_stop(void);

bool audio_is_playing(void);
bool audio_is_paused(void);

/* Seek to an absolute position in the current track. No-op if nothing is loaded */
void audio_seek(double seconds);

/* Seek to a 0-100 percent position in the current track. The request is
 * tied to the current playback generation and conversion to a frame is
 * deferred until that generation's decoder has published total_frames,
 * closing the track-start race described in audio_seek_percent() in audio.c.
 * Prefer this over audio_seek() for any UI control whose target is
 * naturally a percentage of the track (e.g. a progress slider) rather than
 * an absolute time offset the caller computed itself. */
void audio_seek_percent(double percent);

double audio_get_position_seconds(void);
/* Pending-aware position for durable pause/power-loss checkpoints. Unlike
 * audio_get_position_seconds(), this returns the latest deferred long-MP3
 * seek target while its background index is still being prepared. */
double audio_get_resume_position_seconds(void);
double audio_get_duration_seconds(void);

/* Sample rate of the currently loaded track, in Hz. 0 if nothing loaded. */
unsigned int audio_get_sample_rate(void);

/* Copies a coherent current-decoder snapshot. Returns false and zeroes *out
 * when nothing is loaded or the requested track has not opened yet. */
bool audio_get_current_format_info(audio_current_format_info_t * out);

/* 0.0 (silent) - 1.0 (full volume). Applied as software gain on the decoded PCM. */
void audio_set_volume(float volume);
/* Coalesces slider-originated volume changes on a process-lifetime worker.
 * A later synchronous audio_set_volume() always supersedes queued work. */
void audio_request_volume(float volume);
float audio_get_volume(void);

/* Shared with plugin_manager.c's own array-length validation for
 * plugin.set_hw_volume_curve() -- defined once here, rather than as a
 * separate local #define in each .c file, so the two can't drift apart. */
#define HW_VOLUME_CURVE_LEN 101

/* Overrides the UI-volume -> hardware-DAC-register mapping used for the
 * internal codec's own "Left"/"Right Playback Volume" ALSA controls (see
 * audio_apply_volume()'s own comment in audio.c). curve[i], i=0..100, is
 * the raw register value (0-255, hardware-specific meaning, not a dB
 * value) written for UI volume i% -- including curve[0], which stands in
 * for the native taper's own mute/silence handling. Pass NULL to fall
 * back to the built-in calibrated_taper_db()/volume_db_to_hw_raw() taper.
 * Reapplies immediately at the current UI volume so a switch is audible
 * right away rather than waiting for the next slider touch. Has no effect
 * on USB output, which never reaches this codec's own register at all and
 * always uses its own separate digital taper -- see audio_apply_volume()'s
 * own comment on why that path is deliberately untouched by this.
 *
 * For the normal "an already-loaded, running plugin changes its curve"
 * case (e.g. a Settings-list tap) -- NOT for plugin_manager.c's own plugin
 * (re)load transaction (covers both a full deinit/init reload AND each
 * individual plugin's own top-level run within plugin_manager_init()'s
 * load loop), which needs the staging functions below instead.
 * plugin_manager.c itself decides which of the two applies (checking
 * whether a plugin's own top-level script is currently running), not this
 * file -- see l_plugin_set_hw_volume_curve()'s own comment. Mutates ONLY
 * the live state audio_apply_volume() below actually reads -- never the
 * separate staging state the functions below operate on -- so this and a
 * plugin (re)load transaction in progress can never interfere with each
 * other's storage (see audio_stage_custom_hw_volume_curve()'s own comment
 * for why that separation, not just deferring the write, is required). */
void audio_set_custom_hw_volume_curve(const uint8_t * curve);

/* Mutates a SEPARATE staging copy of the curve state (curve non-NULL:
 * staged active with that table; NULL: staged inactive/native) -- not the
 * live state audio_apply_volume() reads, and issues no hardware write.
 * `curve` must be non-NULL whenever `active` is true, ignored when false.
 *
 * This, plus audio_get_staged_hw_volume_curve_state() and
 * audio_commit_hw_volume_curve() below, exist for plugin_manager.c's own
 * (re)load transaction (a full deinit/init reload, or plain top-level
 * loading of several plugin files in a row, any of which may call
 * plugin.set_hw_volume_curve()) for two compounding reasons: (1)
 * audio_output_request_hw_volume_raw() (audio_output.c) is a single-slot
 * "latest wins" async handoff to a background worker thread, not a real
 * queue, so writing hardware after every intermediate state change during
 * a transaction risks an earlier, momentarily-active value actually
 * reaching the DAC as a real, audible blip before the transaction's own
 * true final value overwrites it -- or, if a later plugin in the same
 * transaction then fails, a value that was never meant to survive at all
 * being written anyway. (2) Mutating the SAME live state
 * audio_apply_volume() reads is not enough on its own to prevent that,
 * even with the write itself deferred: a plugin (re)load transaction runs
 * synchronously on the UI thread, but audio_request_volume()'s own
 * worker thread (a queued volume-slider drag sample, audio.c) runs
 * independently and can call audio_apply_volume() -- which DOES write
 * hardware immediately, unconditionally, by design -- at any moment,
 * including mid-transaction; if that read the live state, it could send
 * a transaction's not-yet-committed intermediate value (a temporary
 * native reset, an intermediate plugin's curve, or a curve belonging to
 * a plugin that goes on to fail) straight to the DAC on its own,
 * bypassing the deferred-write discipline entirely. Keeping staged state
 * completely separate closes this: nothing outside plugin_manager.c's own
 * transaction functions ever reads it, so a concurrent volume request
 * during a transaction always sees and writes only the LAST FULLY
 * COMMITTED state, never an in-progress one. plugin_manager.c stages
 * state changes here throughout the whole transaction, then calls
 * audio_commit_hw_volume_curve() exactly once at the very end to install
 * the staged result as the new live state and write hardware, atomically,
 * in one locked step. */
void audio_stage_custom_hw_volume_curve(bool active, const uint8_t * curve);

/* Snapshots the current STAGED (not live) curve state into
 * *active_out/curve_out (curve_out must have room for HW_VOLUME_CURVE_LEN
 * bytes; written unconditionally, even when *active_out comes back false,
 * so a caller can always pass curve_out straight back into
 * audio_stage_custom_hw_volume_curve() to restore exactly this moment's
 * staged state later) -- e.g. plugin_manager.c snapshotting before a
 * single plugin's top-level run, so a mid-script failure can roll the
 * transaction's own staged state back to exactly what was staged before
 * THAT plugin started (which may be a different, still-valid plugin's own
 * curve, staged earlier in the same transaction), not just "off"/native. */
void audio_get_staged_hw_volume_curve_state(bool * active_out, uint8_t * curve_out);

/* Installs the currently staged curve state (see
 * audio_stage_custom_hw_volume_curve() above) as the new LIVE state and
 * writes the hardware register once to match, atomically -- one locked
 * critical section covers both the install and the write, so nothing,
 * including a concurrent volume-slider request on
 * audio_request_volume()'s own worker thread, can ever observe a live
 * state that has changed but hasn't been written to hardware yet, or vice
 * versa. Also correct, and a harmless no-op (staged starts zero-
 * initialized to "inactive", same as live), to call at normal boot for
 * the same reason. */
void audio_commit_hw_volume_curve(void);

/* Returns true (and clears the flag) exactly once when playback reached a
 * true end-of-playlist -- current track finished with no next track queued
 * (or the queued one failed to open). Meant to be polled from the GUI
 * thread's timer, since the audio thread must not call into LVGL directly.
 * When a next track WAS queued and playable, the thread moves on by itself
 * instead -- see audio_consume_track_advanced(). Note: NOT set on errors. */
bool audio_consume_track_finished(void);

/* Returns true (and clears the flag) exactly once when the playback thread
 * has autonomously moved on from "current" to the track staged via
 * audio_set_next_track() (via a gapless handoff or a completed crossfade),
 * without any audio_play_file_at() call. The GUI should treat this as "the
 * playlist index advanced by one" -- update its own index/labels/art and
 * call audio_set_next_track() again for whatever comes after. */
bool audio_consume_track_advanced(void);

/* Unrecoverable playback error codes, distinct from natural end-of-track.
 * audio.c reports failures but does not decide higher-level queue policy.
 * Decoder errors may be automatically skipped by the GUI layer (subject
 * to consecutive-failure safety caps), while output hardware errors remain
 * stopped and retryable at the last confirmed position.
 * Error events carry playback generation to allow the consumer to reject
 * stale errors from previous tracks. */
typedef enum {
    AUDIO_ERROR_NONE = 0,
    AUDIO_ERROR_DECODER_FAILED,  /* unrecoverable decode/container/file failure */
    AUDIO_ERROR_OUTPUT_FAILED,   /* output write error -- hardware recovery exhausted */
} audio_error_t;

/* Returns the most recent unrecoverable error (and clears it) exactly once.
 * AUDIO_ERROR_NONE means no pending error. Polled from the GUI timer. */
audio_error_t audio_consume_error(void);
audio_error_t audio_consume_error_ex(uint64_t * out_generation);

/* Returns the current playback generation counter. */
uint64_t audio_get_playback_generation(void);

#endif /* AUDIO_H */
