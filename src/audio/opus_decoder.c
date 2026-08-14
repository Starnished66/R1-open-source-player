#include "opus_decoder.h"
#include "ogg_demux.h"
#include "opus.h"

#include <stdlib.h>
#include <string.h>

/* opus_decode()'s frame_size parameter is capacity per channel, not total --
 * 5760 = 120ms @ 48kHz, the largest single Opus frame duration RFC 6716
 * defines, so this is always big enough regardless of what any given
 * packet actually contains (opus_decode() itself reports how many samples
 * it decoded). Sized for stereo since channel_mapping_family 0 (the only
 * one ogg_demux.h accepts) tops out at 2 channels. */
#define OPUS_MAX_FRAME_SAMPLES_PER_CHANNEL 5760
#define OPUS_MAX_CHANNELS 2
#define OPUS_MAX_FRAME_SAMPLES (OPUS_MAX_FRAME_SAMPLES_PER_CHANNEL * OPUS_MAX_CHANNELS)

/* One full Ogg page's max payload (255 segments * 255 bytes) -- generous
 * upper bound for a single Opus packet, matching ogg_demux.h's own
 * OGG_MAX_PAGE_SIZE-class sizing rather than guessing a tighter cap. */
#define OPUS_MAX_PACKET_BYTES (255 * 255)

struct opus_decoder_wrap {
    ogg_demux_t * demux;
    OpusDecoder * dec;
    unsigned int channels;
    uint16_t pre_skip;
    uint64_t total_pcm_frames;

    uint8_t * packet_buf; /* OPUS_MAX_PACKET_BYTES bytes */

    int16_t carry_buffer[OPUS_MAX_FRAME_SAMPLES];
    uint64_t carry_frames;
    uint64_t carry_read_pos;
};

static bool decode_next_packet(opus_decoder_wrap_t * dec) {
    uint32_t packet_len = ogg_demux_read_packet(dec->demux, dec->packet_buf, OPUS_MAX_PACKET_BYTES);
    if (packet_len == 0) return false;

    int frames = opus_decode(dec->dec, dec->packet_buf, (opus_int32) packet_len, dec->carry_buffer,
                              OPUS_MAX_FRAME_SAMPLES_PER_CHANNEL, 0);
    if (frames < 0) return false;

    dec->carry_frames = (uint64_t) frames;
    dec->carry_read_pos = 0;
    return true;
}

/* Advances the read position by exactly `count` decoded PCM frames,
 * decoding further packets as needed. Used both to trim the OpusHead
 * pre-skip at open and, after a seek, to discard the gap between the
 * granule position landed on and the exact requested sample -- see
 * ogg_demux_seek_near_granule()'s doc comment for why that gap exists. */
static bool discard_frames(opus_decoder_wrap_t * dec, uint64_t count) {
    while (count > 0) {
        if (dec->carry_read_pos >= dec->carry_frames) {
            if (!decode_next_packet(dec)) return false;
        }
        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        uint64_t skip = count < available ? count : available;
        dec->carry_read_pos += skip;
        count -= skip;
    }
    return true;
}

opus_decoder_wrap_t * opus_open_file(const char * path) {
    ogg_demux_t * demux = ogg_demux_open(path);
    if (!demux) return NULL;

    unsigned int channels = ogg_demux_get_opus_channels(demux);

    int err = 0;
    /* Always decode at 48000 Hz regardless of OpusHead's informational
     * input-sample-rate field -- Ogg Opus's decoded PCM is defined at
     * libopus's fixed internal rate, there's no "native rate" to preserve. */
    OpusDecoder * dec_handle = opus_decoder_create(48000, (int) channels, &err);
    if (err != OPUS_OK || !dec_handle) {
        if (dec_handle) opus_decoder_destroy(dec_handle);
        ogg_demux_close(demux);
        return NULL;
    }

    opus_decoder_wrap_t * dec = calloc(1, sizeof(*dec));
    if (!dec) {
        opus_decoder_destroy(dec_handle);
        ogg_demux_close(demux);
        return NULL;
    }
    dec->packet_buf = malloc(OPUS_MAX_PACKET_BYTES);
    if (!dec->packet_buf) {
        free(dec);
        opus_decoder_destroy(dec_handle);
        ogg_demux_close(demux);
        return NULL;
    }
    dec->demux = demux;
    dec->dec = dec_handle;
    dec->channels = channels;
    dec->pre_skip = ogg_demux_get_opus_pre_skip(demux);

    uint64_t total_granule = ogg_demux_get_total_granule(demux);
    dec->total_pcm_frames = total_granule > dec->pre_skip ? total_granule - dec->pre_skip : 0;

    if (!discard_frames(dec, dec->pre_skip)) {
        opus_close(dec);
        return NULL;
    }

    return dec;
}

unsigned int opus_get_channels(const opus_decoder_wrap_t * dec) {
    return dec->channels;
}

unsigned int opus_get_sample_rate(const opus_decoder_wrap_t * dec) {
    (void) dec;
    return 48000;
}

uint64_t opus_get_total_pcm_frame_count(const opus_decoder_wrap_t * dec) {
    return dec->total_pcm_frames;
}

uint64_t opus_read_pcm_frames_s16(opus_decoder_wrap_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    uint64_t frames_written = 0;

    while (frames_written < frames_to_read) {
        if (dec->carry_read_pos >= dec->carry_frames) {
            if (!decode_next_packet(dec)) break;
        }

        uint64_t available = dec->carry_frames - dec->carry_read_pos;
        uint64_t to_copy = frames_to_read - frames_written;
        if (to_copy > available) to_copy = available;

        memcpy(buffer_out + frames_written * dec->channels, dec->carry_buffer + dec->carry_read_pos * dec->channels,
               (size_t) to_copy * dec->channels * sizeof(int16_t));

        dec->carry_read_pos += to_copy;
        frames_written += to_copy;
    }

    return frames_written;
}

bool opus_seek_to_pcm_frame(opus_decoder_wrap_t * dec, uint64_t frame_index) {
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;

    uint64_t target_granule = frame_index + dec->pre_skip;
    uint64_t landed_granule = ogg_demux_seek_near_granule(dec->demux, target_granule);

    /* Mandatory: libopus's SILK (LSF/pitch prediction history) and CELT
     * (MDCT overlap buffer) state assumes unbroken packet continuity.
     * Decoding forward after jumping the container-level read cursor
     * without resetting this produces audible glitches for the first
     * several packets. */
    if (opus_decoder_ctl(dec->dec, OPUS_RESET_STATE) != OPUS_OK) return false;

    dec->carry_frames = 0;
    dec->carry_read_pos = 0;

    uint64_t discard = target_granule > landed_granule ? target_granule - landed_granule : 0;
    return discard_frames(dec, discard);
}

void opus_close(opus_decoder_wrap_t * dec) {
    if (!dec) return;
    if (dec->dec) opus_decoder_destroy(dec->dec);
    if (dec->demux) ogg_demux_close(dec->demux);
    free(dec->packet_buf);
    free(dec);
}
