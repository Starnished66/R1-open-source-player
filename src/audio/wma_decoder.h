#ifndef WMA_DECODER_H
#define WMA_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* WMA (v1/v2 "WMA Standard", the lossy codecs -- not WMA Pro or WMA
 * Lossless) decoding, driven by src/asf_demux.c for container parsing. A
 * from-scratch reimplementation of the algorithm described by FFmpeg's
 * libavcodec/wma.c + wmadec.c (the codec itself was never officially
 * published by Microsoft; FFmpeg's decoder is the practical reference).
 * The Huffman/critical-band/LSP-codebook constants in wma_tables.h are
 * format-mandated numeric data (an encoder and decoder must agree on them
 * bit-for-bit), not creative expression, and were pulled out with a small
 * extraction script rather than hand-copied.
 *
 * Verified against a real ground-truth file: a 440Hz sine wave encoded to
 * WMAv2/128kbps with ffmpeg, decoded here and compared against ffmpeg's
 * own decode of the same file. */

typedef struct wma_decoder wma_decoder_t;

wma_decoder_t * wma_open_file(const char * path);

unsigned int wma_get_channels(const wma_decoder_t * dec);
unsigned int wma_get_sample_rate(const wma_decoder_t * dec);
uint64_t wma_get_total_pcm_frame_count(const wma_decoder_t * dec);

uint64_t wma_read_pcm_frames_s16(wma_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool wma_seek_to_pcm_frame(wma_decoder_t * dec, uint64_t frame_index);

void wma_close(wma_decoder_t * dec);

#endif /* WMA_DECODER_H */
