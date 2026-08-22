#include "vorbis_decoder.h"

#include "stb_vorbis.h"

#include <stdlib.h>

#define VORBIS_MAX_OUTPUT_CHANNELS 8

struct vorbis_decoder_wrap {
    stb_vorbis * f;
    unsigned int channels;
    unsigned int sample_rate;
    uint64_t total_pcm_frames;
};

vorbis_decoder_wrap_t * vorbis_open_file(const char * path) {
    int error = 0;
    stb_vorbis * f = stb_vorbis_open_filename(path, &error, NULL);
    if (!f) return NULL;

    vorbis_decoder_wrap_t * dec = calloc(1, sizeof(*dec));
    if (!dec) {
        stb_vorbis_close(f);
        return NULL;
    }

    stb_vorbis_info info = stb_vorbis_get_info(f);
    if (info.channels <= 0 || info.channels > VORBIS_MAX_OUTPUT_CHANNELS || info.sample_rate == 0) {
        stb_vorbis_close(f);
        free(dec);
        return NULL;
    }
    dec->f = f;
    dec->channels = (unsigned int) info.channels;
    dec->sample_rate = info.sample_rate;
    dec->total_pcm_frames = stb_vorbis_stream_length_in_samples(f);
    return dec;
}

unsigned int vorbis_get_channels(const vorbis_decoder_wrap_t * dec) {
    return dec->channels;
}

unsigned int vorbis_get_sample_rate(const vorbis_decoder_wrap_t * dec) {
    return dec->sample_rate;
}

uint64_t vorbis_get_total_pcm_frame_count(const vorbis_decoder_wrap_t * dec) {
    return dec->total_pcm_frames;
}

uint64_t vorbis_read_pcm_frames_s16(vorbis_decoder_wrap_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    uint64_t frames_written = 0;
    while (frames_written < frames_to_read) {
        uint64_t frames_left = frames_to_read - frames_written;
        /* stb_vorbis_get_samples_short_interleaved()'s own num_shorts param
         * is int -- cap each call's request so frames_left * channels can
         * never overflow it, same reasoning as every other bounded-chunk
         * read loop in this codebase (e.g. audio.c's own NORMAL_CHUNK_FRAMES
         * sizing). INT_MAX / channels is always comfortably larger than any
         * real chunk size this app ever requests in one call anyway. */
        uint64_t chunk_frames = frames_left;
        if (chunk_frames > (uint64_t) (2147483647 / (dec->channels > 0 ? dec->channels : 1))) {
            chunk_frames = (uint64_t) (2147483647 / (dec->channels > 0 ? dec->channels : 1));
        }
        int got = stb_vorbis_get_samples_short_interleaved(
            dec->f, (int) dec->channels, buffer_out + frames_written * dec->channels,
            (int) (chunk_frames * dec->channels));
        if (got <= 0) break; /* real EOF, or a corrupt stream stb_vorbis gave up resyncing -- either way, nothing more to give */
        frames_written += (uint64_t) got;
    }
    return frames_written;
}

bool vorbis_seek_to_pcm_frame(vorbis_decoder_wrap_t * dec, uint64_t frame_index) {
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;
    /* stb_vorbis_seek() (not the frame-approximate seek_frame()) -- sample-
     * accurate for the get_samples_* family this wrapper's own read function
     * uses, matching every other decoder's seek contract in this app (used
     * for progress-bar scrubbing, where landing a whole Vorbis packet off
     * would be an audible/visible jump). frame_index is already bounded to
     * unsigned int range by the total_pcm_frames clamp above -- stb_vorbis's
     * own sample_number param is unsigned int, narrower than uint64_t. */
    return stb_vorbis_seek(dec->f, (unsigned int) frame_index) != 0;
}

void vorbis_close(vorbis_decoder_wrap_t * dec) {
    if (!dec) return;
    if (dec->f) stb_vorbis_close(dec->f);
    free(dec);
}
