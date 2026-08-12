#ifndef DSD_FILTER_H
#define DSD_FILTER_H

#include <stdbool.h>

#define DSD_FILTER_MAX_TAPS 256

typedef struct {
    float taps[DSD_FILTER_MAX_TAPS];
    int num_taps;
    int decimation_factor;
} dsd_filter_design_t;

/* Designs a windowed-sinc (Hann) lowpass FIR targeting the new Nyquist that
 * results from decimating by decimation_factor -- i.e. cutoff at roughly
 * 0.9 / decimation_factor of the input rate's Nyquist, leaving a bit of
 * transition-band headroom so the stopband is fully attenuated by the time
 * aliasing would occur. num_taps must be <= DSD_FILTER_MAX_TAPS. */
void dsd_filter_design(dsd_filter_design_t * design, int num_taps, int decimation_factor);

/* Streaming per-channel decimator state. One of these per audio channel. */
typedef struct {
    float history[DSD_FILTER_MAX_TAPS];
    int history_pos; /* index of the oldest sample (next to be overwritten) */
    int bit_counter;
} dsd_channel_state_t;

void dsd_channel_reset(dsd_channel_state_t * ch);

/* Feeds one DSD bit (already converted to +1.0f/-1.0f bipolar) into the
 * channel's FIR history. Once every decimation_factor bits, computes and
 * writes a new decimated sample to *out_sample and returns true; otherwise
 * returns false (no new sample yet, keep feeding bits). */
bool dsd_channel_feed_bit(dsd_channel_state_t * ch, const dsd_filter_design_t * design, float bit_value, float * out_sample);

#endif /* DSD_FILTER_H */
