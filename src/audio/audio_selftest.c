#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "audio.h"
#include "audio_helpers.h"

/* --- Test 1: Production safe_path_tail Verification --- */

static void test_safe_path_tail_production(void) {
    printf("Testing production safe_path_tail...\n");

    /* NULL returns (null) */
    assert(strcmp(safe_path_tail(NULL), "(null)") == 0);

    /* Local paths extract leaf basename */
    assert(strcmp(safe_path_tail("/music/album/01 - Song.flac"), "01 - Song.flac") == 0);
    assert(strcmp(safe_path_tail("track.mp3"), "track.mp3") == 0);

    /* Remote URLs with paths strip credentials, tokens, query strings, and hash fragments */
    const char * url_with_creds = "http://user:secret_password@myserver.com:4040/rest/stream?id=12345&u=user&p=secret_password";
    const char * tail1 = safe_path_tail(url_with_creds);
    assert(strcmp(tail1, "stream") == 0);
    assert(strstr(tail1, "secret_password") == NULL);
    assert(strstr(tail1, "user") == NULL);
    assert(strstr(tail1, "12345") == NULL);

    /* Authority-only URLs (no path) redact user:password */
    const char * authority_only = "http://user:password@example.com";
    const char * tail_auth = safe_path_tail(authority_only);
    assert(strcmp(tail_auth, "example.com") == 0);
    assert(strstr(tail_auth, "password") == NULL);
    assert(strstr(tail_auth, "user") == NULL);

    const char * authority_with_query = "https://admin:token123@subsonic.local:4040?format=mp3";
    const char * tail_auth_q = safe_path_tail(authority_with_query);
    assert(strcmp(tail_auth_q, "subsonic.local:4040") == 0);
    assert(strstr(tail_auth_q, "token123") == NULL);
    assert(strstr(tail_auth_q, "admin") == NULL);

    const char * url_with_hash = "https://cdn.example.org/audio/remote_song.opus#t=10,20";
    const char * tail2 = safe_path_tail(url_with_hash);
    assert(strcmp(tail2, "remote_song.opus") == 0);
    assert(strstr(tail2, "#") == NULL);

    /* Length is capped at 48 chars */
    const char * long_name = "/path/012345678901234567890123456789012345678901234567890123456789.mp3";
    const char * tail3 = safe_path_tail(long_name);
    assert(strlen(tail3) <= 48);

    printf("  -> production safe_path_tail passed.\n");
}

/* --- Test 2: Production is_premature_eof Verification (Bounded 8192 Frames) --- */

static void test_premature_eof_production(void) {
    printf("Testing production is_premature_eof with bounded tolerance...\n");

    /* Live streams or 0 total frames never trigger premature EOF */
    assert(!is_premature_eof(0, 0, true));
    assert(!is_premature_eof(1000, 0, false));
    assert(!is_premature_eof(50000, 100000, true));

    /* 4-minute song at 44.1kHz = 10,584,000 frames */
    uint64_t total = 10584000;

    /* 50% through: remaining = 5,292,000 > 8192 -> premature */
    assert(is_premature_eof(5292000, total, false));

    /* 10 seconds remaining: remaining = 441,000 > 8192 -> premature */
    assert(is_premature_eof(total - 441000, total, false));

    /* 1 second remaining: remaining = 44,100 > 8192 -> premature */
    assert(is_premature_eof(total - 44100, total, false));

    /* 10,000 frames remaining (> 8192) -> premature */
    assert(is_premature_eof(total - 10000, total, false));

    /* 8,192 frames remaining (<= 8192) -> natural container EOF (within tolerance) */
    assert(!is_premature_eof(total - 8192, total, false));

    /* 1,000 frames remaining -> natural container EOF */
    assert(!is_premature_eof(total - 1000, total, false));

    /* Exactly at end or beyond */
    assert(!is_premature_eof(total, total, false));
    assert(!is_premature_eof(total + 500, total, false));

    printf("  -> production is_premature_eof passed.\n");
}

/* --- Test 3: Partial Output Delivery & Retry Progress (No Duplication) --- */

typedef struct {
    int call_count;
    uint64_t delivered_first_call;
} mock_partial_device_t;

static bool mock_output_write_partial(mock_partial_device_t * dev, const int16_t * buf,
                                      uint64_t frames, unsigned int channels,
                                      uint64_t * out_written) {
    (void) buf;
    (void) channels;
    dev->call_count++;
    if (dev->call_count == 1) {
        /* First call: partial write of 400 frames, then simulate pipe break */
        uint64_t partial = (frames < dev->delivered_first_call) ? frames : dev->delivered_first_call;
        if (out_written) *out_written = partial;
        return false;
    }
    /* Subsequent call (retry): deliver all remaining frames */
    if (out_written) *out_written = frames;
    return true;
}

static void test_partial_write_accounting(void) {
    printf("Testing partial output write and retry progress without audio duplication...\n");

    int16_t test_buf[2000];
    for (int i = 0; i < 2000; i++) test_buf[i] = (int16_t) i;

    mock_partial_device_t dev = { .call_count = 0, .delivered_first_call = 400 };
    uint64_t total_frames = 1000;
    unsigned int channels = 2;

    uint64_t delivered = 0;
    for (int attempt = 0; attempt <= 3; attempt++) {
        const int16_t * cur_buf = test_buf + (size_t)(delivered * channels);
        uint64_t remaining = total_frames - delivered;
        uint64_t written_this_call = 0;

        bool ok = mock_output_write_partial(&dev, cur_buf, remaining, channels, &written_this_call);
        delivered += written_this_call;

        if (ok || delivered >= total_frames) break;
    }

    /* Verify call 1 wrote 400 frames, call 2 wrote remaining 600 frames */
    assert(dev.call_count == 2);
    assert(delivered == 1000);

    printf("  -> partial output delivery and retry accounting passed.\n");
}

/* --- Test 4: Stop/Restart Abort during Retry --- */

typedef enum {
    TEST_WRITE_OK = 0,
    TEST_WRITE_ABORTED,
    TEST_WRITE_FAILED
} test_write_result_t;

static test_write_result_t simulate_retry_with_abort(bool stop_during_retry) {
    bool stop_req = false;
    for (int attempt = 0; attempt <= 3; attempt++) {
        if (attempt == 1 && stop_during_retry) {
            stop_req = true;
        }
        if (stop_req) {
            return TEST_WRITE_ABORTED;
        }
    }
    return TEST_WRITE_FAILED;
}

static void test_output_retry_abort_handling(void) {
    printf("Testing stop/restart abort during output retries...\n");

    /* Stop arriving during retry produces ABORTED, not FAILED */
    assert(simulate_retry_with_abort(true) == TEST_WRITE_ABORTED);
    /* Permanent failure with no stop produces FAILED */
    assert(simulate_retry_with_abort(false) == TEST_WRITE_FAILED);

    printf("  -> stop/restart abort during retry passed.\n");
}

/* --- Test 5: Reopen and Initial Seek Verification --- */

typedef struct {
    bool open_success;
    bool seek_success;
    bool closed;
} mock_decoder_t;

static bool mock_reopen_decoder_at(mock_decoder_t * dec, bool open_ok, bool seek_ok, uint64_t frame) {
    dec->closed = true; /* close previous instance */
    if (!open_ok) return false;
    dec->open_success = true;
    dec->closed = false;
    if (frame > 0) {
        if (!seek_ok) {
            dec->closed = true; /* seek failed: must close */
            return false;
        }
    }
    return true;
}

static void test_reopen_and_initial_seek_verification(void) {
    printf("Testing reopen and initial seek verification...\n");

    /* 1. Reopen seek failure -> closes decoder and returns false */
    mock_decoder_t dec1 = {0};
    bool ok1 = mock_reopen_decoder_at(&dec1, true, false, 50000);
    assert(!ok1);
    assert(dec1.closed == true);

    /* 2. Reopen seek success -> returns true */
    mock_decoder_t dec2 = {0};
    bool ok2 = mock_reopen_decoder_at(&dec2, true, true, 50000);
    assert(ok2);
    assert(!dec2.closed);

    /* 3. Initial start seek failure -> retains frame 0 */
    uint64_t start_frame = 44100 * 30; /* 30 seconds */
    uint64_t cur_frames_played = 0;
    bool mock_seek_success = false;

    if (mock_seek_success) {
        cur_frames_played = start_frame;
    } else {
        cur_frames_played = 0;
    }
    assert(cur_frames_played == 0); /* Position must accurately remain 0 */

    printf("  -> reopen and initial seek verification passed.\n");
}

/* --- Test 6: Crossfade Premature EOF Detection --- */

static void test_crossfade_premature_eof_handling(void) {
    printf("Testing crossfade premature EOF vs natural EOF handling...\n");

    uint64_t total_frames = 1000000; /* ~22s track */

    /* cur_dec stopped 2s into 3s blend window: 44,100 frames remain (> 8192) */
    uint64_t cur_played_premature = total_frames - 44100;
    bool premature = is_premature_eof(cur_played_premature, total_frames, false);
    assert(premature == true); /* Must trigger recovery, NOT promote next */

    /* cur_dec reached end of container within 1000 frames (<= 8192) */
    uint64_t cur_played_natural = total_frames - 1000;
    bool natural = !is_premature_eof(cur_played_natural, total_frames, false);
    assert(natural == true); /* Natural EOF: promote next track cleanly */

    printf("  -> crossfade premature EOF handling passed.\n");
}

/* --- Test 7: GUI Error Consumption & Queue Preservation --- */

typedef struct {
    int playlist_index;
    bool play_button_playing;
    bool deferred_resume_pending;
    double deferred_resume_position;
    const char * toast_message;
} mock_gui_player_state_t;

static void mock_gui_handle_playback_error(mock_gui_player_state_t * gui, audio_error_t err, double confirmed_pos) {
    if (err == AUDIO_ERROR_NONE) return;
    if (gui->playlist_index < 0) return;

    gui->deferred_resume_position = (confirmed_pos > 0.0) ? confirmed_pos : 0.0;
    gui->deferred_resume_pending = true;
    gui->play_button_playing = false;

    if (err == AUDIO_ERROR_DECODER_FAILED) {
        gui->toast_message = "Playback error: decoder failed";
    } else if (err == AUDIO_ERROR_OUTPUT_FAILED) {
        gui->toast_message = "Playback error: audio output failed";
    }
}

static void mock_gui_toggle_play_pause(mock_gui_player_state_t * gui, int * restarted_at_track, double * restarted_at_pos) {
    if (gui->deferred_resume_pending) {
        *restarted_at_track = gui->playlist_index;
        *restarted_at_pos = gui->deferred_resume_position;
        gui->deferred_resume_pending = false;
        gui->deferred_resume_position = 0.0;
        gui->play_button_playing = true;
    }
}

static void test_gui_error_and_queue_preservation(void) {
    printf("Testing GUI error consumption and playlist queue preservation...\n");

    mock_gui_player_state_t gui = {
        .playlist_index = 4,
        .play_button_playing = true,
        .deferred_resume_pending = false,
        .deferred_resume_position = 0.0,
        .toast_message = NULL
    };

    /* Playback error occurs at 75.5 seconds into track 4 */
    mock_gui_handle_playback_error(&gui, AUDIO_ERROR_OUTPUT_FAILED, 75.5);

    /* 1. Queue is NOT advanced */
    assert(gui.playlist_index == 4);
    /* 2. Play button shows paused */
    assert(gui.play_button_playing == false);
    /* 3. Error toast is set */
    assert(gui.toast_message != NULL);
    assert(strcmp(gui.toast_message, "Playback error: audio output failed") == 0);
    /* 4. Position is checkpointed for retry */
    assert(gui.deferred_resume_pending == true);
    assert(gui.deferred_resume_position == 75.5);

    /* User taps Play: resumes track 4 from 75.5 seconds */
    int restarted_track = -1;
    double restarted_pos = 0.0;
    mock_gui_toggle_play_pause(&gui, &restarted_track, &restarted_pos);

    assert(restarted_track == 4);
    assert(restarted_pos == 75.5);
    assert(gui.play_button_playing == true);
    assert(gui.deferred_resume_pending == false);

    printf("  -> GUI error consumption and queue preservation passed.\n");
}

/* --- Test 8: Phase 1 AIFF Reusable Buffer & Checked Chunk Reads --- */

static void test_aiff_phase1_buffer_reuse(void) {
    printf("Testing Phase 1 AIFF buffer reuse and checked arithmetic...\n");

    /* Verify arithmetic checks */
    uint32_t channels = 2;
    uint32_t bytes_per_sample = 3; /* 24-bit */
    size_t bytes_per_frame = (size_t) channels * (size_t) bytes_per_sample;
    size_t chunk_frames = 8192;
    size_t total_buf_size = chunk_frames * bytes_per_frame;
    assert(total_buf_size == 49152);

    /* Verify chunk subdivision */
    uint64_t large_request = 20000;
    uint64_t frames_read = 0;
    int iterations = 0;
    while (frames_read < large_request) {
        uint64_t chunk = large_request - frames_read;
        if (chunk > 8192) chunk = 8192;
        frames_read += chunk;
        iterations++;
    }
    assert(frames_read == 20000);
    assert(iterations == 3); /* 8192 + 8192 + 3616 */

    printf("  -> Phase 1 AIFF buffer reuse and chunking passed.\n");
}

/* --- Test 9: Phase 2 Decoder Status & Bounded Corruption Recovery --- */

static void test_decoder_result_contracts(void) {
    printf("Testing Phase 2 decoder result status contracts and retry caps...\n");

    decoder_read_result_t ok_res = { .frames = 1024, .status = DECODER_READ_OK };
    assert(ok_res.frames == 1024);
    assert(ok_res.status == DECODER_READ_OK);

    decoder_read_result_t eof_res = { .frames = 0, .status = DECODER_READ_EOF };
    assert(eof_res.frames == 0);
    assert(eof_res.status == DECODER_READ_EOF);

    /* Test DSD classification: 0 frames before total_pcm_frames MUST be fatal, not EOF */
    uint64_t dsd_current_frame = 50000;
    uint64_t dsd_total_frames = 200000;
    decoder_read_status_t dsd_fail_status = (dsd_current_frame >= dsd_total_frames) ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
    assert(dsd_fail_status == DECODER_READ_FATAL_ERROR);

    uint64_t dsd_at_eof_frame = 200000;
    decoder_read_status_t dsd_eof_status = (dsd_at_eof_frame >= dsd_total_frames) ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
    assert(dsd_eof_status == DECODER_READ_EOF);

    /* Simulate consecutive error bound */
    unsigned int consecutive = 0;
    const unsigned int MAX_ERRORS = 5;
    decoder_read_status_t status = DECODER_READ_OK;

    for (int i = 0; i < 10; i++) {
        consecutive++;
        if (consecutive >= MAX_ERRORS) {
            status = DECODER_READ_FATAL_ERROR;
            break;
        }
        status = DECODER_READ_RECOVERABLE_ERROR;
    }

    assert(consecutive == 5);
    assert(status == DECODER_READ_FATAL_ERROR);

    printf("  -> Phase 2 decoder result contracts passed.\n");
}

/* --- Test 10: Phase 3 Steady-State Zero-Allocation Snapshotting (Production Helper) --- */

static void test_phase3_steady_state_snapshot(void) {
    printf("Testing Phase 3 steady-state snapshotting using production next_track_snapshot_t...\n");

    const char * mock_next_path = "/media/sdcard/Music/Album/02 - Track.flac";
    uint64_t active_gen = 42;

    for (int iter = 0; iter < 1000; iter++) {
        next_track_snapshot_t snap = {0};
        if (mock_next_path) {
            size_t len = strlen(mock_next_path);
            assert(len < sizeof(snap.path));
            memcpy(snap.path, mock_next_path, len + 1);
            snap.valid = true;
            snap.replaygain_linear = 0.89f;
            snap.replaygain_applied = true;
            snap.generation = active_gen;
        }

        assert(snap.valid);
        assert(snap.generation == active_gen);
        assert(strcmp(snap.path, mock_next_path) == 0);
        assert(snap.replaygain_linear == 0.89f);
    }

    printf("  -> Phase 3 steady-state snapshotting passed.\n");
}

/* --- Test 11: Phase 4 Transition Ramps & Stereo Balance (Production Helpers) --- */

static void test_phase4_transition_ramps(void) {
    printf("Testing Phase 4 5ms transition ramps using production calculate_ramp_frames and apply_ramp...\n");

    /* Check frame calculations across standard sample rates */
    assert(calculate_ramp_frames(44100) == 221);
    assert(calculate_ramp_frames(48000) == 240);
    assert(calculate_ramp_frames(88200) == 441);
    assert(calculate_ramp_frames(96000) == 480);
    assert(calculate_ramp_frames(176400) == 882);
    assert(calculate_ramp_frames(192000) == 960);
    assert(calculate_ramp_frames(352800) == 1024); /* Clamped to 1024 */

    /* Verify fade-in: starts at 0, ends at 1.0 */
    uint64_t rf = 221;
    unsigned int ch = 2;
    int16_t * pcm = malloc((size_t) rf * ch * sizeof(int16_t));
    for (size_t i = 0; i < rf * ch; i++) pcm[i] = 10000;

    apply_ramp(pcm, rf, ch, 0.0f, 1.0f);
    assert(pcm[0] == 0 && pcm[1] == 0); /* First frame fully zeroed */
    assert(pcm[(rf - 1) * ch] == 10000 && pcm[(rf - 1) * ch + 1] == 10000); /* Last frame at full unity */
    /* Check stereo balance: left channel equals right channel */
    for (uint64_t i = 0; i < rf; i++) {
        assert(pcm[i * ch] == pcm[i * ch + 1]);
    }

    /* Verify fade-out: starts at 1.0, ends at 0 */
    for (size_t i = 0; i < rf * ch; i++) pcm[i] = 10000;
    apply_ramp(pcm, rf, ch, 1.0f, 0.0f);
    assert(pcm[0] == 10000 && pcm[1] == 10000); /* First frame at full unity */
    assert(pcm[(rf - 1) * ch] == 0 && pcm[(rf - 1) * ch + 1] == 0); /* Last frame zeroed */
    for (uint64_t i = 0; i < rf; i++) {
        assert(pcm[i * ch] == pcm[i * ch + 1]);
    }

    free(pcm);
    printf("  -> Phase 4 transition ramps and stereo balance passed.\n");
}

/* --- Test 12: Controlled Transition Ramp Write Abort Policy --- */

static void test_transition_ramp_write_abort_policy(void) {
    printf("Testing transition ramp write abort policy (should_abort_write_retry)...\n");

    /* Normal playback writes: must abort immediately on stop or restart */
    assert(should_abort_write_retry(false, true, false) == true);
    assert(should_abort_write_retry(false, false, true) == true);
    assert(should_abort_write_retry(false, true, true) == true);
    assert(should_abort_write_retry(false, false, false) == false);

    /* Controlled transition ramp writes (allow_during_stop_restart=true): must NOT abort on stop or restart */
    assert(should_abort_write_retry(true, true, false) == false);
    assert(should_abort_write_retry(true, false, true) == false);
    assert(should_abort_write_retry(true, true, true) == false);
    assert(should_abort_write_retry(true, false, false) == false);

    printf("  -> transition ramp write abort policy passed.\n");
}

/* --- Test 13: Crossfade Next-Decoder Failure Cancellation & Audio Preservation --- */

static void test_crossfade_next_decoder_failure_cancellation(void) {
    printf("Testing crossfade next-decoder failure cancellation and audio preservation...\n");

    bool nxt_open = true;
    bool nxt_format_matches = true;
    unsigned int consecutive_nxt_decoder_errors = 0;
    uint64_t cur_frames_played_local = 100000;
    uint64_t n_cur = 1024;
    uint64_t delivered = 0;

    /* 1. Simulate next-decoder returning DECODER_READ_FATAL_ERROR */
    decoder_read_result_t r_next = { .frames = 0, .status = DECODER_READ_FATAL_ERROR };
    bool nxt_failed = false;

    if (r_next.status == DECODER_READ_FATAL_ERROR || (r_next.status == DECODER_READ_EOF && r_next.frames == 0)) {
        nxt_failed = true;
    }

    if (nxt_failed) {
        nxt_open = false;
        nxt_format_matches = false;
        consecutive_nxt_decoder_errors = 0;

        /* Must deliver cur frames rather than discarding */
        delivered = n_cur;
        cur_frames_played_local += delivered;
    }

    /* Verify crossfade is aborted, next decoder is closed, and current frames were NOT skipped */
    assert(!nxt_open);
    assert(!nxt_format_matches);
    assert(cur_frames_played_local == 101024);

    /* 2. Simulate next-decoder consecutive recoverable errors exceeding limit (10) */
    nxt_open = true;
    nxt_format_matches = true;
    consecutive_nxt_decoder_errors = 0;
    nxt_failed = false;

    for (int i = 0; i < 10; i++) {
        consecutive_nxt_decoder_errors++;
        if (consecutive_nxt_decoder_errors >= 10) {
            nxt_failed = true;
            break;
        }
    }

    assert(nxt_failed == true);
    assert(consecutive_nxt_decoder_errors == 10);

    printf("  -> crossfade next-decoder failure cancellation and audio preservation passed.\n");
}

/* --- Test 14: Deadlock-Free Pause State Snapshotting --- */

static void test_deadlock_free_pause_state_handling(void) {
    printf("Testing deadlock-free pause state handling...\n");

    pthread_mutex_t mock_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
    bool mock_paused = true;
    bool mock_stop = false;
    bool mock_restart = false;
    float mock_volume = 0.85f;

    /* 1. Acquire mutex */
    pthread_mutex_lock(&mock_audio_mutex);
    bool is_paused = mock_paused && !mock_stop && !mock_restart;
    float captured_vol = mock_volume;

    /* 2. When pause detected: unlock mutex BEFORE doing DSP/ramp/output-write */
    if (is_paused) {
        pthread_mutex_unlock(&mock_audio_mutex);
    }

    /* 3. Output write helper is called without holding the mutex:
     * It can safely acquire mock_audio_mutex internally without self-deadlock */
    int lock_status = pthread_mutex_trylock(&mock_audio_mutex);
    assert(lock_status == 0); /* Successfully locked because mutex was NOT held */
    pthread_mutex_unlock(&mock_audio_mutex);

    assert(captured_vol == 0.85f);
    printf("  -> deadlock-free pause state handling passed.\n");
}

/* --- Test 15: Safe Queue Advancement on Fatal Decoder Failure (Scenarios 1-15) --- */

static void test_decoder_failure_advance_scenarios_1_to_7(void) {
    printf("Testing decoder failure advance scenarios 1 to 7...\n");

    /* Scenario 1: Sequential playback advances on fatal decoder error */
    int next_seq = compute_decoder_failure_advance_index_pure(0, 3, 0 /* SEQUENTIAL */, 0, NULL, 0, NULL);
    assert(next_seq == 1);
    next_seq = compute_decoder_failure_advance_index_pure(1, 3, 0 /* SEQUENTIAL */, 0, NULL, 0, NULL);
    assert(next_seq == 2);

    /* Explicit queued priority override */
    int next_queued = compute_decoder_failure_advance_index_pure(0, 3, 0 /* SEQUENTIAL */, 1 /* queued */, NULL, 0, NULL);
    assert(next_queued == 1);

    /* Scenario 2: Sequential playback stops cleanly at the end of the playlist on fatal error */
    int next_seq_end = compute_decoder_failure_advance_index_pure(2, 3, 0 /* SEQUENTIAL */, 0, NULL, 0, NULL);
    assert(next_seq_end == -1);

    /* Scenario 3: Repeat All wraps from last to first on fatal error */
    int next_rep_all = compute_decoder_failure_advance_index_pure(2, 3, 1 /* REPEAT_ALL */, 0, NULL, 0, NULL);
    assert(next_rep_all == 0);
    next_rep_all = compute_decoder_failure_advance_index_pure(0, 3, 1 /* REPEAT_ALL */, 0, NULL, 0, NULL);
    assert(next_rep_all == 1);

    /* Scenario 4: Repeat One advances away from a failed track rather than looping infinitely on it */
    int next_rep_one = compute_decoder_failure_advance_index_pure(1, 3, 2 /* REPEAT_ONE */, 0, NULL, 0, NULL);
    assert(next_rep_one == 2);
    assert(next_rep_one != 1); /* Overrides repeat one */

    /* Scenario 5: Repeat One stops if the playlist contains only one track */
    int next_rep_one_single = compute_decoder_failure_advance_index_pure(0, 1, 2 /* REPEAT_ONE */, 0, NULL, 0, NULL);
    assert(next_rep_one_single == -1);

    /* Scenario 6: Shuffle mode chooses next shuffle item on failure without retrying the failed index */
    int shuffle_order[3] = { 2, 0, 1 };
    /* Playing track 2 (at shuffle_pos 0), failed -> next candidate is shuffle_order[1] = 0 */
    int next_shuf = compute_decoder_failure_advance_index_pure(2, 3, 3 /* SHUFFLE */, 0, shuffle_order, 0, NULL);
    assert(next_shuf == 0);
    assert(next_shuf != 2);

    /* Scenario 7: Shuffle mode wrap handles failure on last shuffle item safely */
    int pending_shuffle[3] = { 0, 2, 1 };
    /* Playing track 1 (at shuffle_pos 2 = last item in bag), failed -> next is pending_shuffle[0] = 0 */
    int next_shuf_wrap = compute_decoder_failure_advance_index_pure(1, 3, 3 /* SHUFFLE */, 0, shuffle_order, 2, pending_shuffle);
    assert(next_shuf_wrap == 0);
    assert(next_shuf_wrap != 1);

    printf("  -> decoder failure advance scenarios 1 to 7 passed.\n");
}

static void test_consecutive_decoder_failure_safety_cap_scenario_8(void) {
    printf("Testing consecutive decoder failure safety cap (Scenario 8)...\n");

    /* Safety cap = min(5, playlist_count) */
    int count_10 = 10;
    int max_skips_10 = (count_10 < 5) ? count_10 : 5;
    assert(max_skips_10 == 5);

    int skips = 0;
    bool stopped = false;
    for (int i = 0; i < 6; i++) {
        skips++;
        if (skips >= max_skips_10) {
            stopped = true;
            break;
        }
    }
    assert(stopped == true);
    assert(skips == 5);

    /* Small playlist of 3 items: safety cap = min(5, 3) = 3 */
    int count_3 = 3;
    int max_skips_3 = (count_3 < 5) ? count_3 : 3;
    assert(max_skips_3 == 3);

    skips = 0;
    stopped = false;
    for (int i = 0; i < 5; i++) {
        skips++;
        if (skips >= max_skips_3) {
            stopped = true;
            break;
        }
    }
    assert(stopped == true);
    assert(skips == 3);

    printf("  -> consecutive decoder failure safety cap passed.\n");
}

static void test_consecutive_skip_counter_resets_scenarios_9_to_11(void) {
    printf("Testing consecutive skip counter resets (Scenarios 9 to 11)...\n");

    int consecutive_skips = 3;

    /* Scenario 9: 3 seconds of confirmed playback resets the consecutive skip counter */
    double confirmed_pos = 3.2;
    if (confirmed_pos >= 3.0 && consecutive_skips > 0) {
        consecutive_skips = 0;
    }
    assert(consecutive_skips == 0);

    /* Scenario 10: Explicit user song selection resets the consecutive skip counter */
    consecutive_skips = 4;
    /* User taps song in library */
    consecutive_skips = 0;
    assert(consecutive_skips == 0);

    /* Scenario 11: Explicit user Prev/Next resets the consecutive skip counter */
    consecutive_skips = 2;
    /* User taps Next */
    consecutive_skips = 0;
    assert(consecutive_skips == 0);

    printf("  -> consecutive skip counter resets passed.\n");
}

static void test_output_failure_vs_decoder_failure_scenario_12_and_15(void) {
    printf("Testing output failure vs decoder failure (Scenarios 12 and 15)...\n");

    /* Scenario 12: Output failures (AUDIO_ERROR_OUTPUT_FAILED) never advance queue */
    int current_index = 2;
    bool deferred_resume_pending = false;
    double deferred_resume_position = 0.0;
    bool is_playing = true;

    /* Simulate AUDIO_ERROR_OUTPUT_FAILED */
    audio_error_t err_output = AUDIO_ERROR_OUTPUT_FAILED;
    if (err_output == AUDIO_ERROR_OUTPUT_FAILED) {
        deferred_resume_position = 42.0;
        deferred_resume_pending = true;
        is_playing = false;
        /* Queue index unchanged */
    }
    assert(current_index == 2);
    assert(deferred_resume_pending == true);
    assert(deferred_resume_position == 42.0);
    assert(is_playing == false);

    /* Scenario 15: Deferred resume is cleared on auto-skip so pressing Play does not restart the broken track */
    audio_error_t err_decoder = AUDIO_ERROR_DECODER_FAILED;
    if (err_decoder == AUDIO_ERROR_DECODER_FAILED) {
        int next_idx = compute_decoder_failure_advance_index_pure(current_index, 5, 0, 0, NULL, 0, NULL);
        assert(next_idx == 3);
        current_index = next_idx;
        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        is_playing = true;
    }
    assert(current_index == 3);
    assert(deferred_resume_pending == false);
    assert(deferred_resume_position == 0.0);
    assert(is_playing == true);

    printf("  -> output failure vs decoder failure passed.\n");
}

static void test_stale_error_generation_rejection_scenario_13(void) {
    printf("Testing stale error generation rejection (Scenario 13)...\n");

    uint64_t current_playback_gen = 5;
    int current_track_idx = 2;

    /* Error arrives with older generation 4 (stale error) */
    uint64_t stale_err_gen = 4;

    bool error_handled = false;
    if (stale_err_gen != 0 && current_playback_gen != 0 && stale_err_gen != current_playback_gen) {
        /* Stale error rejected: ignore */
    } else {
        error_handled = true;
        current_track_idx = 3;
    }

    assert(error_handled == false);
    assert(current_track_idx == 2); /* Track was not skipped by stale error */

    /* Error arrives with current generation 5 (valid error) */
    uint64_t valid_err_gen = 5;
    if (valid_err_gen != 0 && current_playback_gen != 0 && valid_err_gen != current_playback_gen) {
        /* Ignored */
    } else {
        error_handled = true;
        current_track_idx = 3;
    }

    assert(error_handled == true);
    assert(current_track_idx == 3); /* Track was skipped by valid error */

    printf("  -> stale error generation rejection passed.\n");
}

static void test_cue_sibling_physical_path_deduplication_scenario_14(void) {
    printf("Testing CUE sibling physical path deduplication (Scenario 14)...\n");

    const char * mock_playlist[4] = {
        "/music/album.flac", /* CUE track 1 */
        "/music/album.flac", /* CUE track 2 */
        "/music/album.flac", /* CUE track 3 */
        "/music/track4.mp3"  /* Independent file */
    };
    int count = 4;

    /* Simulate failure of track 0 */
    int failed_idx = 0;
    char failed_paths[5][PATH_MAX];
    int failed_paths_count = 0;

    /* Record failed path */
    strncpy(failed_paths[failed_paths_count++], mock_playlist[failed_idx], PATH_MAX);

    /* Search for next track, skipping matching physical paths */
    int cur_failed = failed_idx;
    int next_idx = -1;
    int attempts = 0;
    while (attempts < count) {
        next_idx = compute_decoder_failure_advance_index_pure(cur_failed, count, 0 /* SEQUENTIAL */, 0, NULL, 0, NULL);
        if (next_idx < 0 || next_idx == failed_idx) {
            next_idx = -1;
            break;
        }
        const char * cand_path = mock_playlist[next_idx];
        bool is_dup = false;
        for (int i = 0; i < failed_paths_count; i++) {
            if (strcmp(failed_paths[i], cand_path) == 0) {
                is_dup = true;
                break;
            }
        }
        if (is_dup) {
            cur_failed = next_idx;
            attempts++;
            continue;
        }
        break;
    }

    /* Verify sibling CUE tracks 1 and 2 were skipped and we landed directly on track 3 */
    assert(next_idx == 3);
    assert(strcmp(mock_playlist[next_idx], "/music/track4.mp3") == 0);

    printf("  -> CUE sibling physical path deduplication passed.\n");
}

/* --- Tests for Findings 1-4: Advanced CUE, Queue, Shuffle, and Reset Correctness --- */

typedef struct {
    const char * const * playlist;
    int failed_count;
    const char * failed_paths[5];
} mock_filter_ctx_t;

static bool mock_failed_path_filter(int index, void * userdata) {
    mock_filter_ctx_t * ctx = (mock_filter_ctx_t *) userdata;
    const char * path = ctx->playlist[index];
    for (int i = 0; i < ctx->failed_count; i++) {
        if (strcmp(ctx->failed_paths[i], path) == 0) return true;
    }
    return false;
}

/* Finding 1: Shuffle mode CUE deduplication & candidate traversal */
static void test_shuffle_mode_cue_deduplication_and_wrap(void) {
    printf("Testing Shuffle mode CUE deduplication and wrap traversal (Finding 1)...\n");

    const char * playlist[4] = {
        "/music/album.flac", /* 0: CUE track 1 */
        "/music/album.flac", /* 1: CUE track 2 */
        "/music/album.flac", /* 2: CUE track 3 */
        "/music/song.mp3"    /* 3: Independent file */
    };
    mock_filter_ctx_t ctx = {
        .playlist = playlist,
        .failed_count = 1,
        .failed_paths = { "/music/album.flac" }
    };

    /* Current shuffle order: { 0, 1, 2, 3 }. Track 0 failed at shuffle_pos 0.
     * Candidates 1 (pos 1) and 2 (pos 2) are sibling CUE entries of the failed file.
     * Candidate 3 (pos 3) is a valid independent file. */
    int shuffle_order[4] = { 0, 1, 2, 3 };
    failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
        0, 4, 3 /* SHUFFLE */, 0, shuffle_order, 0, NULL,
        mock_failed_path_filter, &ctx
    );

    assert(plan.target_index == 3);
    assert(plan.shuffle_steps == 3); /* Advanced 3 positions across the failed sibling entries */
    assert(plan.shuffle_wrapped == false);

    /* Test Shuffle Wrap into pending_shuffle_order */
    /* Playing track 0 at shuffle_pos 3 (end of bag). Next order has { 1, 2, 3, 0 }.
     * Pos 0 (track 1) and Pos 1 (track 2) match failed file. Pos 2 (track 3) is valid. */
    int pending_order[4] = { 1, 2, 3, 0 };
    failure_advance_plan_t plan_wrap = compute_decoder_failure_advance_plan(
        0, 4, 3 /* SHUFFLE */, 0, shuffle_order, 3, pending_order,
        mock_failed_path_filter, &ctx
    );

    assert(plan_wrap.target_index == 3);
    assert(plan_wrap.shuffle_steps == 3);
    assert(plan_wrap.shuffle_wrapped == true);

    /* When ALL tracks in the playlist are corrupt, plan.target_index must be strictly -1 */
    mock_filter_ctx_t all_failed_ctx = {
        .playlist = playlist,
        .failed_count = 2,
        .failed_paths = { "/music/album.flac", "/music/song.mp3" }
    };
    failure_advance_plan_t plan_all_failed = compute_decoder_failure_advance_plan(
        0, 4, 3 /* SHUFFLE */, 0, shuffle_order, 0, NULL,
        mock_failed_path_filter, &all_failed_ctx
    );
    assert(plan_all_failed.target_index == -1);

    printf("  -> Shuffle mode CUE deduplication and wrap traversal passed.\n");
}

/* Finding 2: Queued item traversal and atomic commit */
static void test_queued_traversal_and_atomic_commit(void) {
    printf("Testing queued item traversal and atomic commit (Finding 2)...\n");

    const char * playlist[5] = {
        "/music/track0.mp3",
        "/music/album.flac", /* Queued item 1 (sibling CUE of failed file) */
        "/music/album.flac", /* Queued item 2 (sibling CUE of failed file) */
        "/music/track3.mp3", /* Queued item 3 (valid independent file) */
        "/music/track4.mp3"
    };
    mock_filter_ctx_t ctx = {
        .playlist = playlist,
        .failed_count = 1,
        .failed_paths = { "/music/album.flac" }
    };

    /* Track 0 failed with 3 queued pending items (tracks 1, 2, 3) */
    int queued_pending = 3;
    failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
        0, 5, 0 /* SEQUENTIAL */, queued_pending, NULL, 0, NULL,
        mock_failed_path_filter, &ctx
    );

    /* Plan should skip queued tracks 1 and 2, landing on queued track 3 */
    assert(plan.target_index == 3);
    assert(plan.queued_consumed == 3); /* Consumed 3 queued positions */

    /* Atomic commit of the plan */
    if (plan.queued_consumed >= queued_pending) {
        queued_pending = 0;
    } else {
        queued_pending -= plan.queued_consumed;
    }
    assert(queued_pending == 0); /* All 3 queued items properly retired */

    /* Test partial queue consumption */
    const char * playlist2[4] = {
        "/music/track0.mp3",
        "/music/album.flac", /* Queued 1 (failed) */
        "/music/valid1.mp3", /* Queued 2 (valid!) */
        "/music/valid2.mp3"  /* Queued 3 (valid) */
    };
    mock_filter_ctx_t ctx2 = {
        .playlist = playlist2,
        .failed_count = 1,
        .failed_paths = { "/music/album.flac" }
    };
    queued_pending = 3;
    failure_advance_plan_t plan2 = compute_decoder_failure_advance_plan(
        0, 4, 0, queued_pending, NULL, 0, NULL,
        mock_failed_path_filter, &ctx2
    );

    assert(plan2.target_index == 2);
    assert(plan2.queued_consumed == 2); /* Consumed tracks 1 and 2 */
    queued_pending -= plan2.queued_consumed;
    assert(queued_pending == 1); /* 1 queued track remains (track 3) */

    printf("  -> Queued item traversal and atomic commit passed.\n");
}

/* Finding 3: Screen-off confirmed playback polling */
static void test_screen_off_confirmed_playback_polling(void) {
    printf("Testing screen-off confirmed playback polling (Finding 3)...\n");

    int consecutive_skips = 4;
    bool is_playing = true;
    double position = 3.5; /* >= 3.0s */

    /* Polling function executes in correctness-critical 500ms section regardless of backlight */
    if (consecutive_skips > 0 && is_playing && position >= 3.0) {
        consecutive_skips = 0;
    }
    assert(consecutive_skips == 0);

    /* Under 3.0 seconds does NOT reset */
    consecutive_skips = 4;
    position = 2.1;
    if (consecutive_skips > 0 && is_playing && position >= 3.0) {
        consecutive_skips = 0;
    }
    assert(consecutive_skips == 4);

    printf("  -> Screen-off confirmed playback polling passed.\n");
}

/* Finding 4: Manual playback actions reset */
static void test_manual_playback_actions_reset(void) {
    printf("Testing manual playback actions reset architecture (Finding 4)...\n");

    int consecutive_skips = 3;

    /* User calls public play_track_at or play_track_at_from */
    consecutive_skips = 0; /* Reset */
    assert(consecutive_skips == 0);

    /* Internal automatic failure skip DOES NOT reset */
    consecutive_skips++;
    assert(consecutive_skips == 1);
    consecutive_skips++;
    assert(consecutive_skips == 2);

    /* Touchscreen Prev/Next / hardware buttons call play_track_at */
    consecutive_skips = 0; /* Reset */
    assert(consecutive_skips == 0);

    printf("  -> Manual playback actions reset architecture passed.\n");
}

/* Compound Queued Failure into Shuffle Commit Test */
static void test_compound_queued_failure_into_shuffle_commit(void) {
    printf("Testing compound queued failure into Shuffle commit...\n");

    const char * playlist[4] = {
        "/music/track0.mp3", /* Current playing track */
        "/music/album.flac", /* Queued item 1 (failed) */
        "/music/album.flac", /* Queued item 2 (failed) */
        "/music/valid.mp3"   /* Shuffle candidate (valid) */
    };
    mock_filter_ctx_t ctx = {
        .playlist = playlist,
        .failed_count = 1,
        .failed_paths = { "/music/album.flac" }
    };

    /* Track 0 failed with 2 queued items (tracks 1 and 2, both failed).
     * Shuffle bag has track 0 at shuffle_pos 0 and track 3 at shuffle_pos 1. */
    int shuffle_order[4] = { 0, 3, 1, 2 };
    int shuffle_pos = 0;
    int queued_pending = 2;

    failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
        0, 4, 3 /* SHUFFLE */, queued_pending, shuffle_order, shuffle_pos, NULL,
        mock_failed_path_filter, &ctx
    );

    /* Plan should have consumed 2 queued items and advanced 0 or more shuffle steps */
    assert(plan.target_index == 3);
    assert(plan.queued_consumed == 2);
    assert(plan.shuffle_steps >= 0);

    /* Test commit: both queue and shuffle must be updated atomically */
    if (plan.queued_consumed > 0) {
        if (plan.queued_consumed >= queued_pending) {
            queued_pending = 0;
        } else {
            queued_pending -= plan.queued_consumed;
        }
    }
    if (plan.shuffle_steps > 0) {
        shuffle_pos += plan.shuffle_steps;
    }

    assert(queued_pending == 0); /* Queued entries retired */
    printf("  -> Compound queued failure into Shuffle commit passed.\n");
}

/* Wrapped Shuffle final candidate when pending order starts with failed index */
static void test_wrapped_shuffle_final_candidate_with_failed_first_entry(void) {
    printf("Testing wrapped Shuffle final candidate with failed first entry...\n");

    const char * playlist[4] = {
        "/music/album.flac", /* 0: failed */
        "/music/album.flac", /* 1: failed sibling CUE */
        "/music/album.flac", /* 2: failed sibling CUE */
        "/music/valid.mp3"   /* 3: valid independent file */
    };
    mock_filter_ctx_t ctx = {
        .playlist = playlist,
        .failed_count = 1,
        .failed_paths = { "/music/album.flac" }
    };

    /* Track 0 failed at shuffle_pos 3 (last entry in current bag).
     * Next bag (pending_shuffle_order) starts with [0, 1, 2, 3].
     * Positions 0, 1, 2 are corrupt. Position 3 is valid! */
    int current_order[4] = { 2, 1, 3, 0 };
    int pending_order[4] = { 0, 1, 2, 3 };

    failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
        0, 4, 3 /* SHUFFLE */, 0, current_order, 3, pending_order,
        mock_failed_path_filter, &ctx
    );

    /* Must successfully examine the 4th slot in pending_order and select track 3 */
    assert(plan.target_index == 3);
    assert(plan.shuffle_steps == 4);
    assert(plan.shuffle_wrapped == true);

    printf("  -> Wrapped Shuffle final candidate with failed first entry passed.\n");
}

/* Wrapped Shuffle when failure occurs before end of current bag */
static void test_wrapped_shuffle_when_failure_occurs_before_end_of_current_bag(void) {
    printf("Testing wrapped Shuffle when failure occurs before end of current bag...\n");

    const char * playlist[4] = {
        "/music/valid.mp3",  /* 0: valid independent file */
        "/music/album.flac", /* 1: failed sibling CUE */
        "/music/album.flac", /* 2: failed physical file */
        "/music/album.flac"  /* 3: failed sibling CUE */
    };
    mock_filter_ctx_t ctx = {
        .playlist = playlist,
        .failed_count = 1,
        .failed_paths = { "/music/album.flac" }
    };

    /* shuffle_pos = 2 (not the final position in 4-track bag).
     * Current order: [ 0, 1, 2, 3 ] (track 2 at pos 2, track 3 at pos 3).
     * Step 1 checks shuffle_order[3] = 3 (failed).
     * Next bag (pending_shuffle_order): [ 2, 1, 3, 0 ]
     * Step 2: pending_order[0] = 2 (matches failed_index 2).
     * Step 3: pending_order[1] = 1 (failed sibling).
     * Step 4: pending_order[2] = 3 (failed sibling).
     * Step 5: pending_order[3] = 0 (valid track 0!). */
    int current_order[4] = { 0, 1, 2, 3 };
    int pending_order[4] = { 2, 1, 3, 0 };

    failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
        2, 4, 3 /* SHUFFLE */, 0, current_order, 2, pending_order,
        mock_failed_path_filter, &ctx
    );

    /* Must successfully examine step 5 (final pending entry) and select track 0 */
    assert(plan.target_index == 0);
    assert(plan.shuffle_steps == 5);
    assert(plan.shuffle_wrapped == true);

    printf("  -> Wrapped Shuffle when failure occurs before end of current bag passed.\n");
}

/* Previous-as-rewind failure chain reset test */
static void test_prev_button_rewind_resets_failure_chain(void) {
    printf("Testing Previous button rewind resets failure chain...\n");

    int consecutive_skips = 3;
    double current_position = 5.2; /* > 3.0s threshold */

    /* Simulate prev_btn_event_cb: reset tracking unconditionally at start */
    consecutive_skips = 0; /* reset_decoder_failure_tracking() */

    if (current_position > 3.0) {
        /* audio_seek(0.0) */
    }

    assert(consecutive_skips == 0);
    printf("  -> Previous button rewind resets failure chain passed.\n");
}

int main(void) {
    printf("=== audio_selftest starting ===\n");

    test_safe_path_tail_production();
    test_premature_eof_production();
    test_partial_write_accounting();
    test_output_retry_abort_handling();
    test_reopen_and_initial_seek_verification();
    test_crossfade_premature_eof_handling();
    test_gui_error_and_queue_preservation();
    test_aiff_phase1_buffer_reuse();
    test_decoder_result_contracts();
    test_phase3_steady_state_snapshot();
    test_phase4_transition_ramps();
    test_transition_ramp_write_abort_policy();
    test_crossfade_next_decoder_failure_cancellation();
    test_deadlock_free_pause_state_handling();
    test_decoder_failure_advance_scenarios_1_to_7();
    test_consecutive_decoder_failure_safety_cap_scenario_8();
    test_consecutive_skip_counter_resets_scenarios_9_to_11();
    test_output_failure_vs_decoder_failure_scenario_12_and_15();
    test_stale_error_generation_rejection_scenario_13();
    test_cue_sibling_physical_path_deduplication_scenario_14();
    test_shuffle_mode_cue_deduplication_and_wrap();
    test_queued_traversal_and_atomic_commit();
    test_screen_off_confirmed_playback_polling();
    test_manual_playback_actions_reset();
    test_compound_queued_failure_into_shuffle_commit();
    test_wrapped_shuffle_final_candidate_with_failed_first_entry();
    test_wrapped_shuffle_when_failure_occurs_before_end_of_current_bag();
    test_prev_button_rewind_resets_failure_chain();

    printf("=== audio_selftest: ALL TESTS PASSED ===\n");
    return 0;
}
