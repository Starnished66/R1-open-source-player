#ifndef AIFF_DECODER_H
#define AIFF_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* Minimal AIFF/AIFF-C reader (big-endian PCM container). No dr_libs
 * equivalent exists for this format, so it's hand-written here, matching
 * the same open/read/seek/close shape as dr_flac/dr_mp3/dr_wav so it slots
 * into audio.c's decoder dispatch the same way. Supports 8/16/24-bit PCM
 * ('AIFF' and uncompressed 'AIFC'); compressed AIFC variants are not
 * supported. */

#include "decoder_result.h"

typedef struct aiff_decoder aiff_decoder_t;

aiff_decoder_t * aiff_open_file(const char * path);

unsigned int aiff_get_channels(const aiff_decoder_t * dec);
unsigned int aiff_get_sample_rate(const aiff_decoder_t * dec);
unsigned int aiff_get_bits_per_sample(const aiff_decoder_t * dec);
uint64_t aiff_get_total_pcm_frame_count(const aiff_decoder_t * dec);

decoder_read_result_t aiff_read_pcm_frames_s16(aiff_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool aiff_seek_to_pcm_frame(aiff_decoder_t * dec, uint64_t frame_index);

void aiff_close(aiff_decoder_t * dec);

#endif /* AIFF_DECODER_H */
