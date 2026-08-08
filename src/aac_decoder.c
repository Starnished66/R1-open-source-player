#include "aac_decoder.h"
#include "mp4_demux.h"
#include "neaacdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADTS_MAX_FRAME_BYTES 8192 /* frame_length is a 13-bit field, max 8191 */
#define AAC_MAX_CHANNELS 8
#define AAC_MAX_FRAME_SAMPLES (2048 * AAC_MAX_CHANNELS) /* generous: HE-AAC frameSize up to 2048/ch */

typedef enum {
    AAC_SOURCE_ADTS,
    AAC_SOURCE_MP4
} aac_source_type_t;

struct aac_decoder {
    aac_source_type_t source_type;

    /* AAC_SOURCE_ADTS */
    FILE * f;
    uint64_t * frame_offsets;
    uint64_t frame_count;

    /* AAC_SOURCE_MP4 */
    mp4_demux_t * demux;

    uint64_t current_frame_index;

    NeAACDecHandle handle;
    unsigned int channels;
    unsigned int sample_rate;
    unsigned int frame_size; /* PCM frames per access unit */
    uint64_t total_pcm_frames;

    int16_t carry_buffer[AAC_MAX_FRAME_SAMPLES];
    uint64_t carry_frames;
    uint64_t carry_read_pos;

    uint64_t current_pcm_frame;
};

static bool parse_adts_header(const uint8_t * h, unsigned int * out_frame_length) {
    if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) return false; /* 12-bit syncword 0xFFF */
    unsigned int frame_length = ((unsigned int) (h[3] & 0x03) << 11) | ((unsigned int) h[4] << 3) | ((h[5] >> 5) & 0x07);
    if (frame_length < 7) return false;
    *out_frame_length = frame_length;
    return true;
}

/* Scans the whole file up front, recording each ADTS frame's byte offset.
 * Cheap (header parsing only) and gives O(1) seek lookups and an exact
 * frame count for duration. */
static bool prescan_adts(FILE * f, uint64_t ** out_offsets, uint64_t * out_count) {
    fseek(f, 0, SEEK_SET);

    uint64_t capacity = 1024;
    uint64_t count = 0;
    uint64_t * offsets = malloc(sizeof(uint64_t) * capacity);
    if (!offsets) {
        *out_offsets = NULL;
        *out_count = 0;
        return false;
    }

    while (1) {
        long pos = ftell(f);
        uint8_t header[7];
        if (fread(header, 1, sizeof(header), f) != sizeof(header)) break;

        unsigned int frame_length;
        if (!parse_adts_header(header, &frame_length)) break;

        if (count == capacity) {
            /* Real-device crash report: long (30+ minute) podcasts crashed
             * outright rather than just failing to load -- this loop's
             * per-ADTS-frame offset table (one uint64_t per frame, tens of
             * thousands of frames for a long episode) used to realloc()
             * without checking the result. On failure realloc() returns
             * NULL while leaving the original block untouched, but
             * unconditionally assigning that NULL back over `offsets` lost
             * the only pointer to it (a leak) and the very next
             * `offsets[count++] = ...` wrote through a NULL pointer --
             * guaranteed to segfault, and more likely to actually happen
             * the longer (so the bigger this table gets) the file is,
             * matching the exact "large podcasts" report. Now grown into a
             * temporary so a failed realloc doesn't destroy the still-valid
             * original block, and a failure here just stops the scan with
             * whatever frames were already found -- same fallback shape as
             * hitting the end of the file or a corrupt frame, not a crash. */
            uint64_t new_capacity = capacity * 2;
            uint64_t * grown = realloc(offsets, sizeof(uint64_t) * new_capacity);
            if (!grown) break;
            offsets = grown;
            capacity = new_capacity;
        }
        offsets[count++] = (uint64_t) pos;

        long next = pos + (long) frame_length;
        if (fseek(f, next, SEEK_SET) != 0) break;
    }

    *out_offsets = offsets;
    *out_count = count;
    return count > 0;
}

/* Reads one compressed access unit (frame_index'th) into buf, regardless of
 * source framing. Returns its size, or 0 on failure/out of range. */
static unsigned int read_compressed_frame(aac_decoder_t * dec, uint64_t frame_index, uint8_t * buf, unsigned int buf_size) {
    if (dec->source_type == AAC_SOURCE_ADTS) {
        if (frame_index >= dec->frame_count) return 0;
        fseek(dec->f, (long) dec->frame_offsets[frame_index], SEEK_SET);

        if (fread(buf, 1, 7, dec->f) != 7) return 0;
        unsigned int frame_length;
        if (!parse_adts_header(buf, &frame_length)) return 0;
        if (frame_length > buf_size || frame_length <= 7) return 0;
        if (fread(buf + 7, 1, frame_length - 7, dec->f) != frame_length - 7) return 0;
        return frame_length;
    }

    /* AAC_SOURCE_MP4: no ADTS header, the container already delimits frames */
    uint32_t size;
    if (!mp4_demux_read_sample(dec->demux, (uint32_t) frame_index, buf, buf_size, &size)) return 0;
    return size;
}

static bool decode_next_frame(aac_decoder_t * dec) {
    uint8_t frame_buf[ADTS_MAX_FRAME_BYTES];
    unsigned int frame_length = read_compressed_frame(dec, dec->current_frame_index, frame_buf, sizeof(frame_buf));
    if (frame_length == 0) return false;

    NeAACDecFrameInfo info;
    memset(&info, 0, sizeof(info));
    void * samples = NeAACDecDecode(dec->handle, &info, frame_buf, frame_length);
    if (info.error != 0 || !samples || info.channels == 0) return false;

    /* info.samples is the total interleaved sample count across all
     * channels (i.e. PCM frames * channels), matching FAAD2's documented
     * usage convention. */
    uint64_t total_samples = info.samples;
    if (total_samples > AAC_MAX_FRAME_SAMPLES) total_samples = AAC_MAX_FRAME_SAMPLES;
    memcpy(dec->carry_buffer, samples, (size_t) total_samples * sizeof(int16_t));

    dec->carry_frames = total_samples / info.channels;
    dec->carry_read_pos = 0;

    dec->current_frame_index++;
    return true;
}

static void configure_decoder(NeAACDecHandle handle) {
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(handle);
    config->outputFormat = FAAD_FMT_16BIT;
    NeAACDecSetConfiguration(handle, config);
}

/* Shared setup once the source-specific fields (frame_offsets/frame_count,
 * or demux) and the first init call are done: prime the decoder past AAC's
 * inherent encoder/decoder delay (the first frame, sometimes more, routinely
 * decodes to zero samples -- using that to establish frame_size would make
 * total_pcm_frames come out as 0, which corrupts caller buffer sizing) and
 * compute the duration estimate. */
static bool finish_open(aac_decoder_t * dec, uint64_t total_frame_count) {
    while (dec->carry_frames == 0) {
        if (!decode_next_frame(dec)) return false;
    }

    dec->frame_size = (unsigned int) dec->carry_frames;
    uint64_t priming_frames_consumed = dec->current_frame_index - 1;
    dec->total_pcm_frames = (total_frame_count - priming_frames_consumed) * dec->frame_size;
    return true;
}

aac_decoder_t * aac_open_file(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    uint64_t * offsets;
    uint64_t count;
    if (!prescan_adts(f, &offsets, &count)) {
        free(offsets);
        fclose(f);
        return NULL;
    }

    NeAACDecHandle handle = NeAACDecOpen();
    if (!handle) {
        free(offsets);
        fclose(f);
        return NULL;
    }
    configure_decoder(handle);

    uint8_t first_frame[ADTS_MAX_FRAME_BYTES];
    fseek(f, (long) offsets[0], SEEK_SET);
    if (fread(first_frame, 1, 7, f) != 7) {
        NeAACDecClose(handle);
        free(offsets);
        fclose(f);
        return NULL;
    }
    unsigned int first_frame_length;
    if (!parse_adts_header(first_frame, &first_frame_length) ||
        fread(first_frame + 7, 1, first_frame_length - 7, f) != first_frame_length - 7) {
        NeAACDecClose(handle);
        free(offsets);
        fclose(f);
        return NULL;
    }

    unsigned long init_sample_rate;
    unsigned char init_channels;
    long init_consumed = NeAACDecInit(handle, first_frame, first_frame_length, &init_sample_rate, &init_channels);
    if (init_consumed < 0 || init_sample_rate == 0 || init_channels == 0) {
        NeAACDecClose(handle);
        free(offsets);
        fclose(f);
        return NULL;
    }

    aac_decoder_t * dec = calloc(1, sizeof(*dec));
    dec->source_type = AAC_SOURCE_ADTS;
    dec->f = f;
    dec->frame_offsets = offsets;
    dec->frame_count = count;
    dec->handle = handle;
    dec->sample_rate = (unsigned int) init_sample_rate;
    dec->channels = (unsigned int) init_channels;

    if (!finish_open(dec, count)) {
        aac_close(dec);
        return NULL;
    }
    return dec;
}

aac_decoder_t * aac_open_file_mp4(const char * path) {
    mp4_demux_t * demux = mp4_demux_open(path);
    if (!demux) return NULL;

    char fourcc[5];
    mp4_demux_get_codec_fourcc(demux, fourcc);
    if (strcmp(fourcc, "mp4a") != 0) {
        mp4_demux_close(demux);
        return NULL;
    }

    uint32_t config_size;
    const uint8_t * config = mp4_demux_get_codec_config(demux, &config_size);
    if (!config || config_size == 0) {
        mp4_demux_close(demux);
        return NULL;
    }

    NeAACDecHandle handle = NeAACDecOpen();
    if (!handle) {
        mp4_demux_close(demux);
        return NULL;
    }
    configure_decoder(handle);

    unsigned long init_sample_rate;
    unsigned char init_channels;
    /* NeAACDecInit2 takes an explicit AudioSpecificConfig -- unlike
     * NeAACDecInit, which expects to find its own ADTS/ADIF sync headers to
     * auto-detect config from, MP4-contained frames are raw/bare with no
     * such headers, framed entirely by the container instead. */
    if (NeAACDecInit2(handle, (unsigned char *) config, config_size, &init_sample_rate, &init_channels) != 0 ||
        init_sample_rate == 0 || init_channels == 0) {
        NeAACDecClose(handle);
        mp4_demux_close(demux);
        return NULL;
    }

    aac_decoder_t * dec = calloc(1, sizeof(*dec));
    dec->source_type = AAC_SOURCE_MP4;
    dec->demux = demux;
    dec->handle = handle;
    dec->sample_rate = (unsigned int) init_sample_rate;
    dec->channels = (unsigned int) init_channels;

    if (!finish_open(dec, mp4_demux_get_sample_count(demux))) {
        aac_close(dec);
        return NULL;
    }
    return dec;
}

unsigned int aac_get_channels(const aac_decoder_t * dec) {
    return dec->channels;
}

unsigned int aac_get_sample_rate(const aac_decoder_t * dec) {
    return dec->sample_rate;
}

uint64_t aac_get_total_pcm_frame_count(const aac_decoder_t * dec) {
    return dec->total_pcm_frames;
}

uint64_t aac_read_pcm_frames_s16(aac_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    uint64_t frames_written = 0;

    while (frames_written < frames_to_read) {
        if (dec->carry_read_pos >= dec->carry_frames) {
            if (!decode_next_frame(dec)) break;
        }

        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        uint64_t to_copy = frames_to_read - frames_written;
        if (to_copy > available) to_copy = available;

        memcpy(buffer_out + frames_written * dec->channels,
               dec->carry_buffer + dec->carry_read_pos * dec->channels,
               (size_t) to_copy * dec->channels * sizeof(int16_t));

        dec->carry_read_pos += to_copy;
        frames_written += to_copy;
        dec->current_pcm_frame += to_copy;
    }

    return frames_written;
}

bool aac_seek_to_pcm_frame(aac_decoder_t * dec, uint64_t frame_index) {
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;

    uint64_t total_frame_count = (dec->source_type == AAC_SOURCE_ADTS) ? dec->frame_count : mp4_demux_get_sample_count(dec->demux);

    uint64_t target_frame = dec->frame_size > 0 ? frame_index / dec->frame_size : 0;
    if (target_frame >= total_frame_count) target_frame = total_frame_count > 0 ? total_frame_count - 1 : 0;

    /* AAC carries inter-frame state (SBR, etc), so the cleanest correct
     * approach is to reset the decoder entirely and resume from the target
     * frame boundary -- meaning seeks land on a frame-size boundary, not
     * the exact requested sample. Accepted imprecision, same as most
     * frame-based codec seeking. */
    NeAACDecClose(dec->handle);
    dec->handle = NeAACDecOpen();
    if (!dec->handle) return false;
    configure_decoder(dec->handle);

    uint8_t frame_buf[ADTS_MAX_FRAME_BYTES];
    unsigned int frame_length = read_compressed_frame(dec, target_frame, frame_buf, sizeof(frame_buf));
    if (frame_length == 0) return false;

    unsigned long sample_rate;
    unsigned char channels;
    if (dec->source_type == AAC_SOURCE_ADTS) {
        if (NeAACDecInit(dec->handle, frame_buf, frame_length, &sample_rate, &channels) < 0) return false;
    } else {
        uint32_t config_size;
        const uint8_t * config = mp4_demux_get_codec_config(dec->demux, &config_size);
        if (NeAACDecInit2(dec->handle, (unsigned char *) config, config_size, &sample_rate, &channels) != 0) return false;
    }

    dec->current_frame_index = target_frame;
    dec->carry_frames = 0;
    dec->carry_read_pos = 0;

    if (!decode_next_frame(dec)) return false;

    dec->current_pcm_frame = target_frame * dec->frame_size;
    return true;
}

void aac_close(aac_decoder_t * dec) {
    if (!dec) return;
    if (dec->handle) NeAACDecClose(dec->handle);
    if (dec->f) fclose(dec->f);
    if (dec->demux) mp4_demux_close(dec->demux);
    free(dec->frame_offsets);
    free(dec);
}
