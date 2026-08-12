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

/* See audio_output_set_bt_requested()'s doc comment in audio_output.h for
 * the real-device bug this exists to fix (originally found/fixed for
 * audio.c's own playback path; this file is that same fix, extracted so
 * usb_dac_bridge.c can share it rather than re-deriving it). bt_requested
 * is set from the GUI thread (poll_refresh_bt_icon() -> either
 * audio_set_bt_output() or usb_dac_bridge_set_bt_output(), both of which
 * call audio_output_set_bt_requested()); bt_active records which output
 * audio_output_ensure() actually has open right now, so a mismatch
 * between the two (checked in audio_output_ensure(), same place a
 * sample-rate change is checked) is what triggers a reopen -- same lazy,
 * only-reopen-when-something-actually-changed shape the rate/channel check
 * already had, just with one more axis. tinyalsa itself only ever talks to
 * numbered hw: cards, so the Bluetooth path is a completely separate
 * mechanism: pipe raw PCM into `aplay -D bluealsa`, letting the real ALSA
 * library (which aplay is linked against, unlike this app) resolve
 * "bluealsa" through /etc/alsa/conf.d/20-bluealsa.conf -- confirmed by
 * reading that file directly: `pcm.bluealsa` is already `type plug` wrapping
 * the raw bluealsa ioplug with `device 00:00:00:00:00:00` (BlueALSA's own
 * "most recently connected device" default) and `profile a2dp`, so no MAC
 * address bookkeeping is needed here, and the "plug" wrapper transparently
 * handles any caller's sample rate without this code needing its own
 * resampler. */
static bool bt_requested = false;
static bool bt_active = false;
static pid_t bt_aplay_pid = -1;
static int bt_aplay_fd = -1;

static unsigned int device_channels = 0;
static unsigned int device_sample_rate = 0;

/* aplay's raw-PCM mode needs the format spelled out on the command line --
 * unlike a .wav it's reading straight off a pipe with no header to sniff
 * rate/channels/format from. `-D bluealsa` resolves through the real ALSA
 * library's plugin system (see the bt_requested doc comment above) rather
 * than tinyalsa, which is what makes this reachable at all. */
static bool open_bt_device(unsigned int channels, unsigned int sample_rate) {
    char rate_str[16], channels_str[8];
    snprintf(rate_str, sizeof(rate_str), "%u", sample_rate);
    snprintf(channels_str, sizeof(channels_str), "%u", channels);

    char * argv[] = { (char *) "aplay", (char *) "-q", (char *) "-D", (char *) "bluealsa",
                       (char *) "-t", (char *) "raw", (char *) "-f", (char *) "S16_LE",
                       (char *) "-r", rate_str, (char *) "-c", channels_str, NULL };
    if (!subprocess_popen_stdin(argv, &bt_aplay_pid, &bt_aplay_fd)) {
        DBG_LOG("audio_output: failed to spawn aplay for Bluetooth output\n");
        return false;
    }
    return true;
}

static void close_bt_device(void) {
    bt_active = false; /* only open_device() below was ever setting this back to false, never this side */
    if (bt_aplay_pid < 0) return;
    subprocess_terminate(bt_aplay_pid);
    close(bt_aplay_fd);
    bt_aplay_pid = -1;
    bt_aplay_fd = -1;
}

static bool open_device(unsigned int channels, unsigned int sample_rate) {
    if (bt_requested) {
        if (!open_bt_device(channels, sample_rate)) return false;
        bt_active = true;
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
        bt_active = false;
    }
    device_channels = channels;
    device_sample_rate = sample_rate;
    return true;
}

void audio_output_close(void) {
    if (bt_active) close_bt_device();
    if (alsa_pcm) { pcm_close(alsa_pcm); alsa_pcm = NULL; }
    device_channels = 0;
    device_sample_rate = 0;
}

bool audio_output_ensure(unsigned int channels, unsigned int sample_rate) {
    /* Real-device bug: aplay can die entirely on its own (BlueZ tearing
     * down the transport underneath it -- a headset bonding hiccup during
     * testing was one confirmed trigger, but any A2DP renegotiation could
     * do the same) while bt_requested never changes (this app's own ~5s
     * connection poll still reports "connected" the whole time, since the
     * radio-level connection can outlive the specific audio transport that
     * died). The bt_active != bt_requested check below alone can't catch
     * that -- both sides still agree "Bluetooth is what we want" -- so a
     * dead aplay would otherwise go unnoticed forever, with every future
     * write() failing instantly and audio_output_write()'s own pacing
     * fallback silently swallowing every chunk. Checking liveness
     * (WNOHANG, not a blocking wait -- this runs on the hot path) here
     * catches it and forces the reopen below to actually respawn aplay,
     * the same as if the target itself had changed. */
    if (bt_active && bt_aplay_pid >= 0) {
        int status;
        if (waitpid(bt_aplay_pid, &status, WNOHANG) == bt_aplay_pid) {
            DBG_LOG("audio_output: aplay died unexpectedly, reopening\n");
            bt_aplay_pid = -1; /* already reaped above -- close_bt_device() below must not wait on it again */
            close(bt_aplay_fd);
            bt_aplay_fd = -1;
        }
    }

    bool device_open = (alsa_pcm != NULL || bt_aplay_pid >= 0);
    if (device_open && bt_active != bt_requested) device_open = false;
    if (device_open && device_channels == channels && device_sample_rate == sample_rate) return true;
    audio_output_close();
    return open_device(channels, sample_rate);
}

void audio_output_write(const int16_t * buf, uint64_t frames, unsigned int channels) {
    bool paced = false;
    if (bt_active) {
        /* No tinyalsa-style blocking-until-room primitive here -- aplay's
         * own ALSA write blocks on ITS end of the pipe once its buffer is
         * full, which backpressures this write() the same way pcm_writei()
         * paces the local path below. Looped for the same reason
         * subprocess_run_checked()'s read loop is: a pipe write can return
         * short. */
        const char * p = (const char *) buf;
        size_t remaining = (size_t) frames * channels * sizeof(int16_t);
        while (remaining > 0) {
            ssize_t n = write(bt_aplay_fd, p, remaining);
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

void audio_output_set_bt_requested(bool requested) {
    bt_requested = requested;
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
