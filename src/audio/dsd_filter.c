#include "dsd_filter.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void dsd_filter_design(dsd_filter_design_t * design, int num_taps, int decimation_factor) {
    if (num_taps > DSD_FILTER_MAX_TAPS) num_taps = DSD_FILTER_MAX_TAPS;

    design->num_taps = num_taps;
    design->decimation_factor = decimation_factor;

    double cutoff = 0.8 / (double) decimation_factor; /* fraction of input Nyquist */
    int m = num_taps - 1;
    double sum = 0.0;

    for (int n = 0; n < num_taps; n++) {
        double x = n - m / 2.0;
        double sinc = (x == 0.0) ? cutoff : sin(M_PI * cutoff * x) / (M_PI * x);
        /* Blackman window: better stopband rejection than Hann (~-58dB first
         * sidelobe vs ~-31dB), which matters a lot here -- real DSD content
         * has strong noise-shaped energy well above the audio band that a
         * weaker window would let through as audible hiss. */
        double window = 0.42 - 0.5 * cos(2.0 * M_PI * n / m) + 0.08 * cos(4.0 * M_PI * n / m);
        double tap = sinc * window;
        design->taps[n] = (float) tap;
        sum += tap;
    }

    /* Normalize so DC gain is exactly 1.0 */
    for (int n = 0; n < num_taps; n++) {
        design->taps[n] = (float) (design->taps[n] / sum);
    }
}

void dsd_channel_reset(dsd_channel_state_t * ch) {
    memset(ch->history, 0, sizeof(ch->history));
    ch->history_pos = 0;
    ch->bit_counter = 0;
}

bool dsd_channel_feed_bit(dsd_channel_state_t * ch, const dsd_filter_design_t * design, float bit_value, float * out_sample) {
    ch->history[ch->history_pos] = bit_value;
    ch->history_pos = (ch->history_pos + 1) % design->num_taps;

    ch->bit_counter++;
    if (ch->bit_counter < design->decimation_factor) return false;
    ch->bit_counter = 0;

    /* history_pos now points at the oldest sample (about to be overwritten
     * next); taps[0] pairs with the oldest sample, taps[num_taps-1] with
     * the newest, matching a standard FIR convolution. */
    float acc = 0.0f;
    int idx = ch->history_pos;
    for (int k = 0; k < design->num_taps; k++) {
        acc += design->taps[k] * ch->history[idx];
        idx = (idx + 1) % design->num_taps;
    }

    *out_sample = acc;
    return true;
}
