#ifndef AUDIO_HELPERS_H
#define AUDIO_HELPERS_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Upper bound on missing end-of-track frames due to container padding,
 * gapless encoder delay, or imprecise duration metadata (e.g. VBR MP3/AAC).
 * 8192 frames is ~0.18s at 44.1kHz. Any termination with more than 8192
 * unread frames is clearly premature and must be retried. */
#define MAX_EOF_TOLERANCE_FRAMES 8192ULL

/* Returns a safe, bounded sanitized filename/leaf for diagnostic logs.
 * Strips directory prefixes, remote URLs, HTTP credentials, query parameters,
 * and tokens so logs never leak sensitive info or private metadata. */
static inline const char * safe_path_tail(const char * path) {
    if (!path) return "(null)";
    const char * query = strchr(path, '?');
    const char * hash = strchr(path, '#');
    const char * end = path + strlen(path);
    if (query && query < end) end = query;
    if (hash && hash < end) end = hash;

    const char * start = path;
    const char * scheme = strstr(path, "://");
    if (scheme && scheme + 3 < end) {
        /* URL: check for a path component after the host/authority */
        const char * path_slash = NULL;
        for (const char * p = scheme + 3; p < end; p++) {
            if (*p == '/') path_slash = p;
        }
        if (path_slash) {
            /* Leaf path component (e.g. /rest/stream.flac -> stream.flac) */
            start = path_slash + 1;
        } else {
            /* Authority-only URL (e.g. http://user:pass@example.com) -> strip user:pass */
            const char * at = NULL;
            for (const char * p = scheme + 3; p < end; p++) {
                if (*p == '@') { at = p; break; }
            }
            start = at ? at + 1 : scheme + 3;
        }
    } else {
        /* Local path: find last '/' */
        const char * slash = NULL;
        for (const char * p = path; p < end; p++) {
            if (*p == '/') slash = p;
        }
        if (slash) start = slash + 1;
    }

    if (start >= end) return "(endpoint)";

    static __thread char buf[64];
    size_t len = (size_t) (end - start);
    if (len > 48) {
        start = end - 48;
        len = 48;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

/* Returns true if a zero-frame decoder read is clearly premature (i.e. not
 * natural EOF) for a finite local file. Live streams (total_frames == 0)
 * are excluded -- they never truly "end", so a zero read just means no data
 * yet. */
static inline bool is_premature_eof(uint64_t frames_played_local, uint64_t total_frames,
                                    bool is_stream) {
    if (is_stream || total_frames == 0) return false;
    if (frames_played_local >= total_frames) return false;
    uint64_t remaining = total_frames - frames_played_local;
    return remaining > MAX_EOF_TOLERANCE_FRAMES;
}

#include <limits.h>
#include <math.h>

#define RAMP_DURATION_MS 5

/* Calculates transition ramp length in frames from sample rate.
 * Capped to [1, 1024] to avoid excess latency or zero-frame divisions. */
static inline uint64_t calculate_ramp_frames(unsigned int sample_rate) {
    if (sample_rate == 0) sample_rate = 44100;
    uint64_t f = (uint64_t) ((5.0 / 1000.0) * (double) sample_rate + 0.5);
    if (f < 1) f = 1;
    if (f > 1024) f = 1024;
    return f;
}

/* Applies linear amplitude ramp from start_gain to end_gain in-place.
 * Preserves stereo balance by scaling every channel in a frame identically. */
static inline void apply_ramp(int16_t * buf, uint64_t frames, unsigned int channels, float start_gain, float end_gain) {
    if (!buf || frames == 0 || channels == 0) return;
    for (uint64_t i = 0; i < frames; i++) {
        float t = (frames > 1) ? (float) i / (float) (frames - 1) : 1.0f;
        float gain = start_gain + t * (end_gain - start_gain);
        for (unsigned int ch = 0; ch < channels; ch++) {
            size_t idx = (size_t) i * channels + ch;
            float val = (float) buf[idx] * gain;
            int32_t s = (int32_t) (val + (val >= 0.0f ? 0.5f : -0.5f));
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            buf[idx] = (int16_t) s;
        }
    }
}

typedef struct {
    char path[PATH_MAX];
    bool valid;
    float replaygain_linear;
    bool replaygain_applied;
    uint64_t generation;
} next_track_snapshot_t;

typedef enum {
    WRITE_RESULT_OK = 0,
    WRITE_RESULT_ABORTED,
    WRITE_RESULT_FAILED
} write_result_t;

/* Returns true if an output retry loop should abort based on playback state */
static inline bool should_abort_write_retry(bool allow_during_stop_restart, bool stop_req, bool restart_req) {
    if (allow_during_stop_restart) return false;
    return (stop_req || restart_req);
}

typedef struct {
    int target_index;            /* Next playlist index to play, or -1 if none */
    int queued_consumed;         /* Number of queued_pending items traversed/consumed */
    int shuffle_steps;           /* Number of shuffle positions advanced */
    bool shuffle_wrapped;        /* Whether shuffle order wrapped into pending_shuffle_order */
} failure_advance_plan_t;

/* Callback type to check whether a given playlist index points to a failed physical file.
 * Returns true if the index should be skipped. */
typedef bool (*is_failed_index_fn)(int index, void * userdata);

/* Pure algorithm to compute next playlist index upon fatal decoder failure with full
 * queue/shuffle step tracking and physical file deduplication.
 *
 * play_mode: 0 = PLAY_MODE_SEQUENTIAL, 1 = PLAY_MODE_REPEAT_ALL, 2 = PLAY_MODE_REPEAT_ONE, 3 = PLAY_MODE_SHUFFLE
 */
static inline failure_advance_plan_t compute_decoder_failure_advance_plan(
    int failed_index,
    int playlist_count,
    int play_mode,
    int queued_pending_count,
    const int * shuffle_order,
    int shuffle_pos,
    const int * pending_shuffle_order,
    is_failed_index_fn is_failed_fn,
    void * userdata)
{
    failure_advance_plan_t plan = {
        .target_index = -1,
        .queued_consumed = 0,
        .shuffle_steps = 0,
        .shuffle_wrapped = false
    };

    if (failed_index < 0 || playlist_count <= 0) return plan;
    int orig_failed_index = failed_index;

    /* 1. Explicitly queued tracks retain priority */
    if (queued_pending_count > 0) {
        int queued_limit = (failed_index + queued_pending_count < playlist_count)
                           ? queued_pending_count
                           : (playlist_count - 1 - failed_index);
        for (int q = 1; q <= queued_limit; q++) {
            int cand = failed_index + q;
            if (!is_failed_fn || !is_failed_fn(cand, userdata)) {
                plan.target_index = cand;
                plan.queued_consumed = q;
                return plan;
            }
        }
        /* All queued items were traversed and failed; record them as consumed */
        plan.queued_consumed = queued_pending_count;
        /* Proceed to normal play_mode from the end of the queued block */
        failed_index = failed_index + queued_pending_count;
        if (failed_index >= playlist_count) failed_index = playlist_count - 1;
    }

    if (playlist_count <= 1) {
        plan.target_index = -1;
        return plan;
    }

    switch (play_mode) {
        case 3: /* PLAY_MODE_SHUFFLE */ {
            if (!shuffle_order) {
                plan.target_index = -1;
                return plan;
            }
            /* Step through shuffle positions. When wrapping into pending_shuffle_order,
             * we need to examine all remaining entries in the current bag plus all
             * entries in the pending bag (remaining_current_steps + playlist_count). */
            int remaining_current_steps = (playlist_count - 1 >= shuffle_pos) ? (playlist_count - 1 - shuffle_pos) : 0;
            int max_steps = (pending_shuffle_order != NULL) ? (remaining_current_steps + playlist_count) : remaining_current_steps;
            for (int step = 1; step <= max_steps; step++) {
                int pos = shuffle_pos + step;
                int cand = -1;
                bool wrapped = false;
                if (pos < playlist_count) {
                    cand = shuffle_order[pos];
                } else if (pending_shuffle_order) {
                    int wrap_pos = pos - playlist_count;
                    if (wrap_pos < playlist_count) {
                        cand = pending_shuffle_order[wrap_pos];
                        wrapped = true;
                    }
                }
                if (cand >= 0 && cand < playlist_count && cand != orig_failed_index && cand != failed_index) {
                    if (!is_failed_fn || !is_failed_fn(cand, userdata)) {
                        plan.target_index = cand;
                        plan.shuffle_steps = step;
                        plan.shuffle_wrapped = wrapped;
                        return plan;
                    }
                }
            }
            plan.target_index = -1;
            return plan;
        }
        case 2: /* PLAY_MODE_REPEAT_ONE: override repeat one for failure recovery */
        case 1: /* PLAY_MODE_REPEAT_ALL */ {
            for (int step = 1; step < playlist_count; step++) {
                int cand = (failed_index + step) % playlist_count;
                if (cand != orig_failed_index && cand != failed_index) {
                    if (!is_failed_fn || !is_failed_fn(cand, userdata)) {
                        plan.target_index = cand;
                        return plan;
                    }
                }
            }
            plan.target_index = -1;
            return plan;
        }
        case 0: /* PLAY_MODE_SEQUENTIAL */
        default: {
            for (int cand = failed_index + 1; cand < playlist_count; cand++) {
                if (cand != orig_failed_index && (!is_failed_fn || !is_failed_fn(cand, userdata))) {
                    plan.target_index = cand;
                    return plan;
                }
            }
            plan.target_index = -1;
            return plan;
        }
    }
}

/* Convenience wrapper returning only the next candidate index */
static inline int compute_decoder_failure_advance_index_pure(
    int failed_index,
    int playlist_count,
    int play_mode,
    int queued_pending_count,
    const int * shuffle_order,
    int shuffle_pos,
    const int * pending_shuffle_order)
{
    failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
        failed_index, playlist_count, play_mode, queued_pending_count,
        shuffle_order, shuffle_pos, pending_shuffle_order, NULL, NULL
    );
    return plan.target_index;
}

#endif /* AUDIO_HELPERS_H */
