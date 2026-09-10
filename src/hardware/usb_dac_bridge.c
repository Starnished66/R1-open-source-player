#include "usb_dac_bridge.h"

#include "audio.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef HOST_BUILD
  #include <poll.h>
  #include "audio_output.h"
#endif

/* Persistent diagnostic log written to .logs/usb_dac_bridge.log on the SD card
 * and mirrored to stderr, gated by the shared developer options toggle
 * (Settings -> About -> Developer Options -> "Enable database logging").
 * Flushed and fsync()'d after every line -- not just fflush(), which only
 * reaches the kernel page cache -- so evidence actually reaches the SD card
 * before an unrecoverable hang or hard reboot, and rotated to .log.1 past
 * BRIDGE_LOG_MAX_BYTES. */
#include "db_log.h"
#include <stdarg.h>
#include <sys/stat.h>

#ifdef HOST_BUILD
#define BRIDGE_LOG_DIR "./music/.logs"
#else
#define BRIDGE_LOG_DIR "/data/mnt/sd_0/.logs"
#endif
#define BRIDGE_LOG_PATH BRIDGE_LOG_DIR "/usb_dac_bridge.log"
#define BRIDGE_LOG_ROTATED_PATH BRIDGE_LOG_DIR "/usb_dac_bridge.log.1"
#define BRIDGE_LOG_MAX_BYTES (2L * 1024 * 1024)

static pthread_mutex_t bridge_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE * bridge_log_file = NULL;

/* Must be called with bridge_log_mutex held and bridge_log_file non-NULL. */
static void bridge_log_rotate_if_needed_locked(void) {
    if (fseek(bridge_log_file, 0, SEEK_END) != 0) return;
    long size = ftell(bridge_log_file);
    if (size < BRIDGE_LOG_MAX_BYTES) return;
    fclose(bridge_log_file);
    bridge_log_file = NULL;
    remove(BRIDGE_LOG_ROTATED_PATH);
    rename(BRIDGE_LOG_PATH, BRIDGE_LOG_ROTATED_PATH);
    bridge_log_file = fopen(BRIDGE_LOG_PATH, "w");
}

void usb_dac_bridge_set_debug_log_enabled(bool enabled) {
    if (!enabled) {
        pthread_mutex_lock(&bridge_log_mutex);
        if (bridge_log_file) {
            fflush(bridge_log_file);
            fclose(bridge_log_file);
            bridge_log_file = NULL;
        }
        pthread_mutex_unlock(&bridge_log_mutex);
    }
}

static void bridge_log(const char * fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (!db_log_enabled()) return;

    pthread_mutex_lock(&bridge_log_mutex);
    if (!db_log_enabled()) {
        if (bridge_log_file) {
            fflush(bridge_log_file);
            fclose(bridge_log_file);
            bridge_log_file = NULL;
        }
        pthread_mutex_unlock(&bridge_log_mutex);
        return;
    }

    if (!bridge_log_file) {
        if (mkdir(BRIDGE_LOG_DIR, 0755) != 0 && errno != EEXIST) {
            pthread_mutex_unlock(&bridge_log_mutex);
            return;
        }
        bridge_log_file = fopen(BRIDGE_LOG_PATH, "a");
        if (!bridge_log_file) {
            pthread_mutex_unlock(&bridge_log_mutex);
            return;
        }
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ms = (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
    fprintf(bridge_log_file, "[usb_dac_bridge] t=%llu ", (unsigned long long) now_ms);

    va_start(ap, fmt);
    vfprintf(bridge_log_file, fmt, ap);
    va_end(ap);
    fflush(bridge_log_file);
    /* fflush() only moves data from libc's buffer to the kernel page cache --
     * not necessarily to physical SD storage. This log exists specifically
     * to survive a hard power-cycle (the only recovery from a stuck USB mode
     * switch with no ADB access), so the most recent lines are exactly the
     * ones that must not be lost to write-back timing; fsync() forces them
     * out to the device before returning.
     * Throttled to at most once/second rather than every call: this runs on
     * the reader thread, which must keep up with incoming USB audio, and
     * fsync() is a blocking device write -- confirmed on real hardware
     * (2026-09-10) as a direct cause of periodic audible crackling when
     * called on every one of this device's routine ~9-15ms EOF/reopen
     * cycles (hundreds of times/second). Worst-case durability cost: up to
     * ~1s of the most recent lines lost on a genuine hard power-cut,
     * accepted given this log's own purpose is post-mortem debugging of a
     * stuck USB mode, not sub-second forensic precision. */
    static uint64_t last_fsync_ms = 0;
    if (now_ms - last_fsync_ms >= 1000) {
        fsync(fileno(bridge_log_file));
        last_fsync_ms = now_ms;
    }
    bridge_log_rotate_if_needed_locked();
    pthread_mutex_unlock(&bridge_log_mutex);
}
#define BRIDGE_LOG(...) bridge_log(__VA_ARGS__)

#define UAC_SA_DEVICE_PATH "/dev/uac_sa"
/* Channels: of the two interleaved 16-bit slots per frame, only the second
 * one (index 1) ever carries real audio -- the first is full-scale noise on
 * every capture, not silence. Verified by comparing zero-crossing rate/
 * amplitude of each slot in a raw capture, and that a byte-level shift
 * doesn't fix it (rules out a framing offset -- it's a per-slot behavior,
 * not misalignment). Worked around by overwriting the noise slot with the
 * real one before playback rather than declaring 1 channel, since
 * duplicating into fake stereo is simpler than reconfiguring tinyalsa/the
 * hardware path for mono. This is unrelated to sample rate and applies
 * regardless of which rate is actually negotiated. */
#define BRIDGE_CHANNELS 2
#define BRIDGE_BIT_DEPTH 16
#define USB_ADVERTISED_SAMPLE_RATE 48000
#define USB_ADVERTISED_BIT_DEPTH 16
#define BRIDGE_PERIOD_FRAMES 1024

#define OPEN_RETRY_TIMEOUT_MS 5000
#define POLL_INTERVAL_MS 200

/* Bounded mute fallback timeout: if the reader never confirms a sample rate,
 * start playback at the default/last confirmed rate anyway to avoid indefinite
 * silence. 5 seconds is a conservative starting point matching this file's
 * other bounded timeouts (e.g. RECOVERY_TIMEOUT_NS), not a hardware-validated number. */
#ifndef MUTE_FALLBACK_TIMEOUT_NS
#define MUTE_FALLBACK_TIMEOUT_NS (5ULL * 1000000000ULL)
#endif
#ifdef TEST_MUTE_FALLBACK_TIMEOUT_OVERRIDE
static uint64_t g_test_mute_fallback_timeout_ns = 0;
#define EFFECTIVE_MUTE_FALLBACK_TIMEOUT_NS (g_test_mute_fallback_timeout_ns > 0 ? g_test_mute_fallback_timeout_ns : MUTE_FALLBACK_TIMEOUT_NS)
#else
#define EFFECTIVE_MUTE_FALLBACK_TIMEOUT_NS MUTE_FALLBACK_TIMEOUT_NS
#endif

/* Real incoming frame rate is NOT trustworthy from the declared USB
 * descriptor alone, and is NOT a fixed constant either -- a real macOS
 * host was observed to genuinely stream at 48000 (the declared rate),
 * producing correct-speed audio only once the local output was reconfigured
 * to match, whereas an earlier empirical measurement against a different
 * host (Windows/Linux) converged on ~96000 and sounded correct there. This
 * gadget's clock source descriptor covers a wide range (32kHz-384kHz per
 * the vendor kernel's own iClockSource string), so different hosts
 * genuinely appear to negotiate different rates -- there is no single safe
 * hardcoded constant. The bridge instead measures the real incoming
 * byte rate continuously (RATE_MEASURE_WINDOW_MS per sample) and only
 * commits a reconfiguration once the same standard rate is measured
 * RATE_CONFIRM_COUNT times in a row, snapping to the nearest of the
 * standard UAC rates below rather than trusting raw jitter -- avoids
 * reconfiguring ALSA on every minor timing wobble or a single glitched
 * window, while still adapting correctly to whatever a given host actually
 * sends instead of assuming any one value is universally correct. */
static const unsigned int STANDARD_RATES[] = {
    32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 352800, 384000
};
#define RATE_MEASURE_WINDOW_MS 250
#define RATE_CONFIRM_COUNT 3

/* Below this, a measurement is treated as a corrupted/implausible outlier
 * rather than real audio, and the window is discarded without touching
 * pending_rate/pending_rate_count -- confirmed on real hardware (2026-09-10):
 * a window whose elapsed wall-clock time was inflated by a stall (a brief
 * gap, or this file's own synchronous per-line log fsync()) can read bytes/
 * elapsed far below the true rate, since a stall can only ever bias this
 * measurement DOWNWARD (undercounting real bytes against inflated elapsed
 * time), never upward. Without this check, snap_to_standard_rate() below
 * still picks the NEAREST standard rate for any input, including obvious
 * garbage (e.g. measured ~2-8kHz snapping to 32000 Hz, then getting
 * confirmed and adopted -- observed on real hardware, played unchanged
 * 48kHz PCM at 32kHz, an audible two-thirds-speed pitch drop). Set well
 * under the lowest real STANDARD_RATES entry (32000, >25% below it) so
 * ordinary jitter on genuine 32kHz audio won't cross it -- a real stream
 * would need an unusually severe stall of its own to read this low, while a
 * stall-corrupted reading this far below any
 * real device rate is. */
#define RATE_MEASURE_MIN_PLAUSIBLE_HZ 24000.0

static unsigned int snap_to_standard_rate(double measured_rate) {
    unsigned int best = STANDARD_RATES[0];
    double best_diff = -1.0;
    for (size_t i = 0; i < sizeof(STANDARD_RATES) / sizeof(STANDARD_RATES[0]); i++) {
        double diff = measured_rate - (double) STANDARD_RATES[i];
        if (diff < 0) diff = -diff;
        if (best_diff < 0 || diff < best_diff) {
            best_diff = diff;
            best = STANDARD_RATES[i];
        }
    }
    return best;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* Escalating backoff ladder for repeat hits (EOF or revents hangup/error) within
 * an active recovery episode, keyed off eof_reopen_attempts (number of open()s
 * attempted so far during the current episode):
 *  - 1st repeat hit (attempts == 1): 0 ms (immediate retry; confirmed normal fast path)
 *  - 2nd repeat hit (attempts == 2): 2 ms
 *  - 3rd repeat hit (attempts == 3): 5 ms
 *  - 4th repeat hit (attempts == 4): 10 ms
 *  - 5th to 10th    (attempts 5..10): 10 ms (hold tier for unusual recovery)
 *  - > 10           (attempts > 10): POLL_INTERVAL_MS (200 ms fallback for prolonged failure)
 */
static unsigned int repeat_hit_backoff_ms(int attempts_so_far) {
    if (attempts_so_far <= 1) {
        return 0;
    } else if (attempts_so_far == 2) {
        return 2;
    } else if (attempts_so_far == 3) {
        return 5;
    } else if (attempts_so_far <= 10) {
        return 10;
    } else {
        return POLL_INTERVAL_MS;
    }
}

static pthread_mutex_t bridge_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool bridge_running = false;
static bool bridge_streaming = false;
static unsigned int active_output_rate = USB_ADVERTISED_SAMPLE_RATE;
static atomic_bool stop_requested = false;

#ifndef HOST_BUILD
/* Bounded circular byte buffer connecting the reader and writer threads.
 *
 * Buffer Capacity Rationale:
 * At the maximum supported rate (384 kHz, stereo 16-bit = 1,536,000 bytes/sec),
 * 512 KiB stores ~350 ms of audio; at standard 48 kHz (192,000 bytes/sec),
 * it stores ~2.8 seconds of audio. This easily absorbs realistic ALSA write
 * latencies (e.g. buffer draining, thread scheduling, Bluetooth aplay pipe
 * latency) across the full 32kHz-384kHz range without overflowing during
 * normal operation. On this device with >64MB RAM, 512 KiB is a lightweight,
 * trivial allocation.
 *
 * Overflow Policy:
 * When full, DROP THE OLDEST data in the ring buffer to accommodate new incoming
 * USB packets. The reader thread NEVER blocks on buffer full, ensuring the read()
 * cadence and sample rate measurement directly mirror the host's actual USB
 * data transmission rate without playback backpressure interference.
 */
#define RING_BUFFER_CAPACITY (512 * 1024)

typedef struct {
    uint8_t * data;
    size_t capacity;
    size_t head; /* write position */
    size_t tail; /* read position */
    size_t count;
    bool producer_finished;
    bool initialized;
    pthread_mutex_t mutex;
    pthread_cond_t cond_data_available;
    uint64_t dropped_bytes_total;
    uint64_t last_drop_log_ns;
} ring_buffer_t;

/* Serializes complete start/stop operations. Workers never take this mutex;
 * bridge_mutex must not be held while joining them. */
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static ring_buffer_t g_rb;
static bool g_reader_running = false;
static bool g_writer_running = false;
static pthread_t g_reader_thread;
static pthread_t g_writer_thread;
/* Owned by lifecycle_mutex; cleared only after a successful join. */
static bool g_reader_thread_created = false;
static bool g_writer_thread_created = false;
static unsigned int g_confirmed_rate = USB_ADVERTISED_SAMPLE_RATE;
static bool g_rate_confirmed_once = false;

static bool ring_buffer_init(ring_buffer_t * rb, size_t capacity) {
    rb->data = malloc(capacity);
    if (!rb->data) {
        rb->capacity = 0;
        rb->head = 0;
        rb->tail = 0;
        rb->count = 0;
        rb->producer_finished = false;
        rb->initialized = false;
        rb->dropped_bytes_total = 0;
        rb->last_drop_log_ns = 0;
        return false;
    }
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->producer_finished = false;
    rb->initialized = true;
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->cond_data_available, NULL);
    rb->dropped_bytes_total = 0;
    rb->last_drop_log_ns = 0;
    return true;
}

static void ring_buffer_signal_finished(ring_buffer_t * rb) {
    if (!rb->initialized) return;
    pthread_mutex_lock(&rb->mutex);
    rb->producer_finished = true;
    pthread_cond_broadcast(&rb->cond_data_available);
    pthread_mutex_unlock(&rb->mutex);
}

static void ring_buffer_destroy(ring_buffer_t * rb) {
    if (!rb->initialized) return;
    pthread_mutex_lock(&rb->mutex);
    if (rb->data) {
        free(rb->data);
        rb->data = NULL;
    }
    rb->capacity = 0;
    rb->count = 0;
    rb->head = 0;
    rb->tail = 0;
    rb->producer_finished = false;
    rb->initialized = false;
    pthread_mutex_unlock(&rb->mutex);
    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->cond_data_available);
}

/* Pushes incoming bytes into the ring buffer. If buffer would overflow,
 * drops the oldest unread data to make room, never blocking the reader. */
static void ring_buffer_write_drop_oldest(ring_buffer_t * rb, const uint8_t * src, size_t len) {
    if (len == 0 || rb->capacity == 0) return;

    uint64_t dropped_bytes_to_log = 0;
    pthread_mutex_lock(&rb->mutex);
    if (!rb->data) {
        pthread_mutex_unlock(&rb->mutex);
        return;
    }

    if (len > rb->capacity) {
        /* Incoming chunk larger than entire capacity (pathological): keep the newest suffix */
        size_t original_len = len;
        src += (len - rb->capacity);
        len = rb->capacity;

        size_t dropped_oversized = original_len - len;
        size_t previously_buffered = rb->count;
        rb->dropped_bytes_total += (uint64_t)(dropped_oversized + previously_buffered);
        rb->head = 0;
        rb->tail = 0;
        rb->count = 0;

        uint64_t now_ns = monotonic_ns();
        if (now_ns - rb->last_drop_log_ns >= 5000000000ULL) { /* rate-limit to once per ~5s */
            dropped_bytes_to_log = rb->dropped_bytes_total;
            rb->last_drop_log_ns = now_ns;
        }
    }

    size_t space = rb->capacity - rb->count;
    if (len > space) {
        size_t bytes_to_drop = len - space;
        rb->tail = (rb->tail + bytes_to_drop) % rb->capacity;
        rb->count -= bytes_to_drop;
        rb->dropped_bytes_total += (uint64_t) bytes_to_drop;

        uint64_t now_ns = monotonic_ns();
        if (now_ns - rb->last_drop_log_ns >= 5000000000ULL) { /* rate-limit to once per ~5s */
            dropped_bytes_to_log = rb->dropped_bytes_total;
            rb->last_drop_log_ns = now_ns;
        }
    }

    /* Copy src into rb->data at head (handling wrap-around) */
    size_t first_chunk = rb->capacity - rb->head;
    if (len <= first_chunk) {
        memcpy(rb->data + rb->head, src, len);
    } else {
        memcpy(rb->data + rb->head, src, first_chunk);
        memcpy(rb->data, src + first_chunk, len - first_chunk);
    }
    rb->head = (rb->head + len) % rb->capacity;
    rb->count += len;

    pthread_cond_signal(&rb->cond_data_available);
    pthread_mutex_unlock(&rb->mutex);
    if (dropped_bytes_to_log != 0) {
        BRIDGE_LOG("usb_dac_bridge: ring buffer overflow, dropped %llu bytes total (writer falling behind)\n",
                (unsigned long long) dropped_bytes_to_log);
    }
}

/* Reads up to max_len bytes from the ring buffer into dst.
 * Blocks on cond_data_available until data is available, stop_requested is set,
 * or the producer has finished.
 * Returns number of bytes read. */
static size_t ring_buffer_read(ring_buffer_t * rb, uint8_t * dst, size_t max_len) {
    if (max_len == 0) return 0;

    pthread_mutex_lock(&rb->mutex);
    while (rb->count == 0 && !stop_requested && !rb->producer_finished) {
        pthread_cond_wait(&rb->cond_data_available, &rb->mutex);
    }

    if (rb->count == 0 || !rb->data) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    size_t to_read = (rb->count < max_len) ? rb->count : max_len;
    size_t first_chunk = rb->capacity - rb->tail;
    if (to_read <= first_chunk) {
        memcpy(dst, rb->data + rb->tail, to_read);
    } else {
        memcpy(dst, rb->data + rb->tail, first_chunk);
        memcpy(dst + first_chunk, rb->data, to_read - first_chunk);
    }
    rb->tail = (rb->tail + to_read) % rb->capacity;
    rb->count -= to_read;

    pthread_mutex_unlock(&rb->mutex);
    return to_read;
}

/* Discards up to max_len bytes from the front (oldest) of the ring buffer
 * without copying them anywhere. Non-blocking: never waits for more data to
 * arrive, only discards what is CURRENTLY buffered. Returns the number of
 * bytes actually discarded (may be less than max_len if the buffer doesn't
 * currently hold that much). */
static size_t ring_buffer_discard(ring_buffer_t * rb, size_t max_len) {
    if (max_len == 0) return 0;
    pthread_mutex_lock(&rb->mutex);
    size_t to_discard = (rb->count < max_len) ? rb->count : max_len;
    rb->tail = (rb->tail + to_discard) % rb->capacity;
    rb->count -= to_discard;
    pthread_mutex_unlock(&rb->mutex);
    return to_discard;
}

static size_t ring_buffer_get_occupancy(ring_buffer_t * rb) {
    if (!rb->initialized) return 0;
    pthread_mutex_lock(&rb->mutex);
    size_t count = rb->count;
    pthread_mutex_unlock(&rb->mutex);
    return count;
}

static void update_bridge_running_state(void) {
    pthread_mutex_lock(&bridge_mutex);
    bridge_running = !stop_requested && (g_reader_running || g_writer_running);
    if (!bridge_running) bridge_streaming = false;
    pthread_mutex_unlock(&bridge_mutex);
}

static void set_streaming(bool streaming) {
    pthread_mutex_lock(&bridge_mutex);
    bridge_streaming = streaming && !stop_requested;
    pthread_mutex_unlock(&bridge_mutex);
}

static void set_active_rate(unsigned int rate) {
    pthread_mutex_lock(&bridge_mutex);
    active_output_rate = rate;
    pthread_mutex_unlock(&bridge_mutex);
}

/* 1. READER THREAD:
 * Opens /dev/uac_sa, runs poll()+read() loop, handles EPERM/EOF/errors,
 * continuously measures incoming USB byte rate (independent of output backpressure),
 * resets measurement window across poll() timeouts or EPERM waits, and pushes
 * raw incoming bytes into the circular ring buffer. Never calls audio_output.h.
 */
static void * bridge_reader_thread_func(void * arg) {
    (void) arg;

    int uac_fd = -1;
    for (int waited_ms = 0; waited_ms < OPEN_RETRY_TIMEOUT_MS; waited_ms += POLL_INTERVAL_MS) {
        if (stop_requested) {
            /* Finish all ring access before publishing completion. */
            ring_buffer_signal_finished(&g_rb);
            pthread_mutex_lock(&bridge_mutex);
            g_reader_running = false;
            pthread_mutex_unlock(&bridge_mutex);
            update_bridge_running_state();
            return NULL;
        }
        uac_fd = open(UAC_SA_DEVICE_PATH, O_RDWR);
        if (uac_fd >= 0) break;
        usleep(POLL_INTERVAL_MS * 1000);
    }
    if (uac_fd < 0) {
        BRIDGE_LOG("usb_dac_bridge: %s never appeared, giving up\n", UAC_SA_DEVICE_PATH);
        ring_buffer_signal_finished(&g_rb);
        pthread_mutex_lock(&bridge_mutex);
        g_reader_running = false;
        pthread_mutex_unlock(&bridge_mutex);
        update_bridge_running_state();
        return NULL;
    }

    size_t frame_bytes = (size_t) BRIDGE_CHANNELS * sizeof(int16_t);
    size_t buf_bytes = (size_t) BRIDGE_PERIOD_FRAMES * frame_bytes;
    uint8_t * buf = malloc(buf_bytes);
    if (!buf) {
        BRIDGE_LOG("usb_dac_bridge: failed to allocate read buffer\n");
        close(uac_fd);
        ring_buffer_signal_finished(&g_rb);
        pthread_mutex_lock(&bridge_mutex);
        g_reader_running = false;
        pthread_mutex_unlock(&bridge_mutex);
        update_bridge_running_state();
        return NULL;
    }

    uint64_t total_bytes_read = 0;
    int poll_timeouts_in_a_row = 0;
    int exit_reason_n = 0;
    int exit_reason_errno = 0;

    /* Live incoming-rate measurement -- decoupled from playback backpressure.
     * Window timing does not start until the first successful read after a gap.
     * Gaps (poll timeouts, EPERM retries, or errors) mark the window unstarted
     * and reset pending rate confirmation state so idle time is never included. */
    uint64_t rate_window_start_ns = 0;
    uint64_t rate_window_bytes = 0;
    bool rate_window_active = false;
    unsigned int pending_rate = 0;
    int pending_rate_count = 0;

    int eperm_retries = 0;
    #define EPERM_RETRY_LIMIT 1500 /* ~5min at POLL_INTERVAL_MS between retries */

    /* Recovery tracking across EOF / gadget session boundaries.
     * Genuine disconnect is bounded by RECOVERY_TIMEOUT_NS (~5s) since last
     * successful read. FAST_RECOVERY_MAX_GAP_NS defines the latency threshold
     * (e.g. 500ms, 2x POLL_INTERVAL_MS) below which an in-progress rate-measurement
     * window is preserved across reopen -- load-bearing, not just an
     * optimization: real BRIDGE_LOG capture showed this device's /dev/uac_sa
     * reopens every ~9-15ms as normal steady-state operation, far more often
     * than the 250ms window needs to complete uninterrupted, so discarding it
     * on every reopen (tried once, reverted -- see the `if (in_recovery)`
     * block's own comment further down) starves rate confirmation entirely. */
    #define RECOVERY_TIMEOUT_NS (5ULL * 1000000000ULL)
    #define FAST_RECOVERY_MAX_GAP_NS (500ULL * 1000000ULL)
    /* Above this, a completed recovery is logged even if it only took one
     * open() attempt -- see the "recovered from EOF" log call's own comment
     * for why the routine (~9-15ms) case isn't logged at all. */
    #define EOF_RECOVERY_LOG_MIN_MS 20.0

    uint64_t last_successful_read_ns = monotonic_ns();
    int eof_reopen_attempts = 0;
    uint64_t eof_recovery_start_ns = 0;
    uint64_t eof_first_open_success_ns = 0;
    uint64_t bytes_before_eof = 0;
    unsigned int cumulative_backoff_ms = 0;
    bool in_recovery = false;

    while (!stop_requested) {
        struct pollfd pfd = { .fd = uac_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (pr < 0) {
            if (errno == EINTR) {
                continue; /* Interrupted by signal; immediately retry without timeout side effects */
            }
            /* Other poll error: handle similarly to timeout / gap. Must still enter
             * recovery bookkeeping here (matching the EOF/revents branches) --
             * otherwise a persistent non-EINTR poll() error (e.g. a bad fd) never
             * starts the RECOVERY_TIMEOUT_NS clock and busy-spins forever, since
             * poll() returns immediately on error rather than blocking. */
            set_streaming(false);
            rate_window_active = false;
            rate_window_bytes = 0;
            pending_rate = 0;
            pending_rate_count = 0;
            if (!in_recovery) {
                in_recovery = true;
                eof_recovery_start_ns = monotonic_ns();
                eof_reopen_attempts = 0;
                cumulative_backoff_ms = 0;
                bytes_before_eof = total_bytes_read;
            }
            if (monotonic_ns() - last_successful_read_ns >= RECOVERY_TIMEOUT_NS) {
                BRIDGE_LOG("usb_dac_bridge: poll error '%s' during recovery, deadline exceeded -- exiting\n",
                           strerror(errno));
                exit_reason_n = -1;
                exit_reason_errno = errno;
                break;
            }
            usleep(POLL_INTERVAL_MS * 1000); /* pace retries; poll() itself doesn't block on error */
            continue;
        } else if (pr == 0) {
            set_streaming(false);
            /* Mark measurement window unstarted across poll timeout gap and clear
             * pending confirmation count to avoid spanning across gaps. */
            rate_window_active = false;
            rate_window_bytes = 0;
            pending_rate = 0;
            pending_rate_count = 0;

            poll_timeouts_in_a_row++;
            if (poll_timeouts_in_a_row == 25) /* ~5s of nothing but still open -- worth knowing */
                BRIDGE_LOG("usb_dac_bridge: no data from %s in ~5s (still waiting)\n", UAC_SA_DEVICE_PATH);

            if (in_recovery && (monotonic_ns() - last_successful_read_ns >= RECOVERY_TIMEOUT_NS)) {
                BRIDGE_LOG("usb_dac_bridge: recovery deadline (~5s) exceeded during poll timeouts -- exiting\n");
                exit_reason_n = 0;
                exit_reason_errno = 0;
                break;
            }
            continue; /* timeout (check stop_requested again) */
        }
        poll_timeouts_in_a_row = 0;

        /* Raw revents bitmask dump removed (2026-09-10): confirmed via a real
         * device log capture to never fire in practice -- this device's
         * routine ~9-15ms reconnect cycle consistently reports plain POLLIN
         * from poll() and gets its EOF from read() returning 0, never from
         * this POLLHUP/POLLERR/POLLNVAL branch. If it ever does start firing
         * (a different host, a different failure mode), the branch below
         * still logs "repeat revents=..." on any 2nd+ attempt within an
         * episode, and the deadline-exceeded paths remain unconditional --
         * this line was purely a redundant raw-bitmask breakdown on top of
         * those, and unconditionally printing it every poll() where
         * revents != POLLIN was a real risk of reintroducing the same
         * audio-thread logging-stall class of bug fixed elsewhere in this
         * function, for no retained diagnostic value in the case that
         * actually occurs on real hardware. */

        /* If poll signaled hangup/error without readable data, treat as recoverable EOF/boundary */
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) && !(pfd.revents & POLLIN)) {
            set_streaming(false);
            if (!in_recovery) {
                in_recovery = true;
                eof_recovery_start_ns = monotonic_ns();
                eof_first_open_success_ns = 0;
                eof_reopen_attempts = 0;
                cumulative_backoff_ms = 0;
                bytes_before_eof = total_bytes_read;
                /* Not logged here -- see "recovered from EOF"'s own comment.
                 * That single summary (fired once this episode's next
                 * successful read lands, in the shared `if (in_recovery)`
                 * block further down) already covers whichever path
                 * (revents-triggered here, or clean EOF below) entered
                 * recovery, so this entry point needs no log of its own. */
            } else {
                /* Repeat hit within active recovery episode: escalating backoff to prevent tight spin without flat latency tax */
                unsigned int backoff_ms = repeat_hit_backoff_ms(eof_reopen_attempts);
                cumulative_backoff_ms += backoff_ms;
                if (eof_reopen_attempts <= 5 || eof_reopen_attempts % 10 == 0) {
                    BRIDGE_LOG("usb_dac_bridge: repeat revents=0x%x within active recovery (attempt count so far: %d), pacing %u ms (cumulative %u ms)\n",
                               pfd.revents, eof_reopen_attempts, backoff_ms, cumulative_backoff_ms);
                }
                if (backoff_ms > 0) {
                    usleep(backoff_ms * 1000);
                }
            }
            if (monotonic_ns() - last_successful_read_ns >= RECOVERY_TIMEOUT_NS) {
                BRIDGE_LOG("usb_dac_bridge: poll revents=0x%x recovery deadline exceeded -- exiting\n", pfd.revents);
                exit_reason_n = 0;
                exit_reason_errno = EIO;
                break;
            }
            close(uac_fd);
            uac_fd = -1;
            /* Actually retry the open at a paced interval until it succeeds, stop is
             * requested, or the recovery deadline is hit -- a single failed attempt
             * must not fall through to polling a closed (-1) fd, which poll() simply
             * ignores forever (always "times out") without ever reopening it. */
            uint64_t loop_start_ns = monotonic_ns();
            while (uac_fd < 0 && !stop_requested) {
                eof_reopen_attempts++;
                uac_fd = open(UAC_SA_DEVICE_PATH, O_RDWR);
                int open_err = (uac_fd < 0) ? errno : 0;
                double elapsed_loop_ms = (double)(monotonic_ns() - loop_start_ns) / 1e6;
                if (uac_fd >= 0) {
                    if (eof_first_open_success_ns == 0) eof_first_open_success_ns = monotonic_ns();
                    /* attempt 1 is the routine (~9-15ms) fast path -- not
                     * logged here either, folded into "recovered from EOF"'s
                     * own attempt count once this episode actually ends. */
                    if ((eof_reopen_attempts > 1 && eof_reopen_attempts <= 5) || eof_reopen_attempts % 10 == 0) {
                        BRIDGE_LOG("usb_dac_bridge: open() succeeded (attempt %d, elapsed %.2f ms since retry loop started)\n",
                                   eof_reopen_attempts, elapsed_loop_ms);
                    }
                    break;
                }
                if (eof_reopen_attempts <= 5 || eof_reopen_attempts % 10 == 0) {
                    BRIDGE_LOG("usb_dac_bridge: open() failed (attempt %d, elapsed %.2f ms since retry loop started): errno=%d (%s)\n",
                               eof_reopen_attempts, elapsed_loop_ms, open_err, strerror(open_err));
                }
                if (monotonic_ns() - last_successful_read_ns >= RECOVERY_TIMEOUT_NS) break;
                usleep(POLL_INTERVAL_MS * 1000);
            }
            if (uac_fd < 0 && !stop_requested) {
                BRIDGE_LOG("usb_dac_bridge: poll revents=0x%x recovery deadline exceeded after %d reopen attempts -- exiting\n",
                           pfd.revents, eof_reopen_attempts);
                exit_reason_n = 0;
                exit_reason_errno = EIO;
                break;
            }
            continue;
        }

        ssize_t n = read(uac_fd, buf, buf_bytes);
        int read_errno = errno; /* captured immediately */
        if (n <= 0) {
            set_streaming(false);

            /* Recoverable clean EOF (n == 0): gadget session boundary */
            if (n == 0) {
                uint64_t now_ns = monotonic_ns();
                if (!in_recovery) {
                    in_recovery = true;
                    eof_recovery_start_ns = now_ns;
                    eof_first_open_success_ns = 0;
                    eof_reopen_attempts = 0;
                    cumulative_backoff_ms = 0;
                    bytes_before_eof = total_bytes_read;
                    /* Not logged here -- see "recovered from EOF"'s own
                     * comment; that single summary covers this entry point
                     * too once the episode ends. */
                } else {
                    /* Repeat hit within active recovery episode: escalating backoff to prevent tight spin without flat latency tax */
                    unsigned int backoff_ms = repeat_hit_backoff_ms(eof_reopen_attempts);
                    cumulative_backoff_ms += backoff_ms;
                    if (eof_reopen_attempts <= 5 || eof_reopen_attempts % 10 == 0) {
                        BRIDGE_LOG("usb_dac_bridge: repeat EOF within active recovery (attempt count so far: %d), pacing %u ms (cumulative %u ms)\n",
                                   eof_reopen_attempts, backoff_ms, cumulative_backoff_ms);
                    }
                    if (backoff_ms > 0) {
                        usleep(backoff_ms * 1000);
                    }
                }

                if (now_ns - last_successful_read_ns >= RECOVERY_TIMEOUT_NS) {
                    BRIDGE_LOG("usb_dac_bridge: EOF recovery deadline (~5s) exceeded -- exiting\n");
                    exit_reason_n = 0;
                    exit_reason_errno = 0;
                    break;
                }

                close(uac_fd);
                uac_fd = -1;
                /* Actually retry the open at a paced interval until it succeeds, stop is
                 * requested, or the recovery deadline is hit -- a single failed attempt
                 * must not fall through to polling a closed (-1) fd, which poll() simply
                 * ignores forever (always "times out") without ever reopening it. */
                uint64_t loop_start_ns = monotonic_ns();
                while (uac_fd < 0 && !stop_requested) {
                    eof_reopen_attempts++;
                    uac_fd = open(UAC_SA_DEVICE_PATH, O_RDWR);
                    int open_err = (uac_fd < 0) ? errno : 0;
                    double elapsed_loop_ms = (double)(monotonic_ns() - loop_start_ns) / 1e6;
                    if (uac_fd >= 0) {
                        if (eof_first_open_success_ns == 0) eof_first_open_success_ns = monotonic_ns();
                        /* attempt 1 is the routine fast path -- see the
                         * mirrored revents-triggered block's own comment. */
                        if ((eof_reopen_attempts > 1 && eof_reopen_attempts <= 5) || eof_reopen_attempts % 10 == 0) {
                            BRIDGE_LOG("usb_dac_bridge: open() succeeded (attempt %d, elapsed %.2f ms since retry loop started)\n",
                                       eof_reopen_attempts, elapsed_loop_ms);
                        }
                        break;
                    }
                    if (eof_reopen_attempts <= 5 || eof_reopen_attempts % 10 == 0) {
                        BRIDGE_LOG("usb_dac_bridge: open() failed (attempt %d, elapsed %.2f ms since retry loop started): errno=%d (%s)\n",
                                   eof_reopen_attempts, elapsed_loop_ms, open_err, strerror(open_err));
                    }
                    rate_window_active = false;
                    rate_window_bytes = 0;
                    pending_rate = 0;
                    pending_rate_count = 0;
                    if (monotonic_ns() - last_successful_read_ns >= RECOVERY_TIMEOUT_NS) break;
                    usleep(POLL_INTERVAL_MS * 1000);
                }
                if (uac_fd < 0 && !stop_requested) {
                    BRIDGE_LOG("usb_dac_bridge: EOF recovery deadline (~5s) exceeded after %d reopen attempts -- exiting\n",
                               eof_reopen_attempts);
                    exit_reason_n = 0;
                    exit_reason_errno = 0;
                    break;
                }
                /* If open succeeded, loop back immediately to poll() without resetting rate state yet */
                continue;
            }

            /* EPERM error handling */
            if (n < 0 && read_errno == EPERM) {
                if (in_recovery && (monotonic_ns() - last_successful_read_ns >= RECOVERY_TIMEOUT_NS)) {
                    BRIDGE_LOG("usb_dac_bridge: EPERM recovery deadline (~5s) exceeded after reopen -- exiting\n");
                    exit_reason_n = (int) n;
                    exit_reason_errno = read_errno;
                    break;
                }
                if (eperm_retries < EPERM_RETRY_LIMIT) {
                    rate_window_active = false;
                    rate_window_bytes = 0;
                    pending_rate = 0;
                    pending_rate_count = 0;
                    eperm_retries++;
                    if (eperm_retries == 1)
                        BRIDGE_LOG("usb_dac_bridge: read() EPERM, waiting for host to arm streaming interface...\n");
                    else if (eperm_retries % 25 == 0) /* ~5s periodic update */
                        BRIDGE_LOG("usb_dac_bridge: still waiting on host stream (EPERM retries=%d)\n", eperm_retries);
                    usleep(POLL_INTERVAL_MS * 1000);
                    continue;
                }
            }

            /* Non-recoverable error */
            rate_window_active = false;
            rate_window_bytes = 0;
            pending_rate = 0;
            pending_rate_count = 0;
            exit_reason_n = (int) n;
            exit_reason_errno = read_errno;
            break; /* device non-EPERM error -- gadget likely torn down underneath us */
        }
        if (eperm_retries > 0) {
            BRIDGE_LOG("usb_dac_bridge: read() succeeded after %d EPERM retries\n", eperm_retries);
            eperm_retries = 0;
            rate_window_active = false;
            rate_window_bytes = 0;
            pending_rate = 0;
            pending_rate_count = 0;
        }

        uint64_t now_ns = monotonic_ns();
        last_successful_read_ns = now_ns;

        if (in_recovery) {
            uint64_t recovery_duration_ns = now_ns - eof_recovery_start_ns;
            double recovery_ms = (double) recovery_duration_ns / 1e6;
            double first_open_ms = (eof_first_open_success_ns > 0 && eof_first_open_success_ns >= eof_recovery_start_ns)
                                   ? (double)(eof_first_open_success_ns - eof_recovery_start_ns) / 1e6
                                   : -1.0;
            /* Time spent between the fd first becoming open and real data finally
             * arriving -- isolates "open() itself was slow/failing" (this stays
             * near 0) from "the freshly-opened fd re-EOF'd and needed the repeat-
             * hit pacing before real data showed up" (this absorbs that time). */
            double open_to_read_ms = (first_open_ms >= 0.0) ? (recovery_ms - first_open_ms) : -1.0;
            /* Only log a recovery that took more than one open() attempt or
             * meaningfully longer than typical -- confirmed on real hardware
             * (2026-09-10): this device's /dev/uac_sa reopens every ~9-15ms
             * as NORMAL steady-state operation (99.5% of measured cycles),
             * so logging every single one of these routine reconnects
             * (previously unconditional here, plus "entering recovery"/
             * "open() succeeded" at each of those call sites) ran BRIDGE_LOG's
             * synchronous fsync() roughly 250-300 times/second on this same
             * reader thread, and grew the log fast enough to hit its 2MB
             * rotation size (its own separate fclose+remove+rename+fopen
             * stall) roughly every 30s -- both squarely on the thread that
             * must keep up with incoming audio. Confirmed as the cause of
             * reported periodic crackling. A genuinely slow/anomalous
             * recovery is still fully logged; the routine fast path no
             * longer costs anything here. */
            if (eof_reopen_attempts > 1 || recovery_ms > EOF_RECOVERY_LOG_MIN_MS) {
                BRIDGE_LOG("usb_dac_bridge: recovered from EOF: %llu bytes read before EOF, %d reopen attempts, latency %.2f ms (time-to-first-open %.2f ms, open-to-first-read %.2f ms, cumulative_backoff %u ms, ring_occupancy=%zu bytes)\n",
                           (unsigned long long) bytes_before_eof,
                           eof_reopen_attempts,
                           recovery_ms,
                           first_open_ms,
                           open_to_read_ms,
                           cumulative_backoff_ms,
                           ring_buffer_get_occupancy(&g_rb));
            }

            /* REVERTED (real-hardware regression, see below): a prior version
             * of this comment argued the in-progress measurement window
             * should always be discarded across this gap, fast or slow,
             * reasoning that a stale rate_window_start_ns spanning the gap
             * corrupts that window's measured rate downward. That reasoning
             * was correct in isolation but wrong for this device's actual
             * behavior: real BRIDGE_LOG capture showed /dev/uac_sa reopens
             * every ~9-15ms as NORMAL steady-state operation (99.5% of
             * measured recovery cycles), far more often than
             * RATE_MEASURE_WINDOW_MS (250ms) needs to complete uninterrupted.
             * Always discarding the window on every recovery made it nearly
             * impossible for any window to ever reach 250ms elapsed at all,
             * starving g_rate_confirmed_once and forcing every session into
             * the full MUTE_FALLBACK_TIMEOUT_NS (5s) bounded-mute fallback --
             * confirmed via real device log analysis (Codex, 2026-09-10)
             * against a live capture: only ~0.05% of successful-recovery-to-
             * next-EOF spans reached 250ms. Reverted to the original
             * behavior: only discard the window on a SLOW recovery
             * (matching the multi-window CONFIRMATION streak's own gating
             * below), preserving it across the device's normal fast
             * micro-reconnects so a window can accumulate across them. The
             * corruption this was meant to fix is real but rare and
             * self-correcting (the next clean window re-measures and
             * re-confirms); see snap_to_standard_rate()'s own comment for
             * the actual fix applied for that -- rejecting implausible
             * measurements outright, rather than never letting a window
             * survive a gap. */
            if (recovery_duration_ns > FAST_RECOVERY_MAX_GAP_NS) {
                rate_window_active = false;
                rate_window_bytes = 0;
                pending_rate = 0;
                pending_rate_count = 0;
            }
            in_recovery = false;
        }

        total_bytes_read += (uint64_t) n;
        set_streaming(true);

        /* Push raw PCM into ring buffer. Drops oldest if full; never blocks reader. */
        ring_buffer_write_drop_oldest(&g_rb, buf, (size_t) n);

        /* Live rate measurement: start timing on first successful read of the window */
        if (!rate_window_active) {
            rate_window_start_ns = now_ns;
            rate_window_bytes = 0;
            rate_window_active = true;
        }

        rate_window_bytes += (uint64_t) n;
        uint64_t elapsed_ns = now_ns - rate_window_start_ns;
        if (elapsed_ns >= (uint64_t) RATE_MEASURE_WINDOW_MS * 1000000ULL) {
            double elapsed_s = (double) elapsed_ns / 1e9;
            double measured_frames_per_sec = ((double) rate_window_bytes / (double) frame_bytes) / elapsed_s;
            if (measured_frames_per_sec < RATE_MEASURE_MIN_PLAUSIBLE_HZ) {
                /* Implausible outlier (see RATE_MEASURE_MIN_PLAUSIBLE_HZ's own
                 * comment) -- discard this window without touching
                 * pending_rate/pending_rate_count, so an otherwise-valid
                 * confirmation streak isn't broken by one corrupted sample. */
                BRIDGE_LOG("usb_dac_bridge: rate window: measured ~%.0f Hz -- implausible, discarding window (confirmed count %d, current confirmed %u Hz)\n",
                           measured_frames_per_sec, pending_rate_count, g_confirmed_rate);
            } else {
                unsigned int snapped = snap_to_standard_rate(measured_frames_per_sec);
                if (snapped == pending_rate) {
                    pending_rate_count++;
                } else {
                    pending_rate = snapped;
                    pending_rate_count = 1;
                }

                pthread_mutex_lock(&bridge_mutex);
                unsigned int current_confirmed = g_confirmed_rate;
                if (pending_rate_count >= RATE_CONFIRM_COUNT) {
                    g_rate_confirmed_once = true;
                    if (pending_rate != current_confirmed) {
                        g_confirmed_rate = pending_rate;
                    }
                }
                pthread_mutex_unlock(&bridge_mutex);

                BRIDGE_LOG("usb_dac_bridge: rate window: measured ~%.0f Hz -> snapped %u Hz (confirmed count %d, current confirmed %u Hz)\n",
                           measured_frames_per_sec, snapped, pending_rate_count, current_confirmed);

                if (pending_rate_count >= RATE_CONFIRM_COUNT && pending_rate != current_confirmed) {
                    BRIDGE_LOG("usb_dac_bridge: incoming rate changed %u -> %u Hz (measured ~%.0f), signalling writer\n",
                            current_confirmed, pending_rate, measured_frames_per_sec);
                }
            }
            rate_window_start_ns = now_ns;
            rate_window_bytes = 0;
        }
    }

    if (!stop_requested) {
        BRIDGE_LOG("usb_dac_bridge: read() returned %d (errno=%d %s) after %llu bytes total -- exiting\n",
                exit_reason_n, exit_reason_errno, strerror(exit_reason_errno), (unsigned long long) total_bytes_read);
    } else {
        BRIDGE_LOG("usb_dac_bridge: reader stopped, %llu bytes read total\n", (unsigned long long) total_bytes_read);
    }

    free(buf);
    if (uac_fd >= 0) close(uac_fd);

    /* Signal producer finished before publishing completion to stop(). */
    ring_buffer_signal_finished(&g_rb);
    pthread_mutex_lock(&bridge_mutex);
    g_reader_running = false;
    pthread_mutex_unlock(&bridge_mutex);
    update_bridge_running_state();

    return NULL;
}

#define CATCHUP_TARGET_RESERVE_MS 150.0  /* starting point per Codex's review -- expect to tune down after real-device testing, as low as possible without reintroducing the earlier crackling/underrun bug */
#define CATCHUP_FADE_MS 10.0             /* short fade-in applied right after a trim, to avoid an audible click at the discontinuity */

/* Called by the writer thread whenever the ring buffer might be holding more
 * backlog than necessary (writer startup, and every time it adopts a newly
 * confirmed rate from the reader -- see call sites below). If current
 * occupancy exceeds a small target reserve (enough to survive the backoff
 * ladder's typical recovery latency, per usb_dac_bridge_lifecycle_regression.c's
 * own evidence and the real-device captures -- NOT enough for its rare
 * worst-case ~277ms+ escalation, which is an accepted, explicit tradeoff per
 * the "as low as possible" instruction, not an oversight), discard the excess
 * directly (skipping stale, already-queued audio -- it's valid data, just
 * queued longer than necessary, not corrupted), then apply a short linear
 * fade-in to the audio immediately following the discard point before it's
 * played, so the skip doesn't produce an audible click. This does NOT touch
 * the reader thread or its pacing in any way -- it only ever discards data
 * the reader has ALREADY produced and buffered, never slows how fast the
 * reader consumes from /dev/uac_sa. */
static bool writer_catchup_trim(unsigned int rate, size_t frame_bytes) {
    size_t target_reserve_bytes = (size_t) ((double) rate * CATCHUP_TARGET_RESERVE_MS / 1000.0) * frame_bytes;
    size_t occupancy_before = ring_buffer_get_occupancy(&g_rb);
    if (occupancy_before <= target_reserve_bytes) return false;

    size_t excess = occupancy_before - target_reserve_bytes;
    size_t fade_bytes = (size_t) ((double) rate * CATCHUP_FADE_MS / 1000.0) * frame_bytes;
    fade_bytes -= fade_bytes % frame_bytes;
    if (fade_bytes == 0) fade_bytes = frame_bytes;

    size_t discard_bytes = (excess > fade_bytes) ? (excess - fade_bytes) : 0;
    size_t discarded = ring_buffer_discard(&g_rb, discard_bytes);

    uint8_t * fade_buf = malloc(fade_bytes);
    if (fade_buf) {
        size_t fade_read = ring_buffer_read(&g_rb, fade_buf, fade_bytes);
        if (fade_read > 0) {
            size_t fade_frames = fade_read / frame_bytes;
            int16_t * samples = (int16_t *) fade_buf;
            /* Apply the SAME "overwrite noise slot with real channel" fixup the
             * normal per-chunk write path applies (see the main loop below) --
             * this fade segment bypasses that normal path, so it must be
             * applied here too, or this segment would play with the wrong/noisy
             * first channel. */
            for (size_t i = 0; i < fade_frames; i++) {
                samples[i * BRIDGE_CHANNELS] = samples[i * BRIDGE_CHANNELS + 1];
            }
            for (size_t i = 0; i < fade_frames; i++) {
                double gain = (fade_frames > 1) ? ((double) i / (double) (fade_frames - 1)) : 1.0;
                for (unsigned int c = 0; c < BRIDGE_CHANNELS; c++) {
                    samples[i * BRIDGE_CHANNELS + c] = (int16_t) ((double) samples[i * BRIDGE_CHANNELS + c] * gain);
                }
            }
            uint64_t written_frames = 0;
            bool write_ok = audio_output_write(samples, fade_frames, BRIDGE_CHANNELS, &written_frames);
            if (!write_ok || written_frames != (uint64_t) fade_frames) {
                BRIDGE_LOG("usb_dac_bridge: catch-up fade write failure/partial: ok=%d written=%" PRIu64 "/%zu frames\n",
                           write_ok ? 1 : 0, written_frames, fade_frames);
            }
        }
        free(fade_buf);
    }

    size_t occupancy_after = ring_buffer_get_occupancy(&g_rb);
    BRIDGE_LOG("usb_dac_bridge: catch-up trim: discarded %zu bytes + %zu-byte fade (occupancy %zu -> %zu bytes, target ~%zu bytes, rate %u Hz)\n",
               discarded, fade_bytes, occupancy_before, occupancy_after, target_reserve_bytes, rate);
    return true;
}

/* 2. WRITER THREAD:
 * Only thread calling audio_output.h. Waits for audio from ring buffer,
 * monitors confirmed rate updates from the reader thread, applies channel
 * fixup (overwriting noise slot), and calls audio_output_ensure() / audio_output_write().
 */
static void * bridge_writer_thread_func(void * arg) {
    (void) arg;

    /* audio_stop() (called by usb_dac_bridge_start() before spawning threads)
     * only signals the audio thread -- audio_output_close() happens
     * asynchronously on that thread. Wait for it to actually finish before
     * touching the shared output device ourselves, or audio_output_ensure()
     * below will fail with EBUSY. */
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (!audio_is_playing() && !audio_is_paused()) break;
        usleep(20000);
    }

    /* Do NOT re-initialize g_confirmed_rate here: usb_dac_bridge_start()
     * already set it (and active_output_rate) to USB_ADVERTISED_SAMPLE_RATE
     * under bridge_mutex before either thread was created, and pthread_create()
     * carries a POSIX-guaranteed happens-before for that write. Re-setting it
     * again here would race against the reader thread, which is created
     * first and could -- on a sufficiently delayed scheduler -- already have
     * confirmed a real rate change before this line ran, silently clobbering
     * it back to the default. */
    unsigned int current_rate = USB_ADVERTISED_SAMPLE_RATE;
    bool started_output = false;
    uint64_t first_read_ns = 0;

    size_t frame_bytes = (size_t) BRIDGE_CHANNELS * sizeof(int16_t);
    size_t chunk_bytes = (size_t) BRIDGE_PERIOD_FRAMES * frame_bytes;
    uint8_t * buf = malloc(chunk_bytes);
    if (!buf) {
        BRIDGE_LOG("usb_dac_bridge: failed to allocate write buffer\n");
        audio_output_close();
        stop_requested = true;
        ring_buffer_signal_finished(&g_rb);
        pthread_mutex_lock(&bridge_mutex);
        g_writer_running = false;
        pthread_mutex_unlock(&bridge_mutex);
        update_bridge_running_state();
        return NULL;
    }

    while (!stop_requested) {
        size_t n = ring_buffer_read(&g_rb, buf, chunk_bytes);
        if (n == 0) {
            /* Woke up due to stop_requested or empty buffer after reader finished */
            if (stop_requested) break;

            pthread_mutex_lock(&g_rb.mutex);
            bool finished = g_rb.producer_finished && (g_rb.count == 0);
            pthread_mutex_unlock(&g_rb.mutex);
            if (finished) {
                /* Producer finished and no remaining buffered data */
                break;
            }
            continue;
        }

        if (!started_output) {
            uint64_t now_ns = monotonic_ns();
            if (first_read_ns == 0) first_read_ns = now_ns;

            pthread_mutex_lock(&bridge_mutex);
            bool confirmed_once = g_rate_confirmed_once;
            unsigned int reader_confirmed = g_confirmed_rate;
            pthread_mutex_unlock(&bridge_mutex);

            bool fallback_expired = (now_ns - first_read_ns) >= EFFECTIVE_MUTE_FALLBACK_TIMEOUT_NS;
            if (!confirmed_once && !fallback_expired) {
                /* Still muted: drain without playing, so the ring buffer doesn't
                 * back up while we wait for a real confirmation. */
                continue;
            }
            if (!confirmed_once) {
                BRIDGE_LOG("usb_dac_bridge: no rate confirmed after %llu ms, starting playback at %u Hz anyway (bounded fallback)\n",
                           (unsigned long long) ((now_ns - first_read_ns) / 1000000ULL), reader_confirmed);
                /* Deliberately do NOT set g_rate_confirmed_once here -- if the reader
                 * later completes a real confirmation, it must still go through the
                 * normal rate-adopt path below, exactly as if this fallback never
                 * happened. */
            }

            /* stop_requested may have flipped while we were muted -- check before
             * doing any real work (opening the device, trimming). */
            if (stop_requested) break;

            current_rate = reader_confirmed;
            set_active_rate(current_rate);
            if (!audio_output_ensure(BRIDGE_CHANNELS, current_rate, true, false)) {
                BRIDGE_LOG("usb_dac_bridge: initial audio_output_ensure failed (rate=%u)\n", current_rate);
                stop_requested = true;
                ring_buffer_signal_finished(&g_rb);
                pthread_mutex_lock(&bridge_mutex);
                g_writer_running = false;
                pthread_mutex_unlock(&bridge_mutex);
                update_bridge_running_state();
                free(buf);
                return NULL;
            }
            writer_catchup_trim(current_rate, frame_bytes);
            started_output = true;
            /* Discard this transition chunk (it was read before we knew we were
             * about to start real output) rather than play it -- matching this
             * file's own existing precedent for exactly this kind of transition
             * (the catch-up-trim ordering fix's use of `continue` to skip a stale
             * pre-read chunk after a trim actually runs). */
            continue;
        }

        /* Check for sample rate changes confirmed by the reader thread */
        pthread_mutex_lock(&bridge_mutex);
        unsigned int reader_confirmed = g_confirmed_rate;
        pthread_mutex_unlock(&bridge_mutex);

        if (reader_confirmed != current_rate) {
            current_rate = reader_confirmed;
            set_active_rate(current_rate);
            BRIDGE_LOG("usb_dac_bridge: writer adopting confirmed rate %u Hz\n", current_rate);
            /* Reopen the output device at the NEW rate before trimming -- the
             * catch-up fade must play on a correctly-configured device, not
             * the stale one about to be closed/reconfigured. */
            if (!audio_output_ensure(BRIDGE_CHANNELS, current_rate, true, false)) {
                BRIDGE_LOG("usb_dac_bridge: audio_output_ensure failed (rate=%u)\n", current_rate);
            }
            /* Only skip `n` (this iteration's already-pre-read chunk) when a
             * trim actually ran: it's then oldest backlog that would have
             * fallen inside the discard window anyway, and playing it after
             * the fade would reintroduce exactly the discontinuity the fade
             * exists to avoid. If the buffer was already at/under target,
             * writer_catchup_trim() is a no-op (no fade played) -- `n` is
             * then just an ordinary chunk and must be played normally below,
             * or it would be silently, needlessly dropped. */
            if (writer_catchup_trim(current_rate, frame_bytes)) {
                continue;
            }
        }

        /* Ensure output device is open and check for dynamic Bluetooth routing changes.
         * Cheap recheck that only reopens when sample rate or target actually changed. */
        if (!audio_output_ensure(BRIDGE_CHANNELS, current_rate, true, false)) {
            BRIDGE_LOG("usb_dac_bridge: audio_output_ensure failed (rate=%u)\n", current_rate);
        }

        /* Only process whole frames */
        size_t frame_count = n / frame_bytes;
        if (frame_count > 0) {
            /* Overwrite the always-noise first channel slot with the real second one */
            int16_t * frame_samples = (int16_t *) buf;
            for (size_t i = 0; i < frame_count; i++) {
                frame_samples[i * 2] = frame_samples[i * 2 + 1];
            }

            uint64_t written_frames = 0;
            bool write_ok = audio_output_write(frame_samples, frame_count, BRIDGE_CHANNELS, &written_frames);
            if (!write_ok || written_frames != (uint64_t) frame_count) {
                BRIDGE_LOG("usb_dac_bridge: audio_output_write failure/partial: ok=%d written=%" PRIu64 "/%zu frames\n",
                           write_ok ? 1 : 0, written_frames, frame_count);
            }
        }
    }

    BRIDGE_LOG("usb_dac_bridge: writer thread exiting\n");
    free(buf);
    audio_output_close();

    pthread_mutex_lock(&bridge_mutex);
    g_writer_running = false;
    pthread_mutex_unlock(&bridge_mutex);
    update_bridge_running_state();

    return NULL;
}

/* Caller holds lifecycle_mutex, never bridge_mutex. A failed join retains
 * ownership so no caller may destroy/reuse resources still owned by a worker. */
static bool join_owned_thread(pthread_t thread, bool * created) {
    if (!*created) return true;
    int rc = pthread_join(thread, NULL);
    if (rc != 0) {
        BRIDGE_LOG("usb_dac_bridge: pthread_join failed: %s\n", strerror(rc));
        return false;
    }
    *created = false;
    return true;
}

#endif

void usb_dac_bridge_start(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&lifecycle_mutex);
    pthread_mutex_lock(&bridge_mutex);
    bool running = bridge_running;
    pthread_mutex_unlock(&bridge_mutex);
    if (running) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }

    /* This may block on a prior timed-out stop. Keep its buffer and stop
     * request intact until every owned thread has actually been joined. */
    bool reader_joined = join_owned_thread(g_reader_thread, &g_reader_thread_created);
    bool writer_joined = join_owned_thread(g_writer_thread, &g_writer_thread_created);
    if (!reader_joined || !writer_joined) {
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }
    ring_buffer_destroy(&g_rb);

    pthread_mutex_lock(&bridge_mutex);
    if (!ring_buffer_init(&g_rb, RING_BUFFER_CAPACITY)) {
        BRIDGE_LOG("usb_dac_bridge: failed to allocate %d bytes for ring buffer\n", RING_BUFFER_CAPACITY);
        g_reader_running = false;
        g_writer_running = false;
        bridge_running = false;
        bridge_streaming = false;
        pthread_mutex_unlock(&bridge_mutex);
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }

    stop_requested = false;
    bridge_running = true;
    bridge_streaming = false;
    g_confirmed_rate = USB_ADVERTISED_SAMPLE_RATE;
    g_rate_confirmed_once = false;
    active_output_rate = USB_ADVERTISED_SAMPLE_RATE;

    /* Clear external USB DAC output request: operating as a USB audio device
     * gadget is mutually exclusive with driving an external USB host DAC. */
    audio_output_set_usb_requested(false, NULL);

    audio_stop();

    int rc_reader = pthread_create(&g_reader_thread, NULL, bridge_reader_thread_func, NULL);
    if (rc_reader == 0) {
        g_reader_running = true;
        g_reader_thread_created = true;
    } else {
        BRIDGE_LOG("usb_dac_bridge: failed to create reader thread: %s\n", strerror(rc_reader));
        g_reader_running = false;
    }

    int rc_writer = pthread_create(&g_writer_thread, NULL, bridge_writer_thread_func, NULL);
    if (rc_writer == 0) {
        g_writer_running = true;
        g_writer_thread_created = true;
    } else {
        BRIDGE_LOG("usb_dac_bridge: failed to create writer thread: %s\n", strerror(rc_writer));
        g_writer_running = false;
    }

    /* All-or-nothing: both threads must start successfully. If either failed,
     * immediately tear down whatever started and leave bridge cleanly stopped. */
    if (rc_reader != 0 || rc_writer != 0) {
        BRIDGE_LOG("usb_dac_bridge: partial startup failure (reader=%d writer=%d), rolling back\n",
                rc_reader, rc_writer);
        stop_requested = true;
        ring_buffer_signal_finished(&g_rb);

        bridge_running = false;
        bridge_streaming = false;
        pthread_mutex_unlock(&bridge_mutex);

        bool reader_joined = join_owned_thread(g_reader_thread, &g_reader_thread_created);
        bool writer_joined = join_owned_thread(g_writer_thread, &g_writer_thread_created);
        if (reader_joined && writer_joined) ring_buffer_destroy(&g_rb);
        pthread_mutex_unlock(&lifecycle_mutex);
        return;
    }

    pthread_mutex_unlock(&bridge_mutex);
    pthread_mutex_unlock(&lifecycle_mutex);
#endif
}

void usb_dac_bridge_stop(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&lifecycle_mutex);
    stop_requested = true;
    pthread_mutex_lock(&bridge_mutex);
    bridge_running = false;
    bridge_streaming = false;
    pthread_mutex_unlock(&bridge_mutex);

    /* Start the worker wait budget before waking the writer. Serialization
     * itself may wait for an overlapping start to reap an older session. */
    const uint64_t stop_timeout_ns = 3000000000ULL; /* 3.0 seconds */
    uint64_t stop_start_ns = monotonic_ns();
    if (g_rb.initialized) {
        pthread_mutex_lock(&g_rb.mutex);
        pthread_cond_broadcast(&g_rb.cond_data_available);
        pthread_mutex_unlock(&g_rb.mutex);
    }
    bool r_done = !g_reader_thread_created;
    bool w_done = !g_writer_thread_created;

    while (!r_done || !w_done) {
        pthread_mutex_lock(&bridge_mutex);
        if (!r_done && !g_reader_running) r_done = true;
        if (!w_done && !g_writer_running) w_done = true;
        pthread_mutex_unlock(&bridge_mutex);

        if (r_done && w_done) break;
        if ((monotonic_ns() - stop_start_ns) >= stop_timeout_ns) break;
        usleep(20000); /* 20ms poll */
    }

    /* Completion flags are published after blocking worker cleanup. Only a
     * successful join releases ownership; timeouts leave it for the next call. */
    if (r_done) join_owned_thread(g_reader_thread, &g_reader_thread_created);
    if (w_done) join_owned_thread(g_writer_thread, &g_writer_thread_created);

    if (!g_reader_thread_created && !g_writer_thread_created) {
        ring_buffer_destroy(&g_rb);
    }
    pthread_mutex_unlock(&lifecycle_mutex);
#endif
}

void usb_dac_bridge_set_bt_output(bool enabled) {
#ifndef HOST_BUILD
    /* Same shared flag audio_set_bt_output() itself sets (see
     * audio_output_set_bt_requested()) -- both call into the same
     * audio_output module, since only one of this bridge's thread or
     * audio.c's own playback thread is ever actually running at a time
     * (see audio_output.h's own comment on why that makes sharing this
     * safe without a lock). */
    audio_output_set_bt_requested(enabled);
#else
    (void) enabled;
#endif
}

void usb_dac_bridge_get_stream_info(usb_dac_stream_info_t * out) {
    if (!out) return;
    pthread_mutex_lock(&bridge_mutex);
    out->bridge_running = bridge_running;
    out->streaming = bridge_streaming;
    out->output_sample_rate = active_output_rate;
    pthread_mutex_unlock(&bridge_mutex);
    out->input_sample_rate = USB_ADVERTISED_SAMPLE_RATE;
    out->input_bit_depth = USB_ADVERTISED_BIT_DEPTH;
    out->output_bit_depth = BRIDGE_BIT_DEPTH;
}
