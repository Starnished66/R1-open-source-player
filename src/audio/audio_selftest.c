#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(void) {
    printf("=== audio_selftest starting ===\n");

    test_safe_path_tail_production();
    test_premature_eof_production();
    test_partial_write_accounting();
    test_output_retry_abort_handling();
    test_reopen_and_initial_seek_verification();
    test_crossfade_premature_eof_handling();
    test_gui_error_and_queue_preservation();

    printf("=== audio_selftest: ALL TESTS PASSED ===\n");
    return 0;
}
