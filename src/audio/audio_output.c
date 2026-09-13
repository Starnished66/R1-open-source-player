#include "audio_output.h"
#include "debug_log.h"
#include "subprocess.h"
#include "headphone_status.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>

#include <tinyalsa/asoundlib.h>
#include <tinyalsa/mixer.h>
#define ALSA_CARD 0
#define ALSA_DEVICE 0

static struct pcm * alsa_pcm = NULL;

/* The shared device handles have one logical producer at a time.  This gate
 * is held only for ownership bookkeeping; device I/O is never done under it. */
static pthread_mutex_t owner_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool owner_valid = false;
static pthread_t owner_thread;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool caller_owns_output(void) {
    bool owns;
    pthread_mutex_lock(&owner_mutex);
    owns = owner_valid && pthread_equal(owner_thread, pthread_self());
    pthread_mutex_unlock(&owner_mutex);
    return owns;
}

/* Three possible output targets: local (ALSA), Bluetooth (bluealsa/aplay),
 * and USB DAC (handled by usb_dac_bridge.c). A tri-state enum rather than
 * multiple bools, since exactly one target is active at any time. */
typedef enum {
    OUTPUT_TARGET_LOCAL = 0,
    OUTPUT_TARGET_BT,
    OUTPUT_TARGET_USB,
} output_target_t;

/* requested_target is set from the GUI thread (poll_refresh_bt_icon() ->
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
 * Meaningless for BT or USB (aplay's own ALSA config, not this one). */
static bool active_low_latency = false;

/* Which ALSA format the CURRENTLY OPEN device actually used (meaningful
 * across LOCAL, USB, and BT targets). Set by open_device() below, read by
 * audio_output_ensure() to detect format changes needing a reopen, and by
 * audio_output_is_s24_active() to verify if S24_LE was actually achieved. */
static enum pcm_format active_format = PCM_FORMAT_S16_LE;

/* Record of an observed hw_params rejection of PCM_FORMAT_S24_LE at a
 * specific (channels, sample_rate) -- set only inside open_device()'s
 * S16_LE fallback below. Exists so a real, reproducible rejection (S24_LE
 * period_size/period_count are explicitly unvalidated -- see open_device()'s
 * own TODO) doesn't get re-attempted on every single chunk of the same
 * track: without this, audio_output_ensure()'s format-mismatch check below
 * would see "S24_LE requested, S16_LE active" as a change worth reopening
 * for on every call, driving a close+reopen+fail+fallback cycle every chunk
 * for the rest of the track instead of settling once.
 *
 * Deliberately NOT permanent for the process's whole lifetime, though: a
 * single pcm_open() failure isn't necessarily a hardware fact -- it could be
 * transient (brief resource contention, a momentarily-busy device) rather
 * than "this config never works." Caching it forever would silently
 * downgrade every later track at this same (channels, rate) to S16_LE for
 * the rest of the session even after whatever caused the one failure has
 * long since cleared. audio_output_reset_s24_probe() clears this, called
 * once per NEW track becoming current (see its own doc comment and callers)
 * -- frequent enough that a real, reproducible hardware limitation still
 * gets caught again almost immediately (so the per-chunk thrashing this
 * exists to prevent stays prevented within any one track), infrequent
 * enough (once per track, not once per chunk) that a transient failure only
 * costs one extra failed pcm_open() attempt before the next track gets a
 * clean shot at S24_LE again. */
static bool s24_unsupported_known = false;
static unsigned int s24_unsupported_channels = 0;
static unsigned int s24_unsupported_rate = 0;

/* aplay's raw-PCM mode needs the format spelled out on the command line --
 * unlike a .wav it's reading straight off a pipe with no header to sniff
 * rate/channels/format from. `-D <device>` resolves through the real ALSA
 * library's plugin system (see requested_target's own doc comment above)
 * rather than tinyalsa, which is what makes either of these reachable at
 * all. Shared by both the Bluetooth and USB targets -- only the device
 * string and which pid/fd pair get filled in differ. */
static bool spawn_aplay(const char * device, unsigned int channels, unsigned int sample_rate,
                         enum pcm_format format, pid_t * out_pid, int * out_fd) {
    char rate_str[16], channels_str[8];
    snprintf(rate_str, sizeof(rate_str), "%u", sample_rate);
    snprintf(channels_str, sizeof(channels_str), "%u", channels);

    const char * format_str = (format == PCM_FORMAT_S24_LE) ? "S24_LE" : "S16_LE";

    char * argv[] = { (char *) "aplay", (char *) "-q", (char *) "-D", (char *) device,
                       (char *) "-t", (char *) "raw", (char *) "-f", (char *) format_str,
                       (char *) "-r", rate_str, (char *) "-c", channels_str, NULL };
    if (!subprocess_popen_stdin(argv, out_pid, out_fd)) {
        DBG_LOG("audio_output: failed to spawn aplay for '%s' output\n", device);
        return false;
    }
    int flags = fcntl(*out_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(*out_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        subprocess_terminate(*out_pid);
        close(*out_fd);
        *out_pid = -1;
        *out_fd = -1;
        DBG_LOG("audio_output: failed to set aplay pipe non-blocking for '%s' output\n", device);
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

static bool open_device(unsigned int channels, unsigned int sample_rate, bool low_latency, bool want_s24,
                        output_target_t target, const char * usb_device) {
    if (target == OUTPUT_TARGET_USB) {
        enum pcm_format format = want_s24 ? PCM_FORMAT_S24_LE : PCM_FORMAT_S16_LE;
        if (!spawn_aplay(usb_device, channels, sample_rate, format, &usb_aplay_pid, &usb_aplay_fd)) return false;
        pthread_mutex_lock(&state_mutex);
        active_target = OUTPUT_TARGET_USB;
        active_format = format;
        pthread_mutex_unlock(&state_mutex);
    } else if (target == OUTPUT_TARGET_BT) {
        if (!spawn_aplay("bluealsa", channels, sample_rate, PCM_FORMAT_S16_LE, &bt_aplay_pid, &bt_aplay_fd)) return false;
        pthread_mutex_lock(&state_mutex);
        active_target = OUTPUT_TARGET_BT;
        active_format = PCM_FORMAT_S16_LE;
        pthread_mutex_unlock(&state_mutex);
    } else {
        struct pcm_config config;
        memset(&config, 0, sizeof(config));
        config.channels = channels;
        config.rate = sample_rate;
        config.format = want_s24 ? PCM_FORMAT_S24_LE : PCM_FORMAT_S16_LE;
        pthread_mutex_lock(&state_mutex);
        active_format = config.format;
        pthread_mutex_unlock(&state_mutex);
        active_low_latency = low_latency;
        if (low_latency) {
            /* Low-latency buffer tuning: 1024 frames x 4 periods (~43ms at 96kHz)
             * to reduce latency for live sources like AirPlay. */
            config.period_size = 1024;
            config.period_count = 4;
        } else {
            /* Standard buffer tuning targets a constant ~170-190ms of
             * underrun tolerance regardless of sample rate, not a constant
             * frame count. period_size was previously a flat 2048 frames at
             * every rate -- ~186ms at 44.1kHz, but only ~85ms at 96kHz and
             * ~43ms at 192kHz, since a fixed frame count covers proportionally
             * less wall-clock time as the rate rises. That halved-or-worse
             * buffer margin at high sample rates was the primary suspect in a
             * real user report of stutter on 24-bit/96kHz FLAC (investigated
             * with Codex/Grok/Gemini -- all three independently converged on
             * this as the dominant cause; see ISSUES.md).
             *
             * period_count=4 is a hardware-validated constant, not a free
             * variable: tools/s24_hw_params_probe.c swept period_count in
             * {2,3,4} at period_size in {512,1024,2048,4096,8192} across
             * every standard rate 44.1kHz-384kHz and both S16_LE/S24_LE on a
             * real R1 -- period_count 2 and 3 failed hw_params (EINVAL) in
             * every single case; only 4 ever worked. Only period_size is
             * scaled here, using values that same probe already confirmed
             * negotiate successfully at every rate. */
            if (sample_rate <= 48000) {
                config.period_size = 2048;  /* ~186ms @ 44.1kHz, ~171ms @ 48kHz */
            } else if (sample_rate <= 96000) {
                config.period_size = 4096;  /* ~171ms @ 88.2/96kHz */
            } else {
                config.period_size = 8192;  /* ~171ms @ 176.4/192kHz, ~85ms @ 352.8/384kHz (best available -- period_size 16384 was not part of the validated sweep) */
            }
            config.period_count = 4;
        }
        /* Explicit rather than left at the zeroed default -- matches
         * tinyalsa's own usual convention (full buffer size) rather than
         * relying on whatever the driver does with 0. */
        config.start_threshold = config.period_size * config.period_count;
        config.stop_threshold = config.period_size * config.period_count;

        alsa_pcm = pcm_open(ALSA_CARD, ALSA_DEVICE, PCM_OUT, &config);
        if (!alsa_pcm || !pcm_is_ready(alsa_pcm)) {
            DBG_LOG("audio_output: pcm_open failed (format=%d): %s\n", (int) config.format,
                    alsa_pcm ? pcm_get_error(alsa_pcm) : "unknown");
            if (alsa_pcm) pcm_close(alsa_pcm);
            alsa_pcm = NULL;
            if (!want_s24) return false;
            /* Fall back to S16_LE if S24_LE hw_params negotiation fails, and record
             * the unsupported format so subsequent chunks do not re-probe. */
            DBG_LOG("audio_output: S24_LE open failed, falling back to S16_LE\n");
            pthread_mutex_lock(&state_mutex);
            s24_unsupported_known = true;
            s24_unsupported_channels = channels;
            s24_unsupported_rate = sample_rate;
            pthread_mutex_unlock(&state_mutex);
            config.format = PCM_FORMAT_S16_LE;
            pthread_mutex_lock(&state_mutex);
            active_format = config.format;
            pthread_mutex_unlock(&state_mutex);
            alsa_pcm = pcm_open(ALSA_CARD, ALSA_DEVICE, PCM_OUT, &config);
            if (!alsa_pcm || !pcm_is_ready(alsa_pcm)) {
                DBG_LOG("audio_output: S16_LE fallback also failed: %s\n",
                        alsa_pcm ? pcm_get_error(alsa_pcm) : "unknown");
                if (alsa_pcm) pcm_close(alsa_pcm);
                alsa_pcm = NULL;
                return false;
            }
        }
        pthread_mutex_lock(&state_mutex);
        active_target = OUTPUT_TARGET_LOCAL;
        pthread_mutex_unlock(&state_mutex);
    }
    pthread_mutex_lock(&state_mutex);
    device_channels = channels;
    device_sample_rate = sample_rate;
    pthread_mutex_unlock(&state_mutex);
    return true;
}

static void close_device_state(void) {
    pthread_mutex_lock(&state_mutex);
    output_target_t target = active_target;
    struct pcm * pcm = alsa_pcm;
    alsa_pcm = NULL;
    pthread_mutex_unlock(&state_mutex);
    if (target == OUTPUT_TARGET_BT) close_bt_device();
    if (target == OUTPUT_TARGET_USB) close_usb_device();
    if (pcm) pcm_close(pcm);
    pthread_mutex_lock(&state_mutex);
    active_target = OUTPUT_TARGET_LOCAL; /* only open_device() above was ever setting this to BT/USB, never this side */
    active_format = PCM_FORMAT_S16_LE;
    device_channels = 0;
    device_sample_rate = 0;
    pthread_mutex_unlock(&state_mutex);
}

void audio_output_close(void) {
    if (!caller_owns_output()) return;
    close_device_state();
    pthread_mutex_lock(&owner_mutex);
    owner_valid = false;
    pthread_mutex_unlock(&owner_mutex);
}

bool audio_output_ensure(unsigned int channels, unsigned int sample_rate, bool low_latency, bool want_s24) {
    pthread_mutex_lock(&owner_mutex);
    if (owner_valid) {
        bool same_owner = pthread_equal(owner_thread, pthread_self());
        pthread_mutex_unlock(&owner_mutex);
        if (!same_owner) return false;
    } else {
        owner_thread = pthread_self();
        owner_valid = true;
        pthread_mutex_unlock(&owner_mutex);
    }

    pthread_mutex_lock(&config_mutex);
    output_target_t target = requested_target;
    char usb_device[sizeof(usb_alsa_device)];
    memcpy(usb_device, usb_alsa_device, sizeof(usb_device));
    pthread_mutex_unlock(&config_mutex);

    /* Already know this exact (channels, rate) fails S24_LE hw_params
     * negotiation (open_device()'s fallback set this on a real failure) --
     * downgrading here, before the format-mismatch check below, keeps a
     * fallen-back-to-S16 device from being torn down and re-attempted at
     * S24_LE on every subsequent chunk of the same track. */
    pthread_mutex_lock(&state_mutex);
    bool s24_known = s24_unsupported_known &&
        s24_unsupported_channels == channels && s24_unsupported_rate == sample_rate;
    pthread_mutex_unlock(&state_mutex);
    if (target == OUTPUT_TARGET_LOCAL && want_s24 && s24_known) {
        want_s24 = false;
    }

    /* Check if aplay died unexpectedly (e.g. transport disconnect) to force
     * reopening even if the requested output target has not changed. */
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
    if (device_open && active_target != target) device_open = false;
    /* Local and USB targets have format options -- a format or mode mismatch
     * must force a reopen the same as a channel/rate change. */
    enum pcm_format requested_format = ((target == OUTPUT_TARGET_LOCAL || target == OUTPUT_TARGET_USB) && want_s24)
                                       ? PCM_FORMAT_S24_LE : PCM_FORMAT_S16_LE;
    if (device_open && (target == OUTPUT_TARGET_LOCAL || target == OUTPUT_TARGET_USB) && active_format != requested_format) device_open = false;
    if (device_open && target == OUTPUT_TARGET_LOCAL && active_low_latency != low_latency) device_open = false;
    if (device_open && device_channels == channels && device_sample_rate == sample_rate) return true;
    close_device_state();
    bool opened = open_device(channels, sample_rate, low_latency, want_s24, target, usb_device);
    if (!opened) {
        pthread_mutex_lock(&owner_mutex);
        owner_valid = false;
        pthread_mutex_unlock(&owner_mutex);
    }
    return opened;
}

static bool write_pipe_bounded(int fd, const char * p, size_t remaining, size_t frame_bytes, size_t * out_written_bytes) {
    size_t total_bytes = remaining;
    size_t chunk_cap = PIPE_BUF - (PIPE_BUF % frame_bytes);
    if (frame_bytes > PIPE_BUF) chunk_cap = frame_bytes;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 2;

    while (remaining > 0) {
        size_t write_len = (remaining < chunk_cap) ? remaining : chunk_cap;
        ssize_t n = write(fd, p, write_len);
        if (n > 0) {
            p += n;
            remaining -= (size_t) n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long remaining_ms = (long long)(deadline.tv_sec - now.tv_sec) * 1000 + (deadline.tv_nsec - now.tv_nsec) / 1000000;
            if (remaining_ms <= 0) {
                errno = ETIMEDOUT;
                *out_written_bytes = total_bytes - remaining;
                return false;
            }
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int pr = poll(&pfd, 1, remaining_ms);
            if (pr == 0) {
                errno = ETIMEDOUT;
                *out_written_bytes = total_bytes - remaining;
                return false;
            }
            if (pr < 0 && errno == EINTR) {
                continue;
            }
            if (pr < 0) {
                *out_written_bytes = total_bytes - remaining;
                return false;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = EPIPE;
                *out_written_bytes = total_bytes - remaining;
                return false;
            }
            if (pfd.revents & POLLOUT) {
                continue;
            }
        }
        *out_written_bytes = total_bytes - remaining;
        return false;
    }
    *out_written_bytes = total_bytes;
    return true;
}

bool audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels, uint64_t * out_frames_written) {
    if (out_frames_written) *out_frames_written = 0;
    if (!caller_owns_output()) return false;
    if (audio_output_is_s24_active()) {
        DBG_LOG("audio_output: audio_output_write called while device is S24_LE (target=%d format=%d)\n",
                (int) active_target, (int) active_format);
        return false;
    }
    if (active_target == OUTPUT_TARGET_BT || active_target == OUTPUT_TARGET_USB) {
        int fd = (active_target == OUTPUT_TARGET_BT) ? bt_aplay_fd : usb_aplay_fd;
        if (fd < 0) {
            /* No pipe open -- pace so the caller's loop doesn't spin */
            unsigned int rate = device_sample_rate ? device_sample_rate : 44100;
            usleep((useconds_t) ((uint64_t) frames * 1000000ULL / rate));
            return false;
        }
        /* Pipe write: bounded, frame-safe. */
        const char * p = (const char *) buf;
        size_t frame_bytes = channels * sizeof(int16_t);
        size_t total_bytes = (size_t) frames * frame_bytes;
        size_t written_bytes = 0;
        
        if (!write_pipe_bounded(fd, p, total_bytes, frame_bytes, &written_bytes)) {
            int err = errno;
            if (out_frames_written) {
                *out_frames_written = written_bytes / frame_bytes;
            }
            
            output_target_t target = active_target;
            if (target == OUTPUT_TARGET_BT) {
                close_bt_device();
            } else if (target == OUTPUT_TARGET_USB) {
                close_usb_device();
            }
            
            if (err == ETIMEDOUT) {
                DBG_LOG("audio_output: BT/USB pipe write timed out after 2s, closing device (target=%d written=%" PRIu64 "/%" PRIu64 " frames)\n",
                        (int) target, out_frames_written ? *out_frames_written : 0ULL, frames);
            } else {
                DBG_LOG("audio_output: pipe write failed (target=%d errno=%d written=%" PRIu64 "/%" PRIu64 " frames)\n",
                        (int) target, err, out_frames_written ? *out_frames_written : 0ULL, frames);
            }
            return false;
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

bool audio_output_write_s24(const int32_t * buf, uint64_t frames, unsigned int channels, uint64_t * out_frames_written) {
    if (out_frames_written) *out_frames_written = 0;
    if (!caller_owns_output()) return false;
    /* Checking active_target alone is not enough: open_device()'s own S24_LE
     * hw_params fallback can leave active_target == OUTPUT_TARGET_LOCAL while
     * active_format == PCM_FORMAT_S16_LE. Handing this 32-bit-per-sample
     * buffer to a PCM handle actually configured for S16_LE (2 bytes/sample)
     * would not fail -- pcm_writei() sizes bytes from pcm->config.format, so
     * it would happily write frames*channels*2 bytes read from a buffer
     * meant to be read at 4 bytes/sample, i.e. real, silent sample
     * corruption reaching the DAC, not a clean error. audio_output_is_s24_
     * active() is the one true precondition for this function. */
    if (!audio_output_is_s24_active()) {
        DBG_LOG("audio_output: audio_output_write_s24 called while device is not S24_LE (target=%d format=%d)\n",
                (int) active_target, (int) active_format);
        return false;
    }

    if (active_target == OUTPUT_TARGET_USB) {
        int fd = usb_aplay_fd;
        if (fd < 0) {
            unsigned int rate = device_sample_rate ? device_sample_rate : 44100;
            usleep((useconds_t) ((uint64_t) frames * 1000000ULL / rate));
            return false;
        }
        const char * p = (const char *) buf;
        size_t frame_bytes = channels * sizeof(int32_t);
        size_t total_bytes = (size_t) frames * frame_bytes;
        size_t written_bytes = 0;
        
        if (!write_pipe_bounded(fd, p, total_bytes, frame_bytes, &written_bytes)) {
            int err = errno;
            if (out_frames_written) {
                *out_frames_written = written_bytes / frame_bytes;
            }
            close_usb_device();
            if (err == ETIMEDOUT) {
                DBG_LOG("audio_output: USB S24 pipe write timed out after 2s, closing device (written=%" PRIu64 "/%" PRIu64 " frames)\n",
                        out_frames_written ? *out_frames_written : 0ULL, frames);
            } else {
                DBG_LOG("audio_output: USB S24 pipe write failed (errno=%d written=%" PRIu64 "/%" PRIu64 " frames)\n",
                        err, out_frames_written ? *out_frames_written : 0ULL, frames);
            }
            return false;
        }
        if (out_frames_written) *out_frames_written = frames;
        return true;
    }

    if (alsa_pcm) {
        /* pcm_writei blocks until ALSA has room -- natural backpressure.
         * tinyalsa sizes bytes per frame internally from pcm->config.format
         * (PCM_FORMAT_S24_LE -> 4 bytes per sample / 8 bytes per stereo frame). */
        int ret = pcm_writei(alsa_pcm, buf, (unsigned int) frames);
        if (ret < 0 || (unsigned int) ret != (unsigned int) frames) {
            uint64_t written = (ret > 0) ? (uint64_t) ret : 0;
            if (out_frames_written) *out_frames_written = written;
            DBG_LOG("audio_output: s24 pcm_writei returned %d (wanted %u written=%" PRIu64 "): %s\n",
                    ret, (unsigned int) frames, written,
                    ret < 0 ? pcm_get_error(alsa_pcm) : "short write");
            return false;
        }
        if (out_frames_written) *out_frames_written = frames;
        return true;
    }

    unsigned int rate = device_sample_rate ? device_sample_rate : 44100;
    usleep((useconds_t) ((uint64_t) frames * 1000000ULL / rate));
    return false;
}

bool audio_output_is_local_requested(void) {
    pthread_mutex_lock(&config_mutex);
    bool local = requested_target == OUTPUT_TARGET_LOCAL;
    pthread_mutex_unlock(&config_mutex);
    return local;
}

bool audio_output_supports_wide_path(void) {
    pthread_mutex_lock(&config_mutex);
    bool supports = (requested_target == OUTPUT_TARGET_LOCAL || requested_target == OUTPUT_TARGET_USB);
    pthread_mutex_unlock(&config_mutex);
    return supports;
}

bool audio_output_is_s24_active(void) {
    pthread_mutex_lock(&state_mutex);
    bool active = (active_target == OUTPUT_TARGET_LOCAL || active_target == OUTPUT_TARGET_USB) && active_format == PCM_FORMAT_S24_LE;
    pthread_mutex_unlock(&state_mutex);
    return active;
}

void audio_output_reset_s24_probe(void) {
    pthread_mutex_lock(&state_mutex);
    s24_unsupported_known = false;
    pthread_mutex_unlock(&state_mutex);
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
    pthread_mutex_lock(&config_mutex);
    bt_requested = requested;
    recompute_requested_target();
    pthread_mutex_unlock(&config_mutex);
}

void audio_output_set_usb_requested(bool requested, const char * alsa_device) {
    pthread_mutex_lock(&config_mutex);
    usb_requested = requested;
    if (requested) snprintf(usb_alsa_device, sizeof(usb_alsa_device), "%s", alsa_device);
    recompute_requested_target();
    pthread_mutex_unlock(&config_mutex);
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

static void audio_output_set_hw_volume_raw(int raw_left, int raw_right) {
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

/* Route values for the R3 Pro II's "Output Port Switch" mixer control --
 * ported from a separate, already-working player for this same hardware. */
#define OUTPUT_PORT_HEADSET 2
#define OUTPUT_PORT_BALANCED 3
// TODO: add usb spdif output mode. reference https://github.com/hiby-modding/hiby_os_crack/blob/main/docs/r3proii/OUTPUT_MODES.md

/* Mirrors the ported detect_output(): balanced (4.4mm) takes priority over
 * plain 3.5mm if somehow both switch_dev nodes read connected at once,
 * otherwise 3.5mm, otherwise 3.5mm again as the default output port. */
static int detect_output_port(void) {
    return (get_headphone_state() == HEADPHONE_STATE_BALANCED) ? OUTPUT_PORT_BALANCED : OUTPUT_PORT_HEADSET;
}

/* Actual mixer write for output-port routing -- called from volume_worker_main()
 * to serialize access to ALSA mixer controls alongside hardware volume updates. */
static void apply_output_port(int port) {
    static struct mixer_ctl * port_ctl = NULL;
    static bool port_ctl_lookup_done = false;
    static int last_port = -1; /* -1 = never written yet */

    struct mixer * mixer = get_alsa_mixer();
    if (!mixer) return;
    if (!port_ctl_lookup_done) {
        port_ctl = mixer_get_ctl_by_name(mixer, "Output Port Switch");
        port_ctl_lookup_done = true;
    }
    if (!port_ctl) return; /* no such control -- R1/host; safe no-op either way */

    if (port == last_port) return;
    if (mixer_ctl_set_value(port_ctl, 0, port) == 0) last_port = port;
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
static bool output_port_worker_pending = false;
static int output_port_worker_value;

static void * volume_worker_main(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&volume_worker_mutex);
        while (!volume_worker_pending && !output_port_worker_pending)
            pthread_cond_wait(&volume_worker_cond, &volume_worker_mutex);
        bool do_volume = volume_worker_pending;
        int left = volume_worker_left;
        int right = volume_worker_right;
        volume_worker_pending = false;
        bool do_output_port = output_port_worker_pending;
        int output_port = output_port_worker_value;
        output_port_worker_pending = false;
        pthread_mutex_unlock(&volume_worker_mutex);
        if (do_volume) audio_output_set_hw_volume_raw(left, right);
        if (do_output_port) apply_output_port(output_port);
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

/* UI-thread-safe half of output-port routing sync -- called every tick from
 * gui.c's update_timer_cb(). Everything here is either a plain sysfs read
 * (detect_output_port()'s own calls) or state private to this one function
 * (own static, never touched by the worker thread), so there is nothing to
 * synchronize on this side; only the actual mixer write (apply_output_port()
 * above) needs to stay on the single worker thread, same as hardware volume
 * already does. */
void audio_output_sync_balanced_output(void) {
    static int last_requested = -1; /* -1 = never requested yet */
    int port = detect_output_port();
    if (port == last_requested) return;
    last_requested = port;

    pthread_once(&volume_worker_once, start_volume_worker);
    if (!volume_worker_ready) {
        apply_output_port(port);
        return;
    }
    pthread_mutex_lock(&volume_worker_mutex);
    output_port_worker_value = port;
    output_port_worker_pending = true;
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
    pthread_mutex_lock(&state_mutex);
    bool active = active_target == OUTPUT_TARGET_USB;
    pthread_mutex_unlock(&state_mutex);
    return active;
}
