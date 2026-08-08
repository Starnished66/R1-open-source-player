#ifndef ALAC_DECODER_H
#define ALAC_DECODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ALAC (Apple Lossless) decoding via Apple's own reference decoder
 * (github.com/mikebrady/alac, a maintained mirror -- Apache 2.0, so unlike
 * FAAD2 this doesn't affect the project's licensing), reading samples out
 * of an MP4/M4A container via mp4_demux.h. Only 16-bit and 24-bit source
 * material is handled (16-bit is the vast majority of real ALAC content);
 * other bit depths are rejected rather than silently mishandled. */

typedef struct alac_decoder alac_decoder_t;

alac_decoder_t * alac_open_file(const char * path);

unsigned int alac_get_channels(const alac_decoder_t * dec);
unsigned int alac_get_sample_rate(const alac_decoder_t * dec);
uint64_t alac_get_total_pcm_frame_count(const alac_decoder_t * dec);

uint64_t alac_read_pcm_frames_s16(alac_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool alac_seek_to_pcm_frame(alac_decoder_t * dec, uint64_t frame_index);

void alac_close(alac_decoder_t * dec);

#ifdef __cplusplus
}
#endif

#endif /* ALAC_DECODER_H */
