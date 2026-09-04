#include "alac_decoder.h"

extern "C" {
#include "mp4_demux.h"
}

#include "ALACDecoder.h"
#include "ALACBitUtilities.h"

#include <cstdlib>
#include <cstring>

struct alac_decoder {
    mp4_demux_t * demux;
    ALACDecoder * decoder;

    unsigned int channels;
    unsigned int sample_rate;
    unsigned int bit_depth;
    uint32_t frame_size; /* PCM frames per demuxed sample, e.g. 4096 */
    uint64_t total_pcm_frames;

    uint32_t current_sample_index;
    unsigned int consecutive_errors;

    uint8_t * compressed_buf;
    uint32_t compressed_buf_capacity;

    uint8_t * raw_output_buf; /* native bit-depth output from ALACDecoder::Decode() */

    int16_t * carry_buffer; /* converted to S16, ready to deliver */
    int32_t * carry_buffer_s32; /* converted to S32/S24_LE layout, ready to deliver */
    uint32_t carry_frames;
    uint32_t carry_read_pos;

    uint64_t current_pcm_frame;
};

#define ALAC_MAX_CONSECUTIVE_ERRORS 5

static bool decode_next_sample_ex(alac_decoder_t * dec, decoder_read_status_t * out_status) {
    if (dec->current_sample_index >= mp4_demux_get_sample_count(dec->demux)) {
        if (out_status) *out_status = DECODER_READ_EOF;
        return false;
    }

    uint32_t compressed_size;
    if (!mp4_demux_read_sample(dec->demux, dec->current_sample_index, dec->compressed_buf,
                                dec->compressed_buf_capacity, &compressed_size)) {
        dec->consecutive_errors++;
        dec->current_sample_index++;
        if (dec->consecutive_errors >= ALAC_MAX_CONSECUTIVE_ERRORS) {
            if (out_status) *out_status = DECODER_READ_FATAL_ERROR;
            return false;
        }
        if (out_status) *out_status = DECODER_READ_RECOVERABLE_ERROR;
        return false;
    }

    BitBuffer bits;
    BitBufferInit(&bits, dec->compressed_buf, compressed_size);

    uint32_t out_frames = 0;
    int32_t status = dec->decoder->Decode(&bits, dec->raw_output_buf, dec->frame_size, dec->channels, &out_frames);
    if (status != 0) {
        dec->consecutive_errors++;
        dec->current_sample_index++;
        if (dec->consecutive_errors >= ALAC_MAX_CONSECUTIVE_ERRORS) {
            if (out_status) *out_status = DECODER_READ_FATAL_ERROR;
            return false;
        }
        if (out_status) *out_status = DECODER_READ_RECOVERABLE_ERROR;
        return false;
    }

    if (dec->bit_depth == 16) {
        memcpy(dec->carry_buffer, dec->raw_output_buf, (size_t) out_frames * dec->channels * sizeof(int16_t));
        if (dec->carry_buffer_s32) {
            const int16_t * src16 = (const int16_t *) dec->raw_output_buf;
            size_t sample_count = (size_t) out_frames * dec->channels;
            for (size_t i = 0; i < sample_count; i++) {
                dec->carry_buffer_s32[i] = (int32_t) src16[i];
            }
        }
    } else { /* 24-bit: packed 3-byte little-endian samples per copyPredictorTo24/unmix24 */
        const uint8_t * src = dec->raw_output_buf;
        size_t sample_count = (size_t) out_frames * dec->channels;
        for (size_t i = 0; i < sample_count; i++) {
            int32_t v = src[i * 3] | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
            if (v & 0x800000) v |= (int32_t) 0xFF000000; /* sign-extend */
            dec->carry_buffer[i] = (int16_t) (v >> 8);
            if (dec->carry_buffer_s32) {
                /* ALAC decoded sample v is already a right-justified, sign-extended
                 * true-magnitude int32 value -- do NOT shift, it is already in the
                 * low 24 bits matching ALSA S24_LE layout. */
                dec->carry_buffer_s32[i] = v;
            }
        }
    }

    dec->consecutive_errors = 0;
    dec->carry_frames = out_frames;
    dec->carry_read_pos = 0;
    dec->current_sample_index++;
    if (out_status) *out_status = DECODER_READ_OK;
    return true;
}

static bool decode_next_sample(alac_decoder_t * dec) {
    return decode_next_sample_ex(dec, NULL);
}

extern "C" alac_decoder_t * alac_open_file(const char * path) {
    mp4_demux_t * demux = mp4_demux_open(path);
    if (!demux) return NULL;

    char fourcc[5];
    mp4_demux_get_codec_fourcc(demux, fourcc);
    if (strcmp(fourcc, "alac") != 0) {
        mp4_demux_close(demux);
        return NULL;
    }

    uint32_t config_size;
    const uint8_t * config = mp4_demux_get_codec_config(demux, &config_size);
    if (!config || config_size == 0) {
        mp4_demux_close(demux);
        return NULL;
    }

    ALACDecoder * decoder = new ALACDecoder();
    /* Init() only reads from this buffer (parses header fields), never writes it,
     * despite the non-const signature. */
    if (decoder->Init((void *) config, config_size) != 0) {
        delete decoder;
        mp4_demux_close(demux);
        return NULL;
    }

    if (decoder->mConfig.bitDepth != 16 && decoder->mConfig.bitDepth != 24) {
        delete decoder;
        mp4_demux_close(demux);
        return NULL; /* 20/32-bit ALAC not handled -- rare in practice */
    }

    alac_decoder_t * dec = (alac_decoder_t *) calloc(1, sizeof(alac_decoder_t));
    dec->demux = demux;
    dec->decoder = decoder;
    dec->channels = decoder->mConfig.numChannels;
    dec->sample_rate = decoder->mConfig.sampleRate;
    dec->bit_depth = decoder->mConfig.bitDepth;
    dec->frame_size = mp4_demux_get_frames_per_sample(demux);
    if (dec->frame_size == 0) dec->frame_size = decoder->mConfig.frameLength;

    /* Exact total (not sample_count * frame_size) -- the last sample
     * typically represents fewer frames than a full access unit, and the
     * container's own stts table already records the real per-sample
     * durations. */
    dec->total_pcm_frames = mp4_demux_get_total_pcm_frame_count(demux);

    /* Generous: worst case is uncompressed-ish escape frames, channels *
     * bytes-per-sample-at-full-depth * frame_size, plus header slack. */
    dec->compressed_buf_capacity = dec->frame_size * dec->channels * 4 + 4096;
    dec->compressed_buf = (uint8_t *) malloc(dec->compressed_buf_capacity);

    size_t bytes_per_sample = (dec->bit_depth == 16) ? sizeof(int16_t) : 3;
    dec->raw_output_buf = (uint8_t *) malloc((size_t) dec->frame_size * dec->channels * bytes_per_sample);
    dec->carry_buffer = (int16_t *) malloc((size_t) dec->frame_size * dec->channels * sizeof(int16_t));

    if (!decode_next_sample(dec)) {
        alac_close(dec);
        return NULL;
    }

    return dec;
}

unsigned int alac_get_channels(const alac_decoder_t * dec) {
    return dec->channels;
}

unsigned int alac_get_sample_rate(const alac_decoder_t * dec) {
    return dec->sample_rate;
}

unsigned int alac_get_bit_depth(const alac_decoder_t * dec) {
    return dec->bit_depth;
}

uint64_t alac_get_total_pcm_frame_count(const alac_decoder_t * dec) {
    return dec->total_pcm_frames;
}

decoder_read_result_t alac_read_pcm_frames_s16(alac_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !buffer_out) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    while (res.frames < frames_to_read) {
        if (dec->carry_read_pos >= dec->carry_frames) {
            decoder_read_status_t status = DECODER_READ_OK;
            if (!decode_next_sample_ex(dec, &status)) {
                if (res.frames > 0) {
                    res.status = DECODER_READ_OK;
                } else {
                    res.status = status;
                }
                break;
            }
        }

        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        uint64_t to_copy = frames_to_read - res.frames;
        if (to_copy > available) to_copy = available;

        memcpy(buffer_out + res.frames * dec->channels,
               dec->carry_buffer + dec->carry_read_pos * dec->channels,
               (size_t) to_copy * dec->channels * sizeof(int16_t));

        dec->carry_read_pos += (uint32_t) to_copy;
        res.frames += to_copy;
        dec->current_pcm_frame += to_copy;
    }

    return res;
}

decoder_read_result_t alac_read_pcm_frames_s32(alac_decoder_t * dec, uint64_t frames_to_read, int32_t * buffer_out) {
    if (dec && !dec->carry_buffer_s32) {
        dec->carry_buffer_s32 = (int32_t *) malloc((size_t) dec->frame_size * dec->channels * sizeof(int32_t));
        if (dec->carry_buffer_s32 && dec->carry_frames > 0) {
            if (dec->bit_depth == 16) {
                const int16_t * src16 = (const int16_t *) dec->raw_output_buf;
                size_t sample_count = (size_t) dec->carry_frames * dec->channels;
                for (size_t i = 0; i < sample_count; i++) {
                    dec->carry_buffer_s32[i] = (int32_t) src16[i];
                }
            } else {
                const uint8_t * src = dec->raw_output_buf;
                size_t sample_count = (size_t) dec->carry_frames * dec->channels;
                for (size_t i = 0; i < sample_count; i++) {
                    int32_t v = src[i * 3] | (src[i * 3 + 1] << 8) | (src[i * 3 + 2] << 16);
                    if (v & 0x800000) v |= (int32_t) 0xFF000000;
                    dec->carry_buffer_s32[i] = v;
                }
            }
        }
    }

    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !buffer_out || !dec->carry_buffer_s32) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    while (res.frames < frames_to_read) {
        if (dec->carry_read_pos >= dec->carry_frames) {
            decoder_read_status_t status = DECODER_READ_OK;
            if (!decode_next_sample_ex(dec, &status)) {
                if (res.frames > 0) {
                    res.status = DECODER_READ_OK;
                } else {
                    res.status = status;
                }
                break;
            }
        }

        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        uint64_t to_copy = frames_to_read - res.frames;
        if (to_copy > available) to_copy = available;

        memcpy(buffer_out + res.frames * dec->channels,
               dec->carry_buffer_s32 + dec->carry_read_pos * dec->channels,
               (size_t) to_copy * dec->channels * sizeof(int32_t));

        dec->carry_read_pos += (uint32_t) to_copy;
        res.frames += to_copy;
        dec->current_pcm_frame += to_copy;
    }

    return res;
}

bool alac_seek_to_pcm_frame(alac_decoder_t * dec, uint64_t frame_index) {
    if (!dec) return false;
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;

    uint32_t target_sample = (uint32_t) (frame_index / dec->frame_size);
    uint32_t sample_count = mp4_demux_get_sample_count(dec->demux);
    if (target_sample >= sample_count) target_sample = sample_count > 0 ? sample_count - 1 : 0;

    dec->current_sample_index = target_sample;
    dec->carry_frames = 0;
    dec->carry_read_pos = 0;
    dec->consecutive_errors = 0;

    if (!decode_next_sample(dec)) return false;

    dec->current_pcm_frame = (uint64_t) target_sample * dec->frame_size;
    return true;
}

void alac_close(alac_decoder_t * dec) {
    if (!dec) return;
    delete dec->decoder;
    mp4_demux_close(dec->demux);
    free(dec->compressed_buf);
    free(dec->raw_output_buf);
    free(dec->carry_buffer);
    free(dec->carry_buffer_s32);
    free(dec);
}
