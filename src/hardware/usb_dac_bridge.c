#include "usb_dac_bridge.h"

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

#define UAC_SA_DEVICE_PATH "/dev/uac_sa"
/* Neither of these match uac_device_config.sh's own gadget config
 * (c_chmask=0x03/c_ssize=2/c_srate=48000) -- confirmed empirically against
 * a real device instead, since the driver's actual behavior doesn't match
 * what it advertises (see usb_dac_bridge.h for why the real protocol
 * isn't known and this is empirical rather than derived):
 *   - Channels: of the two interleaved 16-bit slots per frame, only the
 *     second one (index 1) ever carries real audio -- the first is
 *     full-scale noise on every capture, not silence. Verified by
 *     comparing zero-crossing rate/amplitude of each slot in a raw capture,
 *     and that a byte-level shift doesn't fix it (rules out a framing
 *     offset -- it's a per-slot behavior, not misalignment). Worked around
 *     by overwriting the noise slot with the real one before playback
 *     rather than declaring 1 channel, since duplicating into fake stereo
 *     is simpler than reconfiguring tinyalsa/the hardware path for mono.
 *   - Rate: wall-clock-measured actual frame arrival rate on a real device
 *     converged to ~96300 frames/s, not the declared 48000 -- and 96000
 *     is also what sounded correct to a human listener, independently
 *     confirming the measurement. */
#define BRIDGE_CHANNELS 2
#define BRIDGE_SAMPLE_RATE 96000
#define BRIDGE_BIT_DEPTH 16
#define USB_ADVERTISED_SAMPLE_RATE 48000
#define USB_ADVERTISED_BIT_DEPTH 16
#define BRIDGE_PERIOD_FRAMES 1024

#define OPEN_RETRY_TIMEOUT_MS 5000
#define POLL_INTERVAL_MS 200

static pthread_mutex_t bridge_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool bridge_running = false;
static bool bridge_streaming = false;
static volatile bool stop_requested = false;

#ifndef HOST_BUILD
static void set_running(bool running) {
    pthread_mutex_lock(&bridge_mutex);
    bridge_running = running;
    if (!running) bridge_streaming = false;
    pthread_mutex_unlock(&bridge_mutex);
}

static void set_streaming(bool streaming) {
    pthread_mutex_lock(&bridge_mutex);
    bridge_streaming = streaming;
    pthread_mutex_unlock(&bridge_mutex);
}

static void * bridge_thread_func(void * arg) {
    (void) arg;

    /* audio_stop() (called by usb_dac_bridge_start() before spawning this
     * thread) only signals the audio thread -- audio_output_close() happens
     * asynchronously on that thread. Wait for it to actually finish before
     * touching the shared output device ourselves, or audio_output_ensure()
     * below fails with the same "Resource busy" this project has already
     * hit once (see subprocess.c's close_inherited_fds() doc comment for
     * that incident). */
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (!audio_is_playing() && !audio_is_paused()) break;
        usleep(20000);
    }

    int uac_fd = -1;
    for (int waited_ms = 0; waited_ms < OPEN_RETRY_TIMEOUT_MS; waited_ms += POLL_INTERVAL_MS) {
        if (stop_requested) {
            set_running(false);
            return NULL;
        }
        uac_fd = open(UAC_SA_DEVICE_PATH, O_RDWR);
        if (uac_fd >= 0) break;
        usleep(POLL_INTERVAL_MS * 1000);
    }
    if (uac_fd < 0) {
        fprintf(stderr, "usb_dac_bridge: %s never appeared, giving up\n", UAC_SA_DEVICE_PATH);
        set_running(false);
        return NULL;
    }

    /* REVERTED (was briefly true): a real-device bug report asked for this
     * ("Latency is very high, audio is even out of sync with the videos"),
     * matching the same low-latency tuning airplay_bridge.c already uses
     * successfully (~21ms vs ~85ms of ALSA buffering at this bridge's own
     * 96kHz rate) -- but a second real-device report after trying it was
     * "USB Dac mode is completely broken now... not emitting any sound at
     * all", not crackling/dropouts, which reads as pcm_open() itself
     * failing at the smaller period size on this specific hardware/kernel
     * rather than an underrun. This is exactly the risk the original
     * comment here already flagged as "untested on real USB-DAC hardware"
     * before ever trying it. Do not flip this back to true without a live
     * device confirming pcm_open() actually succeeds at period_size=1024/
     * period_count=2 at 96000Hz on this exact hardware -- if it does, the
     * AV-sync bug report above is still real and worth revisiting. */
    if (!audio_output_ensure(BRIDGE_CHANNELS, BRIDGE_SAMPLE_RATE, false)) {
        fprintf(stderr, "usb_dac_bridge: audio_output_ensure failed\n");
        close(uac_fd);
        set_running(false);
        return NULL;
    }

    size_t frame_bytes = (size_t) BRIDGE_CHANNELS * sizeof(int16_t);
    size_t buf_bytes = (size_t) BRIDGE_PERIOD_FRAMES * frame_bytes;
    uint8_t * buf = malloc(buf_bytes);

    uint64_t total_bytes_read = 0;
    int poll_timeouts_in_a_row = 0;
    int exit_reason_n = 0; /* read()'s return value that broke the loop, for the exit log below */
    int exit_reason_errno = 0;

    /* EPERM here (as opposed to e.g. EIO/ENODEV) matches the shape of a
     * driver-side "host hasn't armed the streaming interface yet" gate
     * (uac_setup_complete, alongside uac_read/uac_ioctl in /proc/kallsyms,
     * looks like the USB gadget framework's SET_INTERFACE-completion
     * callback) rather than a permanent failure -- retried with backoff
     * instead of treated as fatal on the first hit, up to a bound generous
     * enough for a PC's audio stack to actually finish selecting the
     * device after enumeration. */
    int eperm_retries = 0;
    #define EPERM_RETRY_LIMIT 75 /* ~15s at POLL_INTERVAL_MS between retries */

    while (!stop_requested) {
        struct pollfd pfd = { .fd = uac_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, POLL_INTERVAL_MS);
        if (pr <= 0) {
            set_streaming(false);
            poll_timeouts_in_a_row++;
            if (poll_timeouts_in_a_row == 25) /* ~5s of nothing but still open -- worth knowing */
                fprintf(stderr, "usb_dac_bridge: no data from %s in ~5s (still waiting)\n", UAC_SA_DEVICE_PATH);
            continue; /* timeout (check stop_requested again) or poll() error */
        }
        poll_timeouts_in_a_row = 0;

        ssize_t n = read(uac_fd, buf, buf_bytes);
        if (n <= 0) {
            set_streaming(false);
            if (errno == EPERM && eperm_retries < EPERM_RETRY_LIMIT) {
                eperm_retries++;
                if (eperm_retries == 1)
                    fprintf(stderr, "usb_dac_bridge: read() EPERM, retrying for up to ~15s "
                                     "(host may not have armed the streaming interface yet)\n");
                usleep(POLL_INTERVAL_MS * 1000);
                continue;
            }
            exit_reason_n = (int) n;
            exit_reason_errno = errno;
            break; /* device closed/error -- gadget likely torn down underneath us */
        }
        if (eperm_retries > 0) {
            fprintf(stderr, "usb_dac_bridge: read() succeeded after %d EPERM retries\n", eperm_retries);
            eperm_retries = 0;
        }

        total_bytes_read += (uint64_t) n;
        set_streaming(true);

        /* Same reasoning as audio.c's own playback loop (see its comment
         * above the equivalent call): the requested Bluetooth target can
         * change at any moment from the GUI thread's poll
         * (usb_dac_bridge_set_bt_output()), and this stream has no natural
         * "track boundary" to piggyback the check on the way audio.c's
         * playback loop originally did, so it's checked every chunk here
         * unconditionally -- cheap, since audio_output_ensure() only
         * actually reopens if something changed. Must pass the same
         * low_latency value as the initial audio_output_ensure() call
         * above -- a mismatch here would make every single chunk look like
         * a mode change and force a reopen on every read, not just once. */
        audio_output_ensure(BRIDGE_CHANNELS, BRIDGE_SAMPLE_RATE, false);

        /* Overwrite the always-noise first channel slot with the real
         * second one -- see the empirical finding in this file's top
         * comment. */
        int16_t * frame_samples = (int16_t *) buf;
        size_t frame_count = (size_t) n / frame_bytes;
        for (size_t i = 0; i < frame_count; i++) {
            frame_samples[i * 2] = frame_samples[i * 2 + 1];
        }

        /* Ignore write failure: the bridge has no per-frame recovery --
         * audio_output_ensure() at the top of the next iteration handles
         * reconnection if aplay died. */
        (void) audio_output_write(frame_samples, frame_count, BRIDGE_CHANNELS, NULL);
    }

    if (!stop_requested) {
        fprintf(stderr, "usb_dac_bridge: read() returned %d (errno=%d %s) after %llu bytes total -- exiting\n",
                exit_reason_n, exit_reason_errno, strerror(exit_reason_errno), (unsigned long long) total_bytes_read);
    } else {
        fprintf(stderr, "usb_dac_bridge: stopped, %llu bytes read total\n", (unsigned long long) total_bytes_read);
    }

    free(buf);
    audio_output_close();
    close(uac_fd);
    set_running(false);
    return NULL;
}
#endif

void usb_dac_bridge_start(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&bridge_mutex);
    if (bridge_running) {
        pthread_mutex_unlock(&bridge_mutex);
        return;
    }
    bridge_running = true;
    pthread_mutex_unlock(&bridge_mutex);
    stop_requested = false;

    /* Real-device bug report: "USB DAC mode connected but not emitting any
     * sound", traced to audio_output.c's shared requested_target: an
     * external-USB-DAC-accessory request (audio_output_set_usb_requested(),
     * "USB Audio Output" -- a completely different, fully automatic feature,
     * see gui_shell.c's poll_usb_audio_output()) takes priority over both
     * Bluetooth and local in recompute_requested_target(). That poll ran
     * unconditionally regardless of usb_mode and had (falsely, it turned
     * out) detected an external accessory while actually in DAC/gadget
     * mode -- see poll_usb_audio_output()'s own updated comment for the
     * real fix (guarding that poll against USB_MODE_DAC, since it runs
     * continuously and would otherwise re-set this flag again within one
     * tick of any one-shot clear here). This bridge's own writes then
     * silently tried to reopen aplay against a nonexistent external
     * accessory device string instead of local hardware -- reads from
     * /dev/uac_sa kept succeeding (so the PC side looked "connected" and
     * the incoming-format display kept updating) while every write went
     * nowhere. Kept here too, redundantly but harmlessly, purely for
     * immediate correctness at the exact moment DAC mode starts rather than
     * waiting up to one poll tick for the real fix above to take effect --
     * being a USB gadget (device) is physically incompatible with
     * simultaneously being a USB host for an external accessory, so
     * clearing this is always safe regardless of how it got set.
     * usb_dac_bridge_set_bt_output() is untouched -- routing this bridge's
     * own output to Bluetooth is a real, independent feature (see this
     * file's own header comment) and must not be cleared here. */
    audio_output_set_usb_requested(false, NULL);

    audio_stop();

    pthread_t thread;
    pthread_create(&thread, NULL, bridge_thread_func, NULL);
    pthread_detach(thread);
#endif
}

void usb_dac_bridge_stop(void) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&bridge_mutex);
    bool was_running = bridge_running;
    pthread_mutex_unlock(&bridge_mutex);
    if (!was_running) return;

    stop_requested = true;

    /* No join() available (the thread is detached, since usb_dac_bridge_start()
     * doesn't keep its handle around) -- poll for the thread clearing
     * bridge_running itself instead, same pattern as subprocess.c's bounded
     * waitpid() polling loop. */
    for (int waited_ms = 0; waited_ms < 3000; waited_ms += 50) {
        pthread_mutex_lock(&bridge_mutex);
        bool still_running = bridge_running;
        pthread_mutex_unlock(&bridge_mutex);
        if (!still_running) return;
        usleep(50000);
    }
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
    pthread_mutex_unlock(&bridge_mutex);
    out->input_sample_rate = USB_ADVERTISED_SAMPLE_RATE;
    out->input_bit_depth = USB_ADVERTISED_BIT_DEPTH;
    out->output_sample_rate = BRIDGE_SAMPLE_RATE;
    out->output_bit_depth = BRIDGE_BIT_DEPTH;
}
