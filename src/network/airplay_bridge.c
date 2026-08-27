#include "airplay_bridge.h"
#include "airplay_metadata.h"
#include "audio.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HOST_BUILD
  #include <poll.h>
  #include "audio_output.h"
#endif

/* Fixed by the AirPlay 1/RAOP protocol itself -- see airplay_bridge.h's own
 * comment. 4096 frames (~93ms at 44100 Hz) matches this codebase's other
 * bridge's period-sized reads (usb_dac_bridge.c's own BRIDGE_PERIOD_FRAMES)
 * in spirit -- large enough to keep read()/write() call overhead low,
 * small enough to keep latency reasonable for a receive-mode use case that
 * was never going to be sample-accurate anyway. */
#define BRIDGE_CHANNELS 2
#define BRIDGE_SAMPLE_RATE 44100
#define BRIDGE_PERIOD_FRAMES 4096
#define FRAME_BYTES ((size_t) BRIDGE_CHANNELS * sizeof(int16_t))

/* Shortened from an original 200ms specifically to bound this bridge's own
 * contribution to the (still real, still not fully closed -- see
 * airplay_bridge_stop()'s own comment) race window against a NEW consumer
 * of the shared, lock-free audio_output singleton acquiring it before this
 * bridge's dying thread has actually called audio_output_close(). Does not
 * meaningfully change idle battery cost -- poll() still blocks in the
 * kernel either way, this only changes how often it wakes to recheck
 * stop_requested. */
#define POLL_INTERVAL_MS 50

typedef enum {
    BRIDGE_STOPPED = 0,
    BRIDGE_RUNNING,
    BRIDGE_STOPPING,
} bridge_state_t;

static pthread_mutex_t bridge_mutex = PTHREAD_MUTEX_INITIALIZER;
static bridge_state_t bridge_state = BRIDGE_STOPPED;
static bool restart_requested = false;
static volatile bool stop_requested = false;

/* True only while real PCM is actually flowing through the current session
 * -- see airplay_bridge_start()'s own doc comment for why this is tracked
 * separately from bridge_state (RUNNING means "listening/ready", not
 * "actively using the device"). Guarded by bridge_mutex like everything
 * else here rather than made atomic -- same deliberate, codebase-wide
 * volatile-bool convention as stop_requested, see this file's history for
 * why that tradeoff was made rather than introducing a different
 * synchronization primitive just for these two files. */
static bool is_streaming = false;

static void set_streaming(bool streaming) {
    pthread_mutex_lock(&bridge_mutex);
    is_streaming = streaming;
    pthread_mutex_unlock(&bridge_mutex);
}

bool airplay_bridge_is_streaming(void) {
    pthread_mutex_lock(&bridge_mutex);
    bool streaming = is_streaming;
    pthread_mutex_unlock(&bridge_mutex);
    return streaming;
}

bool airplay_bridge_is_stopped(void) {
    pthread_mutex_lock(&bridge_mutex);
    bool stopped = (bridge_state == BRIDGE_STOPPED);
    pthread_mutex_unlock(&bridge_mutex);
    return stopped;
}

#ifndef HOST_BUILD
static void run_session(uint8_t * buf, size_t buf_bytes, uint64_t * total_bytes_read) {
    /* Carries 1-3 leftover bytes across read() calls within one streaming
     * run -- a pipe read() is NOT guaranteed to land on a 4-byte (stereo
     * s16) frame boundary just because shairport's own writes happen to be
     * frame-aligned; the kernel pipe layer doesn't preserve write-sized
     * boundaries at all. Dropping a stray remainder instead of carrying it
     * would silently and permanently misalign every sample for the rest of
     * the stream, not just glitch once. Reset whenever streaming actually
     * (re)starts, not merely on a fresh FIFO open, since a fresh open no
     * longer implies a fresh audio stream (see the listening/streaming
     * split below). */
    uint8_t leftover[FRAME_BYTES];
    size_t leftover_len = 0;

    /* Outer loop: one iteration per shairport connection/writer. A FIFO fd
     * that has already seen EOF (every writer closed) keeps reporting EOF
     * forever even after a NEW writer later opens the same path -- only a
     * fresh open() picks up the next session, so a normal (non-stop)
     * writer disconnect re-enters this loop instead of ending the bridge,
     * letting AirPlay stay "on" across multiple connect/play/disconnect
     * cycles from the phone without the user re-toggling the setting. */
    while (!stop_requested) {
        /* O_NONBLOCK so this open() itself can never block indefinitely
         * waiting for shairport to open its own end -- the poll() loop
         * below is what actually waits, and (like every other bridge/
         * daemon loop in this codebase) re-checks stop_requested every
         * POLL_INTERVAL_MS instead of blocking uninterruptibly.
         * airplay_control_start() guarantees the FIFO node itself exists
         * (mkfifo()) before this thread is ever started. */
        int fifo_fd = open(AIRPLAY_FIFO_PATH, O_RDONLY | O_NONBLOCK);
        if (fifo_fd < 0) {
            if (stop_requested) break;
            usleep(POLL_INTERVAL_MS * 1000);
            continue;
        }

        /* LISTENING for this fifo_fd until the first real byte arrives --
         * deliberately does NOT touch audio.c/audio_output at all yet. This
         * is the crux of the real-device UX fix: toggling AirPlay on (which
         * gets this thread and shairport itself running) must not
         * interrupt whatever is already playing locally; only an actual
         * incoming stream should, and only once it genuinely starts. */
        bool session_active = false;
        leftover_len = 0;

        bool stopped_mid_session = false;
        while (!stop_requested) {
            struct pollfd pfd = { .fd = fifo_fd, .events = POLLIN };
            int pr = poll(&pfd, 1, POLL_INTERVAL_MS);
            if (pr <= 0) continue; /* timeout or poll() error -- recheck stop_requested */

            ssize_t n = read(fifo_fd, buf + leftover_len, buf_bytes - leftover_len);
            if (n < 0) {
                if (errno == EAGAIN) continue; /* nothing ready despite POLLIN -- keep polling */
                break; /* genuine read error -- close and retry fresh below */
            }
            if (n == 0) break; /* every writer closed -- shairport session ended */

            if (!session_active) {
                /* First real bytes of this connection -- transition from
                 * LISTENING to actually STREAMING. Symmetric with local
                 * playback's own airplay_control_disconnect_active_stream()
                 * (airplay_control.h), which gives local playback priority
                 * the other direction: whichever side starts second wins,
                 * neither permanently disables the other. Same reasoning
                 * as usb_dac_bridge.c's own wait -- audio_stop() only
                 * signals the audio thread; audio_output_close() happens
                 * asynchronously on that thread, so wait for it to actually
                 * finish before touching the shared output device
                 * ourselves, or audio_output_ensure() below can fail the
                 * same "Resource busy" way this project has already hit
                 * once (see subprocess.c's close_inherited_fds() doc
                 * comment). */
                audio_stop();
                for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
                    if (!audio_is_playing() && !audio_is_paused()) break;
                    usleep(20000);
                }
                set_streaming(true);
                session_active = true;
                leftover_len = 0;
            }

            *total_bytes_read += (uint64_t) n;
            /* Real-time source, no local decoder buffering ahead of this --
             * see audio_output_ensure()'s low_latency parameter doc comment
             * for why the standard local-playback tuning adds needless
             * latency here, and the bug report this fixes. */
            audio_output_ensure(BRIDGE_CHANNELS, BRIDGE_SAMPLE_RATE, true);

            size_t total = leftover_len + (size_t) n;
            size_t frame_count = total / FRAME_BYTES;
            size_t used_bytes = frame_count * FRAME_BYTES;

            /* Ignore write failure: no per-frame recovery here, same as
             * usb_dac_bridge.c -- audio_output_ensure() at the top of the
             * next chunk handles reconnection if the output device died. */
            (void) audio_output_write((int16_t *) buf, frame_count, BRIDGE_CHANNELS, NULL);

            size_t remainder = total - used_bytes;
            if (remainder > 0) memmove(leftover, buf + used_bytes, remainder);
            leftover_len = remainder;
            if (leftover_len > 0) memcpy(buf, leftover, leftover_len);
        }
        if (stop_requested) stopped_mid_session = true;

        if (session_active) {
            /* Stream ended (writer closed, error, or a stop was requested
             * mid-stream) -- release the device immediately rather than
             * waiting for the whole bridge to fully stop, so local
             * playback can reclaim it the moment the phone's own stream
             * ends, not only when the user later toggles AirPlay off. */
            audio_output_close();
            /* Invalidate metadata BEFORE flipping is_streaming false, not
             * after -- gui_network_poll_airplay_overlay() reads is_
             * streaming() and airplay_metadata_consume_update() as two
             * independent, unsynchronized calls. If set_streaming(false)
             * ran first, the UI thread could observe streaming==false and
             * still consume a not-yet-invalidated `pending` update from
             * THIS session in the same tick (the overlay poll applies
             * metadata regardless of streaming, see its own comment) --
             * displaying this session's stale content on a screen with no
             * active stream, or worse, into the next one before it gets its
             * own update. Invalidating first means any update the UI can
             * possibly see once it observes streaming==false has already
             * gone through generation-checked publish_pending() with the
             * new generation, or been dropped by free_pending_locked() here.
             *
             * airplay_metadata.c's own reader has no notion of PCM session
             * boundaries and keeps running across many of these without
             * airplay_metadata_stop() ever being called (that only happens
             * on a full airplay_control_stop()), so without this call a
             * slow artwork decode for the session that just ended could
             * still publish afterward and surface on whatever reconnects
             * next. Harmless to call even when nothing was actually pending
             * or mid-decode. */
            airplay_metadata_invalidate();
            set_streaming(false);
        }

        close(fifo_fd);
        if (stopped_mid_session) break;
        /* Otherwise: writer disconnected normally -- loop back to
         * LISTENING and wait for the next one. */
    }
}

static void * bridge_thread_func(void * arg) {
    (void) arg;

    size_t buf_bytes = (size_t) BRIDGE_PERIOD_FRAMES * FRAME_BYTES;
    uint8_t * buf = malloc(buf_bytes);
    if (!buf) {
        /* buf_bytes is a fixed ~16KB -- this is not expected to ever
         * actually fail on this target, but a NULL buf would otherwise
         * reach read(fifo_fd, buf + leftover_len, ...) as a wild pointer.
         * Fail safe: log, reset state, and never touch the FIFO/output
         * device at all this run rather than crash. */
        fprintf(stderr, "airplay_bridge: buffer allocation failed -- AirPlay audio will not work this session\n");
        pthread_mutex_lock(&bridge_mutex);
        bridge_state = BRIDGE_STOPPED;
        pthread_mutex_unlock(&bridge_mutex);
        return NULL;
    }
    uint64_t total_bytes_read = 0;

    /* Restart-in-place loop: airplay_bridge_start() sets restart_requested
     * instead of spawning a second thread when called while this one is
     * still in BRIDGE_STOPPING (see that function's own comment for the
     * real bug this fixes -- a rapid off-then-on used to silently no-op,
     * leaving a freshly spawned shairport with no reader at all). Treating
     * a requested restart as "run another session in this same thread"
     * rather than "spawn a new one" means there is never a window where
     * two bridge threads could exist, or where bridge_state could say
     * STOPPED/STOPPING while a shairport process is actually running with
     * nothing reading its FIFO. */
    for (;;) {
        run_session(buf, buf_bytes, &total_bytes_read);

        pthread_mutex_lock(&bridge_mutex);
        if (restart_requested) {
            restart_requested = false;
            stop_requested = false;
            pthread_mutex_unlock(&bridge_mutex);
            fprintf(stderr, "airplay_bridge: restarting in place (%llu bytes read so far)\n",
                    (unsigned long long) total_bytes_read);
            continue;
        }
        bridge_state = BRIDGE_STOPPED;
        pthread_mutex_unlock(&bridge_mutex);
        break;
    }

    fprintf(stderr, "airplay_bridge: stopped, %llu bytes read total\n", (unsigned long long) total_bytes_read);

    free(buf);
    /* No trailing audio_output_close() here -- run_session() already closes
     * the device itself the instant a streaming run ends (see its own
     * "if (session_active)" block), covering every path this thread can
     * exit through. A call here used to run unconditionally as "belt and
     * suspenders", but it could only ever legitimately have something to
     * close if session_active was true when the LAST run_session() call
     * returned, and that path already closed it. If this thread stopped
     * while merely LISTENING (session_active never became true this run),
     * the bridge itself never touched audio_output at all -- but by the
     * time this line ran, bridge_state had already flipped to BRIDGE_
     * STOPPED, so an unrelated consumer (most exposed: local playback
     * starting immediately after the user turns AirPlay off while idle)
     * could already have opened the shared output in that window. Closing
     * it here regardless would then tear that down out from under them --
     * a real, previously-unguarded race, not a hypothetical one. */
    return NULL;
}
#endif

bool airplay_bridge_start(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&bridge_mutex);
    if (bridge_state == BRIDGE_RUNNING) {
        pthread_mutex_unlock(&bridge_mutex);
        return true;
    }
    if (bridge_state == BRIDGE_STOPPING) {
        /* The previous thread hasn't noticed stop_requested yet (or is
         * mid-teardown) -- tell it to restart itself once it gets there
         * rather than racing a second pthread_create() against a
         * bridge_state the old thread hasn't cleared yet. See
         * bridge_thread_func()'s own comment for the bug this fixes. */
        restart_requested = true;
        pthread_mutex_unlock(&bridge_mutex);
        return true;
    }
    bridge_state = BRIDGE_RUNNING;
    stop_requested = false;
    restart_requested = false;
    pthread_mutex_unlock(&bridge_mutex);

    pthread_t thread;
    if (pthread_create(&thread, NULL, bridge_thread_func, NULL) != 0) {
        /* Reset state immediately rather than leaving bridge_state stuck at
         * RUNNING forever with no thread to ever clear it -- airplay_
         * control_start() has already created the FIFO by the time this is
         * called (see its own comment on the ordering, and on why it no
         * longer spawns shairport when this returns false). */
        pthread_mutex_lock(&bridge_mutex);
        bridge_state = BRIDGE_STOPPED;
        pthread_mutex_unlock(&bridge_mutex);
        fprintf(stderr, "airplay_bridge: pthread_create failed -- AirPlay audio will not work this session\n");
        return false;
    }
    pthread_detach(thread);
    return true;
#else
    return true;
#endif
}

void airplay_bridge_stop(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&bridge_mutex);
    if (bridge_state != BRIDGE_RUNNING) {
        /* Nothing to stop (already stopped, or a stop is already in
         * flight) -- also correctly absorbs a redundant stop() call
         * without clobbering a pending restart_requested. */
        pthread_mutex_unlock(&bridge_mutex);
        return;
    }
    bridge_state = BRIDGE_STOPPING;
    pthread_mutex_unlock(&bridge_mutex);

    stop_requested = true;

    /* Deliberately does NOT block waiting for the thread to actually exit,
     * unlike usb_dac_bridge_stop()'s own bounded poll loop -- that one is
     * safe to block on because its only caller (usb_mode_control_apply())
     * already runs on a background thread. Every airplay_control_stop()
     * call site (gui_network.c) runs directly on the UI/LVGL thread as an
     * lv_event_t callback, and none of them need the audio device back
     * synchronously before their own next line -- they just flip a
     * setting and refresh a screen. Blocking here up to 3s (as a first
     * cut of this code did) would freeze the whole UI for that long every
     * time AirPlay is turned off. The thread still cleans up (closes the
     * FIFO fd, audio_output_close(), clears bridge_state) on its own
     * shortly after this returns -- just not synchronously guaranteed by
     * the time the caller's next line runs.
     *
     * Known residual risk, not fully closed by this design: audio_output.c
     * itself has no lock at all -- it relies on "only one consumer active
     * at a time" as a convention, not an enforced invariant, and this
     * non-blocking stop means the dying thread can still be mid audio_
     * output_ensure()/_write() for up to roughly POLL_INTERVAL_MS after
     * this call returns, IF it was actually streaming when stopped. USB
     * DAC mode has its own ~2s wait before it first touches audio_output
     * (usb_dac_bridge.c), which in practice dwarfs that window.
     *
     * The specific most-exposed path -- local playback resuming immediately
     * after this -- is narrowed at the caller level, not here: airplay_
     * control_disconnect_active_stream() (airplay_control.h), the only
     * caller that hands the device straight to local playback right after
     * stopping this bridge, polls airplay_bridge_is_stopped() with a
     * bounded (2s) wait before proceeding. In the ordinary case that wait
     * succeeds well within its bound (bridge_state normally reaches BRIDGE_
     * STOPPED within roughly one POLL_INTERVAL_MS, and only reaches it after
     * run_session() has already closed the device itself, see bridge_
     * thread_func()'s own comment), so by the time that caller returns, this
     * thread has, in practice, already released the device. That is a
     * bounded wait, NOT a guarantee: if the bridge thread is somehow still
     * not reporting stopped after the full 2s (would require it to be
     * genuinely wedged, not just slow), that caller proceeds to resume local
     * playback anyway rather than hang indefinitely -- logging the
     * exceptional case rather than eliminating the resulting race, so this
     * is narrowed to a near-impossible-in-practice window, not closed to
     * zero. A plain settings-screen toggle-off (airplay_toggle_cb()) still
     * doesn't wait at all -- nothing there needs the device back
     * synchronously, and blocking it would freeze the UI for no benefit; a
     * user manually toggling AirPlay off and then tapping Play within
     * roughly one POLL_INTERVAL_MS of each other (sub-50ms, not achievable
     * by human reaction time) is a second, separate, un-narrowed residual
     * through that path. Closing either residual fully would need a shared
     * ownership-arbitration mechanism across audio.c/usb_dac_bridge.c/this
     * file -- a larger, separate change than this bridge's own files can
     * safely make alone. */
#endif
}
