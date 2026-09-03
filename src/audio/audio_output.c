#include "audio_output.h"
#include "debug_log.h"
#include "subprocess.h"
#include "balanced_output_status.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>
#include <tinyalsa/mixer.h>
#define ALSA_CARD 0
#define ALSA_DEVICE 0

static struct pcm * alsa_pcm = NULL;

/* Three possible output targets -- originally just local-vs-Bluetooth (two
 * bools, bt_requested/bt_active), refactored to a tri-state enum when USB
 * output was added rather than bolting a third bool onto that scheme,
 * since "which ONE of three is active" is what every call site actually
 * needs, not three independent flags that could disagree with each other. */
typedef enum {
    OUTPUT_TARGET_LOCAL = 0,
    OUTPUT_TARGET_BT,
    OUTPUT_TARGET_USB,
} output_target_t;

/* See audio_output_set_bt_requested()/_set_usb_requested()'s doc comments
 * in audio_output.h for the real-device bug this exists to fix (originally
 * found/fixed for audio.c's own playback path; this file is that same fix,
 * extracted so usb_dac_bridge.c can share it rather than re-deriving it).
 * requested_target is set from the GUI thread (poll_refresh_bt_icon() ->
 * audio_set_bt_output()/usb_dac_bridge_set_bt_output(), or the USB Audio
 * Output poll -> audio_output_set_usb_requested()); active_target records
 * which output audio_output_ensure() actually has open right now, so a
 * mismatch between the two (checked in audio_output_ensure(), same place a
 * sample-rate change is checked) is what triggers a reopen -- same lazy,
 * only-reopen-when-something-actually-changed shape the rate/channel check
 * already had, just with one more axis. tinyalsa itself only ever talks to
 * numbered hw: cards, so both Bluetooth and USB are a completely separate
 * mechanism from local playback: pipe raw PCM into `aplay -D <target>`,
 * letting the real ALSA library (which aplay is linked against, unlike
 * this app) resolve the target through its own plugin system -- for
 * Bluetooth, "bluealsa" through /etc/alsa/conf.d/20-bluealsa.conf
 * (confirmed by reading that file directly: `pcm.bluealsa` is already
 * `type plug` wrapping the raw bluealsa ioplug with `device
 * 00:00:00:00:00:00`, BlueALSA's own "most recently connected device"
 * default, and `profile a2dp`, so no MAC address bookkeeping is needed
 * here); for USB, a resolved `plughw:<card>,0` string from usb_audio_
 * output.c (also "plug"-wrapped, same reasoning: an arbitrary external DAC
 * isn't guaranteed to natively support whatever rate this app is decoding
 * at). Either way the "plug" wrapper transparently handles the caller's
 * sample rate without this code needing its own resampler. */
static output_target_t requested_target = OUTPUT_TARGET_LOCAL;
static output_target_t active_target = OUTPUT_TARGET_LOCAL;
static pid_t bt_aplay_pid = -1;
static int bt_aplay_fd = -1;
static pid_t usb_aplay_pid = -1;
static int usb_aplay_fd = -1;
static char usb_alsa_device[32] = "";

static unsigned int device_channels = 0;
static unsigned int device_sample_rate = 0;

/* Which ALSA tuning the CURRENTLY OPEN OUTPUT_TARGET_LOCAL device actually
 * used -- set by open_device() below, read by audio_output_ensure() so a
 * mode change (same channels/rate, different low_latency) is recognized as
 * needing a reopen instead of silently keeping whatever was already open.
 * Meaningless for BT/USB (aplay's own ALSA config, not this one) -- not
 * updated on those paths, and audio_output_ensure() only ever compares it
 * when requested_target is LOCAL. */
static bool active_low_latency = false;

/* aplay's raw-PCM mode needs the format spelled out on the command line --
 * unlike a .wav it's reading straight off a pipe with no header to sniff
 * rate/channels/format from. `-D <device>` resolves through the real ALSA
 * library's plugin system (see requested_target's own doc comment above)
 * rather than tinyalsa, which is what makes either of these reachable at
 * all. Shared by both the Bluetooth and USB targets -- only the device
 * string and which pid/fd pair get filled in differ. */
static bool spawn_aplay(const char * device, unsigned int channels, unsigned int sample_rate,
                         pid_t * out_pid, int * out_fd) {
    char rate_str[16], channels_str[8];
    snprintf(rate_str, sizeof(rate_str), "%u", sample_rate);
    snprintf(channels_str, sizeof(channels_str), "%u", channels);

    char * argv[] = { (char *) "aplay", (char *) "-q", (char *) "-D", (char *) device,
                       (char *) "-t", (char *) "raw", (char *) "-f", (char *) "S16_LE",
                       (char *) "-r", rate_str, (char *) "-c", channels_str, NULL };
    if (!subprocess_popen_stdin(argv, out_pid, out_fd)) {
        DBG_LOG("audio_output: failed to spawn aplay for '%s' output\n", device);
        return false;
    }
    return true;
}

static void close_bt_device(void) {
    if (bt_aplay_pid < 0) return;
    subprocess_terminate(bt_aplay_pid);
    close(bt_aplay_fd);
    bt_aplay_pid = -1;
    bt_aplay_fd = -1;
}

static void close_usb_device(void) {
    if (usb_aplay_pid < 0) return;
    subprocess_terminate(usb_aplay_pid);
    close(usb_aplay_fd);
    usb_aplay_pid = -1;
    usb_aplay_fd = -1;
}

static bool open_device(unsigned int channels, unsigned int sample_rate, bool low_latency) {
    if (requested_target == OUTPUT_TARGET_USB) {
        if (!spawn_aplay(usb_alsa_device, channels, sample_rate, &usb_aplay_pid, &usb_aplay_fd)) return false;
        active_target = OUTPUT_TARGET_USB;
    } else if (requested_target == OUTPUT_TARGET_BT) {
        if (!spawn_aplay("bluealsa", channels, sample_rate, &bt_aplay_pid, &bt_aplay_fd)) return false;
        active_target = OUTPUT_TARGET_BT;
    } else {
        struct pcm_config config;
        memset(&config, 0, sizeof(config));
        config.channels = channels;
        config.rate = sample_rate;
        config.format = PCM_FORMAT_S16_LE;
        active_low_latency = low_latency;
        if (low_latency) {
            /* Real-device bug report: AirPlay had "significantly more audio
             * delay than stock player" -- the stock firmware's shairport
             * invocation (-o ot) writes straight to hardware from within
             * shairport itself; this app instead relays through a FIFO into
             * this shared local ALSA path, whose standard tuning below adds
             * its own ~186ms start_threshold on top with no decoder-timing
             * reason for a live source (AirPlay currently, and the USB DAC
             * bridge -- see this function's own callers) to pay it.
             *
             * period_size=1024/period_count=2 (~21ms buffer) was tried here
             * first and caused a real-device regression when the USB DAC
             * bridge used it ("completely broken... not emitting any sound
             * at all"). Root-caused with a standalone tinyalsa probe run
             * directly on this hardware (same card/device/channels/rate/
             * format as here): pcm_open()'s hw_params negotiation rejects
             * that exact (period_size, period_count) pair outright with
             * EINVAL, not an underrun -- this hardware/driver enforces a
             * 1024-frame minimum period and only accepts specific
             * (period_size, period_count) pairs, not a free choice of
             * buffer size. The same probe swept nearby configs on the real
             * device: 2048x2, 1024x3, 2048x3, and 4096x2 all also failed
             * hw_params, while 1024x4 (4096-frame buffer, ~43ms at 96kHz --
             * half of the ~85ms standard config below) succeeded, including
             * repeated pcm_writei() calls with no failures. That's the
             * config used here. It has not been verified under sustained
             * real playback jitter (screen redraws, database/artwork
             * activity, Wi-Fi) the way the standard config's four periods
             * were tuned for -- if AV sync is still off or new dropouts
             * appear, that's the next thing to check, not another blind
             * period_count change. */
            config.period_size = 1024;
            config.period_count = 4;
        } else {
            /* The original 1024-frame period woke this single-core device
             * about 43 times/sec at 44.1 kHz even when the decoder supplied
             * 8192-frame screen-off batches. A 2048-frame period halves
             * kernel/ALSA period wakeups while retaining ~46 ms period
             * granularity; four periods provide ~186 ms of underrun
             * protection, still below the app's existing 500 ms hardware-
             * button dispatch interval. */
            config.period_size = 2048;
            config.period_count = 4;
        }
        /* Explicit rather than left at the zeroed default -- matches
         * tinyalsa's own usual convention (full buffer size) rather than
         * relying on whatever the driver does with 0. */
        config.start_threshold = config.period_size * config.period_count;
        config.stop_threshold = config.period_size * config.period_count;

        alsa_pcm = pcm_open(ALSA_CARD, ALSA_DEVICE, PCM_OUT, &config);
        if (!alsa_pcm || !pcm_is_ready(alsa_pcm)) {
            DBG_LOG("audio_output: pcm_open failed: %s\n", alsa_pcm ? pcm_get_error(alsa_pcm) : "unknown");
            if (alsa_pcm) pcm_close(alsa_pcm);
            alsa_pcm = NULL;
            return false;
        }
        active_target = OUTPUT_TARGET_LOCAL;
    }
    device_channels = channels;
    device_sample_rate = sample_rate;
    return true;
}

void audio_output_close(void) {
    if (active_target == OUTPUT_TARGET_BT) close_bt_device();
    if (active_target == OUTPUT_TARGET_USB) close_usb_device();
    active_target = OUTPUT_TARGET_LOCAL; /* only open_device() above was ever setting this to BT/USB, never this side */
    if (alsa_pcm) { pcm_close(alsa_pcm); alsa_pcm = NULL; }
    device_channels = 0;
    device_sample_rate = 0;
}

bool audio_output_ensure(unsigned int channels, unsigned int sample_rate, bool low_latency) {
    /* Real-device bug: aplay can die entirely on its own (BlueZ tearing
     * down the transport underneath it -- a headset bonding hiccup during
     * testing was one confirmed trigger, but any A2DP renegotiation could
     * do the same; the same class of thing could happen to a USB DAC being
     * unplugged mid-stream) while requested_target never changes (this
     * app's own connection polls still report "connected" for a beat after
     * the specific audio transport actually died). The active_target !=
     * requested_target check below alone can't catch that -- both sides
     * still agree on the target -- so a dead aplay would otherwise go
     * unnoticed forever, with every future write() failing instantly and
     * audio_output_write()'s own pacing fallback silently swallowing every
     * chunk. Checking liveness (WNOHANG, not a blocking wait -- this runs
     * on the hot path) here catches it and forces the reopen below to
     * actually respawn aplay, the same as if the target itself had
     * changed. */
    if (active_target == OUTPUT_TARGET_BT && bt_aplay_pid >= 0) {
        int status;
        if (waitpid(bt_aplay_pid, &status, WNOHANG) == bt_aplay_pid) {
            DBG_LOG("audio_output: Bluetooth aplay died unexpectedly, reopening\n");
            bt_aplay_pid = -1; /* already reaped above -- close_bt_device() below must not wait on it again */
            close(bt_aplay_fd);
            bt_aplay_fd = -1;
        }
    }
    if (active_target == OUTPUT_TARGET_USB && usb_aplay_pid >= 0) {
        int status;
        if (waitpid(usb_aplay_pid, &status, WNOHANG) == usb_aplay_pid) {
            DBG_LOG("audio_output: USB aplay died unexpectedly, reopening\n");
            usb_aplay_pid = -1; /* already reaped above -- close_usb_device() below must not wait on it again */
            close(usb_aplay_fd);
            usb_aplay_fd = -1;
        }
    }

    bool device_open = (alsa_pcm != NULL || bt_aplay_pid >= 0 || usb_aplay_pid >= 0);
    if (device_open && active_target != requested_target) device_open = false;
    /* Only the OUTPUT_TARGET_LOCAL tinyalsa path has more than one tuning
     * (see open_device()) -- a mode mismatch there must force a reopen the
     * same as a channel/rate change would, or a caller requesting
     * low_latency while the device happens to already be open at the same
     * channels/rate (e.g. AirPlay starting up before local playback's own
     * async teardown has actually closed it) would silently keep the
     * wrong tuning instead of getting what it just asked for. */
    if (device_open && requested_target == OUTPUT_TARGET_LOCAL && active_low_latency != low_latency) device_open = false;
    if (device_open && device_channels == channels && device_sample_rate == sample_rate) return true;
    audio_output_close();
    return open_device(channels, sample_rate, low_latency);
}

bool audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels, uint64_t * out_frames_written) {
    if (out_frames_written) *out_frames_written = 0;
    if (active_target == OUTPUT_TARGET_BT || active_target == OUTPUT_TARGET_USB) {
        int fd = (active_target == OUTPUT_TARGET_BT) ? bt_aplay_fd : usb_aplay_fd;
        if (fd < 0) {
            /* No pipe open -- pace so the caller's loop doesn't spin */
            unsigned int rate = device_sample_rate ? device_sample_rate : 44100;
            usleep((useconds_t) ((uint64_t) frames * 1000000ULL / rate));
            return false;
        }
        /* Pipe write: loop until all bytes delivered, handling EINTR.
         * aplay's own ALSA write blocks on its end once its buffer is full,
         * which backpressures this write() naturally -- same as pcm_writei().
         * Any other error (EPIPE, EIO, ...) means aplay died; report failure
         * so the caller can close+reopen rather than silently dropping. */
        const char * p = (const char *) buf;
        size_t frame_bytes = channels * sizeof(int16_t);
        size_t total_bytes = (size_t) frames * frame_bytes;
        size_t remaining = total_bytes;
        while (remaining > 0) {
            ssize_t n;
            do { n = write(fd, p, remaining); } while (n < 0 && errno == EINTR);
            if (n <= 0) {
                size_t written_bytes = total_bytes - remaining;
                /* On pipe write error (aplay terminated/EPIPE), the old pipe is closed
                 * and destroyed. Return the count of fully delivered whole frames.
                 * The caller will reopen a new aplay pipe and resume from the whole-frame
                 * boundary, ensuring the new pipe receives strictly frame-aligned PCM. */
                if (out_frames_written) {
                    *out_frames_written = written_bytes / frame_bytes;
                }
                DBG_LOG("audio_output: pipe write failed (target=%d errno=%d written=%" PRIu64 "/%" PRIu64 " frames)\n",
                        (int) active_target, errno,
                        out_frames_written ? *out_frames_written : 0ULL, frames);
                return false;
            }
            p += n;
            remaining -= (size_t) n;
        }
        if (out_frames_written) *out_frames_written = frames;
        return true;
    }

    if (alsa_pcm) {
        /* pcm_writei blocks until ALSA has room -- natural backpressure.
         * Verify the full frame count was accepted; a short or negative
         * return means the device had an error (underrun, hw reset, etc.) */
        int ret = pcm_writei(alsa_pcm, buf, (unsigned int) frames);
        if (ret < 0 || (unsigned int) ret != (unsigned int) frames) {
            uint64_t written = (ret > 0) ? (uint64_t) ret : 0;
            if (out_frames_written) *out_frames_written = written;
            DBG_LOG("audio_output: pcm_writei returned %d (wanted %u written=%" PRIu64 "): %s\n",
                    ret, (unsigned int) frames, written,
                    ret < 0 ? pcm_get_error(alsa_pcm) : "short write");
            return false;
        }
        if (out_frames_written) *out_frames_written = frames;
        return true;
    }

    /* Nothing open -- pace so the caller cannot race ahead of real time,
     * then report failure so the caller knows frames were not delivered. */
    unsigned int rate = device_sample_rate ? device_sample_rate : 44100;
    usleep((useconds_t) ((uint64_t) frames * 1000000ULL / rate));
    return false;
}


/* USB takes priority over Bluetooth if both are somehow requested at once
 * (see audio_output.h's own doc comment on audio_output_set_usb_requested())
 * -- computed fresh from both callers' last-known request rather than
 * tracking a single requested_target directly, so either caller can flip
 * its own flag independently without needing to know about the other's
 * current state. */
static bool bt_requested = false;
static bool usb_requested = false;

static void recompute_requested_target(void) {
    requested_target = usb_requested ? OUTPUT_TARGET_USB : (bt_requested ? OUTPUT_TARGET_BT : OUTPUT_TARGET_LOCAL);
}

void audio_output_set_bt_requested(bool requested) {
    bt_requested = requested;
    recompute_requested_target();
}

void audio_output_set_usb_requested(bool requested, const char * alsa_device) {
    usb_requested = requested;
    if (requested) snprintf(usb_alsa_device, sizeof(usb_alsa_device), "%s", alsa_device);
    recompute_requested_target();
}

/* Lazily opened, never closed -- lives for the process's lifetime, same as
 * every other singleton hardware handle in this codebase (e.g. the
 * backlight sysfs fd). The mixer belongs to the card, not to whatever PCM
 * stream is currently open, so it deliberately isn't tied to
 * open_device()/audio_output_close()'s own lifecycle. */
static struct mixer * alsa_mixer = NULL;

static struct mixer * get_alsa_mixer(void) {
    if (!alsa_mixer) alsa_mixer = mixer_open(ALSA_CARD);
    return alsa_mixer;
}

void audio_output_set_hw_volume_raw(int raw_left, int raw_right) {
    static struct mixer_ctl * left_ctl = NULL;
    static struct mixer_ctl * right_ctl = NULL;
    static int last_left = INT_MIN;
    static int last_right = INT_MIN;

    if (raw_left == last_left && raw_right == last_right) return;

    struct mixer * mixer = get_alsa_mixer();
    if (!mixer) return;
    if (!left_ctl) left_ctl = mixer_get_ctl_by_name(mixer, "Left Playback Volume");
    if (!right_ctl) right_ctl = mixer_get_ctl_by_name(mixer, "Right Playback Volume");
    if (left_ctl && raw_left != last_left && mixer_ctl_set_value(left_ctl, 0, raw_left) == 0)
        last_left = raw_left;
    if (right_ctl && raw_right != last_right && mixer_ctl_set_value(right_ctl, 0, raw_right) == 0)
        last_right = raw_right;
}

/* Actual mixer write for balanced-output routing -- only ever called from
 * volume_worker_main() below, never directly from the UI thread. See
 * audio_output_sync_balanced_output()'s own comment for why: get_alsa_
 * mixer()'s lazy-init (alsa_mixer above) and this function's own cached
 * mixer_ctl* have no locking of their own, matching audio_output_set_hw_
 * volume_raw()'s identical pattern just above -- that one has always been
 * safe in practice because its ONLY caller is this same dedicated worker
 * thread (audio_output_request_hw_volume_raw()'s queueing wrapper), never
 * called directly from elsewhere. Real-device review finding: an earlier
 * version of this function ran the mixer I/O directly on the LVGL/UI
 * thread on every 500ms tick, racing this worker thread's own unsynchronized
 * access to the exact same alsa_mixer/mixer_ctl statics whenever a volume
 * drag happened to land in the same window -- moved here to close that
 * gap the same way volume writes already avoid it, rather than adding a
 * new lock (this file has no other locks besides the queue below; adding
 * one just for this would be a second synchronization mechanism doing the
 * same job the existing worker-thread serialization already does). */
static void apply_balanced_output(bool enabled) {
    static struct mixer_ctl * balanced_ctl = NULL;
    static bool balanced_ctl_lookup_done = false;
    static int last_enabled = -1; /* -1 = never written yet */

    struct mixer * mixer = get_alsa_mixer();
    if (!mixer) return;
    if (!balanced_ctl_lookup_done) {
        balanced_ctl = mixer_get_ctl_by_name(mixer, "Balance Lineout En");
        balanced_ctl_lookup_done = true;
    }
    if (!balanced_ctl) return; /* no such control -- R1/host, or the name is wrong; safe no-op either way */

    int value = enabled ? 1 : 0;
    if (value == last_enabled) return;
    if (mixer_ctl_set_value(balanced_ctl, 0, value) == 0) last_enabled = value;
}

static pthread_once_t volume_worker_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t volume_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t volume_worker_cond = PTHREAD_COND_INITIALIZER;
static bool volume_worker_ready = false;
static bool volume_worker_pending = false;
static int volume_worker_left;
static int volume_worker_right;
/* Same mutex/cond as the volume request above -- one worker thread, two
 * kinds of pending work, checked together on every wake rather than
 * spinning up a second thread for something that changes this rarely. */
static bool balanced_worker_pending = false;
static bool balanced_worker_enabled;

static void * volume_worker_main(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&volume_worker_mutex);
        while (!volume_worker_pending && !balanced_worker_pending)
            pthread_cond_wait(&volume_worker_cond, &volume_worker_mutex);
        bool do_volume = volume_worker_pending;
        int left = volume_worker_left;
        int right = volume_worker_right;
        volume_worker_pending = false;
        bool do_balanced = balanced_worker_pending;
        bool balanced_enabled = balanced_worker_enabled;
        balanced_worker_pending = false;
        pthread_mutex_unlock(&volume_worker_mutex);
        if (do_volume) audio_output_set_hw_volume_raw(left, right);
        if (do_balanced) apply_balanced_output(balanced_enabled);
    }
    return NULL;
}

static void start_volume_worker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, volume_worker_main, NULL) == 0) {
        pthread_detach(thread);
        volume_worker_ready = true;
    }
}

/* UI-thread-safe half of balanced-output sync -- called every tick from
 * gui.c's update_timer_cb(). Everything here is either a plain sysfs read
 * (balanced_headphone_is_connected()) or state private to this one
 * function (own static, never touched by the worker thread), so there is
 * nothing to synchronize on this side; only the actual mixer write
 * (apply_balanced_output() above) needs to stay on the single worker
 * thread, same as hardware volume already does. */
void audio_output_sync_balanced_output(void) {
    static int last_requested = -1; /* -1 = never requested yet */
    int enabled = balanced_headphone_is_connected() ? 1 : 0;
    if (enabled == last_requested) return;
    last_requested = enabled;

    pthread_once(&volume_worker_once, start_volume_worker);
    if (!volume_worker_ready) {
        apply_balanced_output(enabled != 0);
        return;
    }
    pthread_mutex_lock(&volume_worker_mutex);
    balanced_worker_enabled = enabled != 0;
    balanced_worker_pending = true;
    pthread_cond_signal(&volume_worker_cond);
    pthread_mutex_unlock(&volume_worker_mutex);
}

void audio_output_request_hw_volume_raw(int raw_left, int raw_right) {
    pthread_once(&volume_worker_once, start_volume_worker);
    if (!volume_worker_ready) {
        audio_output_set_hw_volume_raw(raw_left, raw_right);
        return;
    }
    pthread_mutex_lock(&volume_worker_mutex);
    volume_worker_left = raw_left;
    volume_worker_right = raw_right;
    volume_worker_pending = true;
    pthread_cond_signal(&volume_worker_cond);
    pthread_mutex_unlock(&volume_worker_mutex);
}

bool audio_output_is_usb_active(void) {
    return active_target == OUTPUT_TARGET_USB;
}
