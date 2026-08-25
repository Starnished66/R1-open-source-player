#ifndef DSD_DECODER_H
#define DSD_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* DSF (Sony DSD Stream File) and DFF (Philips DSDIFF) container reader,
 * converting the raw 1-bit DSD bitstream to PCM via dsd_filter.h. Rather
 * than downsampling all the way to 44.1kHz (which would need a very long,
 * expensive single-stage filter), this decimates by 8x (DSD64) or 16x
 * (DSD128) to land on a fixed 352.8kHz PCM output -- comfortably within
 * the R1's CS43131 DAC's supported PCM range (up to 384kHz), matching how
 * most native-DSD-capable hardware handles this. DSD256 and above are not
 * supported. Output sample rate is therefore NOT the same as the file's
 * nominal DSD rate -- callers must use dsd_get_pcm_sample_rate(). */

typedef struct dsd_decoder dsd_decoder_t;

dsd_decoder_t * dsd_open_file(const char * path);

unsigned int dsd_get_channels(const dsd_decoder_t * dec);
unsigned int dsd_get_source_sample_rate(const dsd_decoder_t * dec);
unsigned int dsd_get_pcm_sample_rate(const dsd_decoder_t * dec);
uint64_t dsd_get_total_pcm_frame_count(const dsd_decoder_t * dec);

uint64_t dsd_read_pcm_frames_s16(dsd_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool dsd_seek_to_pcm_frame(dsd_decoder_t * dec, uint64_t frame_index);

void dsd_close(dsd_decoder_t * dec);

#endif /* DSD_DECODER_H */
