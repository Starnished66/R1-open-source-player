#ifndef VORBIS_DECODER_H
#define VORBIS_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* Ogg Vorbis (.ogg) decoding via stb_vorbis (vendored, stb_vorbis/stb_vorbis.c
 * -- public domain, github.com/nothings/stb) -- unlike Opus above, this one
 * library handles the Ogg container framing AND the Vorbis codec decode
 * itself, so there's no separate ogg_demux.h split here. Named vorbis_
 * decoder_wrap_t (not vorbis_decoder_t) for the same reason opus_decoder_
 * wrap_t is -- avoids clashing with stb_vorbis's own `stb_vorbis` type name. */

#include "decoder_result.h"

typedef struct vorbis_decoder_wrap vorbis_decoder_wrap_t;

vorbis_decoder_wrap_t * vorbis_open_file(const char * path);

unsigned int vorbis_get_channels(const vorbis_decoder_wrap_t * dec);
unsigned int vorbis_get_sample_rate(const vorbis_decoder_wrap_t * dec);
uint64_t     vorbis_get_total_pcm_frame_count(const vorbis_decoder_wrap_t * dec);

decoder_read_result_t vorbis_read_pcm_frames_s16(vorbis_decoder_wrap_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
bool     vorbis_seek_to_pcm_frame(vorbis_decoder_wrap_t * dec, uint64_t frame_index);

void vorbis_close(vorbis_decoder_wrap_t * dec);

#endif /* VORBIS_DECODER_H */
