#ifndef APE_DECODER_H
#define APE_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* Monkey's Audio (.ape) decoding -- a from-scratch C port of the relevant
 * parts of FFmpeg's libavcodec/apedec.c (LGPL 2.1+, Copyright (c) 2007
 * Benjamin Zores, based on libdemac by Dave Chapman), scoped to fileversion
 * 3980-3990 (see ape_demux.h for why that's not a practical limitation).
 * Within that range this covers: the range-coder-based entropy decoders
 * (the "3900"/"3930"/"3990" variants), the 64-bit adaptive predictor (the
 * "3950" variant), and the multi-level adaptive FIR filter stage -- i.e.
 * every compression level (Fast through Insane) actually used by any
 * present-day Monkey's Audio encoder. The legacy 32-bit predictor, the
 * pre-range-coder bit-packed entropy decoders, and the old pre-3930 long
 * filters are all dropped as effectively unreachable for real files.
 *
 * IMPORTANT: unlike the other decoders in this codebase, this one has not
 * been verified against a real Monkey's Audio file -- there is no APE
 * encoder available anywhere in the development environment (FFmpeg only
 * decodes APE; there's no packaged encoder either), so this is a careful
 * line-by-line port rather than a tested implementation. Treat it as
 * unverified until checked against real files on real hardware. */

typedef struct ape_decoder ape_decoder_t;

ape_decoder_t * ape_open_file(const char * path);

unsigned int ape_get_channels(const ape_decoder_t * dec);
unsigned int ape_get_sample_rate(const ape_decoder_t * dec);
uint64_t ape_get_total_pcm_frame_count(const ape_decoder_t * dec);

uint64_t ape_read_pcm_frames_s16(ape_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool ape_seek_to_pcm_frame(ape_decoder_t * dec, uint64_t frame_index);

void ape_close(ape_decoder_t * dec);

#endif /* APE_DECODER_H */
