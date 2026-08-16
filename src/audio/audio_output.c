#include "audio_output.h"
#include "debug_log.h"
#include "subprocess.h"

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

static bool open_device(unsigned int channels, unsigned int sample_rate) {
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
        config.period_size = 1024;
        config.period_count = 4;
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

bool audio_output_ensure(unsigned int channels, unsigned int sample_rate) {
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
    if (device_open && device_channels == channels && device_sample_rate == sample_rate) return true;
    audio_output_close();
    return open_device(channels, sample_rate);
}

void audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels) {
    bool paced = false;
    if (active_target == OUTPUT_TARGET_BT || active_target == OUTPUT_TARGET_USB) {
        /* No tinyalsa-style blocking-until-room primitive here -- aplay's
         * own ALSA write blocks on ITS end of the pipe once its buffer is
         * full, which backpressures this write() the same way pcm_writei()
         * paces the local path below. Looped for the same reason
         * subprocess_run_checked()'s read loop is: a pipe write can return
         * short. */
        int fd = (active_target == OUTPUT_TARGET_BT) ? bt_aplay_fd : usb_aplay_fd;
        const char * p = (const char *) buf;
        size_t remaining = (size_t) frames * channels * sizeof(int16_t);
        while (remaining > 0) {
            ssize_t n = write(fd, p, remaining);
            if (n <= 0) break; /* aplay died or the pipe broke -- drop this chunk, next audio_output_ensure() call will notice and reopen */
            p += n;
            remaining -= (size_t) n;
        }
        paced = (remaining == 0);
    } else if (alsa_pcm) {
        /* pcm_writei blocks until ALSA has room, which paces the caller's own loop naturally */
        (void) pcm_writei(alsa_pcm, buf, (unsigned int) frames);
        paced = true;
    }
    /* Real-device incident: when aplay dies out from under a live Bluetooth
     * connection (confirmed live -- BlueZ tearing down the PCM the instant
     * the headphones actually disconnect, which bluealsa then reports to
     * aplay as EOF/error, killing it), the write() above starts failing
     * instantly instead of blocking -- and unlike pcm_writei()'s failure
     * modes, an instantly-failing write() gives the caller's decode loop no
     * backpressure at all. Confirmed live (audio.c's own playback path,
     * before this was extracted): position tracking raced far ahead of real
     * time in the gap between the actual disconnect and the next ~5s GUI
     * connection poll noticing and flipping bt_requested back to local,
     * burning CPU decoding audio nobody will ever hear. Approximates this
     * chunk's real playback duration so the loop free-runs no faster than a
     * working device would have paced it, same as every other no-device-open
     * case (an initial open failure, mid-stream switch failure) already
     * silently falls into. */
    if (!paced) {
        unsigned int rate = device_sample_rate ? device_sample_rate : 44100;
        usleep((useconds_t) (frames * 1000000ULL / rate));
    }
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
    struct mixer * mixer = get_alsa_mixer();
    if (!mixer) return;
    struct mixer_ctl * left = mixer_get_ctl_by_name(mixer, "Left Playback Volume");
    struct mixer_ctl * right = mixer_get_ctl_by_name(mixer, "Right Playback Volume");
    if (left) mixer_ctl_set_value(left, 0, raw_left);
    if (right) mixer_ctl_set_value(right, 0, raw_right);
}

bool audio_output_is_usb_active(void) {
    return active_target == OUTPUT_TARGET_USB;
}
