/*
 * tools/s24_hw_params_probe.c
 *
 * Standalone tinyalsa diagnostic tool for the HiBy R1 / R3 Pro II (Ingenic X1600
 * SoC, Cirrus Logic CS43131 DAC, Linux, tinyalsa).
 *
 * BACKGROUND & MOTIVATION:
 * The open_hiby_player app recently gained native 24-bit audio output for local
 * playback (FLAC, WAV, ALAC, APE, AIFF) via PCM_FORMAT_S24_LE on card 0, device 0.
 * While real-device hardware parameters dump (via `aplay -D hw:0,0 --dump-hw-params`)
 * confirmed that the kernel ALSA driver accepts S24_LE in principle, the driver's
 * buffer sizing constraints are notoriously finicky:
 *
 * When tuning the S16_LE low-latency and standard playback paths in
 * src/audio/audio_output.c:162-210, period_size=1024 / period_count=2 (~21ms buffer)
 * was initially attempted and completely broke audio output. Root-cause analysis
 * revealed that pcm_open()'s hw_params negotiation failed with EINVAL because the
 * Ingenic X1600 audio driver enforces strict constraints on (period_size, period_count)
 * pairs (e.g. 1024x2, 1024x3, 2048x2, 2048x3, 4096x2 all failed hw_params, while
 * 1024x4 and 2048x4 succeeded).
 *
 * For S24_LE, sample words are 32-bit (4 bytes per sample vs. 2 bytes for S16_LE),
 * doubling byte throughput and DMA FIFO burst requirements for a given frame count.
 * The audio pipeline currently reuses the S16_LE configurations (2048x4 standard,
 * 1024x4 low latency) for S24_LE at 44.1 kHz, but this has never been swept across
 * high-resolution sample rates (48 kHz through 384 kHz) or channel counts.
 *
 * PURPOSE:
 * This standalone diagnostic program performs an exhaustive matrix probe across:
 *   - 8 standard sample rates (44.1, 48, 88.2, 96, 176.4, 192, 352.8, 384 kHz)
 *   - 2 channel modes (1 = Mono, 2 = Stereo)
 *   - 10 (period_size, period_count) pairs (512x4, 1024x2, 1024x3, 1024x4, 2048x2,
 *     2048x3, 2048x4, 4096x2, 4096x4, 8192x2)
 * Total: 160 configurations.
 *
 * For each configuration:
 *   1. Configures struct pcm_config with PCM_FORMAT_S24_LE and start/stop thresholds.
 *   2. Invokes pcm_open(0, 0, PCM_OUT, &config) and pcm_is_ready(pcm).
 *   3. If negotiation succeeds, performs three consecutive pcm_writei() calls of zeroed
 *      audio (one period per write) to verify that the driver's DMA engine accepts
 *      and consumes actual audio frames without immediate EPIPE, EIO, or short writes.
 *   4. Closes the PCM handle and reports PASS or FAIL (<reason>).
 *   5. Outputs a summary reporting total pass/fail and the first passing configuration
 *      for each (sample_rate, channels) pair.
 *
 * THIS FILE IS A DISPOSABLE DIAGNOSTIC TOOL. DO NOT wire this into the main Makefile
 * or ship it as part of the application binary.
 *
 * CROSS-COMPILATION INSTRUCTIONS:
 * Run from the repository root using the mipsel-linux-musl cross toolchain:
 *
 *   mipsel-linux-musl-gcc -O2 -Wall -static \
 *       -Itinyalsa/include -Itinyalsa/src \
 *       tools/s24_hw_params_probe.c \
 *       tinyalsa/src/pcm.c tinyalsa/src/pcm_hw.c tinyalsa/src/pcm_plugin.c \
 *       tinyalsa/src/snd_card_plugin.c tinyalsa/src/mixer.c tinyalsa/src/mixer_hw.c \
 *       tinyalsa/src/mixer_plugin.c tinyalsa/src/limits.c \
 *       -o s24_hw_params_probe
 *
 * ON-DEVICE DEPLOYMENT & EXECUTION (via adb):
 * Following TESTING.md guidelines for the HiBy R1:
 *
 *   1. Kill the stock player processes first so ALSA card 0 is completely released
 *      (otherwise pcm_open will fail with EBUSY):
 *      adb shell "killall -9 hiby_player.sh open_hiby_player 2>/dev/null; sleep 1; ps | grep -iE 'hiby|player'"
 *
 *   2. Push to writable /usr/data/ (/usr/bin/ is read-only squashfs):
 *      adb push s24_hw_params_probe /usr/data/s24_hw_params_probe
 *      adb shell "chmod +x /usr/data/s24_hw_params_probe"
 *
 *   3. Run the probe:
 *      adb shell "/usr/data/s24_hw_params_probe"
 *
 *   4. Clean up after testing:
 *      adb shell "rm -f /usr/data/s24_hw_params_probe"
 */

#include <tinyalsa/asoundlib.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALSA_CARD 0
#define ALSA_DEVICE 0

/* Sample rates typical for 24-bit PCM playback (44.1 kHz to 384 kHz) */
static const unsigned int rates[] = {
    44100,
    48000,
    88200,
    96000,
    176400,
    192000,
    352800,
    384000
};
#define NUM_RATES (sizeof(rates) / sizeof(rates[0]))

/* Mono and Stereo */
static const unsigned int channels_list[] = {
    1,
    2
};
#define NUM_CHANNELS (sizeof(channels_list) / sizeof(channels_list[0]))

/* Buffer period configurations to probe */
struct period_pair {
    unsigned int period_size;
    unsigned int period_count;
};

static const struct period_pair period_pairs[] = {
    {  512, 4 },
    { 1024, 2 },
    { 1024, 3 },
    { 1024, 4 },
    { 2048, 2 },
    { 2048, 3 },
    { 2048, 4 },
    { 4096, 2 },
    { 4096, 4 },
    { 8192, 2 }
};
#define NUM_PERIOD_PAIRS (sizeof(period_pairs) / sizeof(period_pairs[0]))

struct first_pass_entry {
    unsigned int rate;
    unsigned int channels;
    bool found;
    unsigned int period_size;
    unsigned int period_count;
};

int main(void) {
    printf("======================================================================\n");
    printf("HiBy R1 / Ingenic X1600 ALSA Hardware Parameters Probe (PCM_FORMAT_S24_LE)\n");
    printf("Target Card: %d, Device: %d, Flags: PCM_OUT\n", ALSA_CARD, ALSA_DEVICE);
    printf("Matrix: %u rates x %u channel configs x %u period pairs = %u combinations\n",
           (unsigned int)NUM_RATES, (unsigned int)NUM_CHANNELS, (unsigned int)NUM_PERIOD_PAIRS,
           (unsigned int)(NUM_RATES * NUM_CHANNELS * NUM_PERIOD_PAIRS));
    printf("======================================================================\n\n");
    fflush(stdout);

    struct first_pass_entry first_passing[NUM_RATES * NUM_CHANNELS];
    size_t fp_idx = 0;
    for (size_t r = 0; r < NUM_RATES; r++) {
        for (size_t c = 0; c < NUM_CHANNELS; c++) {
            first_passing[fp_idx].rate = rates[r];
            first_passing[fp_idx].channels = channels_list[c];
            first_passing[fp_idx].found = false;
            first_passing[fp_idx].period_size = 0;
            first_passing[fp_idx].period_count = 0;
            fp_idx++;
        }
    }

    unsigned int total_combinations = 0;
    unsigned int total_pass = 0;
    unsigned int total_fail = 0;

    for (size_t r = 0; r < NUM_RATES; r++) {
        unsigned int rate = rates[r];

        for (size_t c = 0; c < NUM_CHANNELS; c++) {
            unsigned int ch = channels_list[c];
            size_t entry_idx = r * NUM_CHANNELS + c;

            for (size_t p = 0; p < NUM_PERIOD_PAIRS; p++) {
                unsigned int ps = period_pairs[p].period_size;
                unsigned int pc = period_pairs[p].period_count;
                total_combinations++;

                struct pcm_config config;
                memset(&config, 0, sizeof(config));
                config.channels = ch;
                config.rate = rate;
                config.period_size = ps;
                config.period_count = pc;
                config.format = PCM_FORMAT_S24_LE;
                /* Matches tinyalsa convention used in audio_output.c:open_device() */
                config.start_threshold = (unsigned long)ps * pc;
                config.stop_threshold = (unsigned long)ps * pc;

                struct pcm *pcm = pcm_open(ALSA_CARD, ALSA_DEVICE, PCM_OUT, &config);
                if (!pcm || !pcm_is_ready(pcm)) {
                    const char *err = pcm ? pcm_get_error(pcm) : "pcm_open returned NULL";
                    printf("rate=%-6u channels=%u period_size=%-4u period_count=%u -> FAIL (%s)\n",
                           rate, ch, ps, pc, err);
                    fflush(stdout);
                    if (pcm) {
                        pcm_close(pcm);
                    }
                    total_fail++;
                    continue;
                }

                /* S24_LE format bits: 32 bits = 4 bytes per sample */
                size_t bytes_per_sample = pcm_format_to_bits(config.format) / 8;
                size_t frame_bytes = (size_t)ch * bytes_per_sample;
                size_t period_bytes = (size_t)ps * frame_bytes;

                void *zero_buf = calloc(1, period_bytes);
                if (!zero_buf) {
                    printf("rate=%-6u channels=%u period_size=%-4u period_count=%u -> FAIL (calloc failed)\n",
                           rate, ch, ps, pc);
                    fflush(stdout);
                    pcm_close(pcm);
                    total_fail++;
                    continue;
                }

                /* Write 3 full periods of silence to verify DMA consumption and backpressure */
                bool write_ok = true;
                char write_err[256] = {0};

                for (int w = 0; w < 3; w++) {
                    int ret = pcm_writei(pcm, zero_buf, ps);
                    if (ret < 0) {
                        snprintf(write_err, sizeof(write_err), "%s", pcm_get_error(pcm));
                        write_ok = false;
                        break;
                    } else if ((unsigned int)ret != ps) {
                        snprintf(write_err, sizeof(write_err), "short write: wrote %d of %u frames", ret, ps);
                        write_ok = false;
                        break;
                    }
                }

                free(zero_buf);
                pcm_close(pcm);

                if (!write_ok) {
                    printf("rate=%-6u channels=%u period_size=%-4u period_count=%u -> FAIL (%s)\n",
                           rate, ch, ps, pc, write_err);
                    fflush(stdout);
                    total_fail++;
                } else {
                    printf("rate=%-6u channels=%u period_size=%-4u period_count=%u -> PASS\n",
                           rate, ch, ps, pc);
                    fflush(stdout);
                    total_pass++;

                    if (!first_passing[entry_idx].found) {
                        first_passing[entry_idx].found = true;
                        first_passing[entry_idx].period_size = ps;
                        first_passing[entry_idx].period_count = pc;
                    }
                }
            }
        }
    }

    printf("\n======================================================================\n");
    printf("SWEEP SUMMARY: %u combinations tried | %u PASS | %u FAIL\n",
           total_combinations, total_pass, total_fail);
    printf("======================================================================\n\n");

    printf("--- First Passing (period_size x period_count) by (rate, channels) ---\n");
    for (size_t i = 0; i < NUM_RATES * NUM_CHANNELS; i++) {
        if (first_passing[i].found) {
            unsigned int buf_frames = first_passing[i].period_size * first_passing[i].period_count;
            double latency_ms = ((double)buf_frames * 1000.0) / (double)first_passing[i].rate;
            printf("rate=%-6u channels=%u -> %ux%u (buffer %u frames, ~%.1f ms)\n",
                   first_passing[i].rate,
                   first_passing[i].channels,
                   first_passing[i].period_size,
                   first_passing[i].period_count,
                   buf_frames,
                   latency_ms);
        } else {
            printf("rate=%-6u channels=%u -> NONE (all tested period configs failed)\n",
                   first_passing[i].rate,
                   first_passing[i].channels);
        }
    }
    printf("======================================================================\n");
    fflush(stdout);

    return (total_fail == total_combinations) ? EXIT_FAILURE : EXIT_SUCCESS;
}
