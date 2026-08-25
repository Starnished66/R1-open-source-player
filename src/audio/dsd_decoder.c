#include "dsd_decoder.h"
#include "dsd_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DSD_MAX_CHANNELS 8
#define DFF_BUFFER_SIZE 4096

typedef enum {
    DSD_CONTAINER_DSF,
    DSD_CONTAINER_DFF
} dsd_container_t;

struct dsd_decoder {
    FILE * f;
    dsd_container_t container;

    unsigned int channels;
    unsigned int dsd_sample_rate;
    unsigned int pcm_sample_rate;
    int decimation_factor;
    uint64_t total_pcm_frames;
    uint64_t current_pcm_frame;

    long data_start_offset;

    /* DSF: data is block-interleaved -- all of channel 0's block, then
     * channel 1's, etc, repeating. Buffer one full multi-channel block at a
     * time and index into it directly. */
    uint32_t dsf_block_size;
    uint8_t * dsf_block_buffer;
    uint32_t dsf_block_pos; /* position within each channel's segment of the buffered block */

    /* DFF: data is simple round-robin byte interleaving (ch0,ch1,...,ch0,...). */
    uint8_t dff_buffer[DFF_BUFFER_SIZE];
    size_t dff_buffer_len;
    size_t dff_buffer_pos;

    dsd_filter_design_t filter_design;
    dsd_channel_state_t channel_states[DSD_MAX_CHANNELS];
};

static uint32_t read_u32le(const uint8_t * b) {
    return (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
}

static uint64_t read_u64le(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

static uint32_t read_u32be(const uint8_t * b) {
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | (uint32_t) b[3];
}

static uint16_t read_u16be(const uint8_t * b) {
    return (uint16_t) (((uint16_t) b[0] << 8) | b[1]);
}

static uint64_t read_u64be(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

/* Picks the decimation factor landing closest to 352.8kHz output, rejecting
 * anything above DSD128 (not supported -- see header comment). */
static int pick_decimation_factor(unsigned int dsd_rate) {
    if (dsd_rate <= 2822400 * 1.1) return 8;  /* DSD64  -> 352.8kHz */
    if (dsd_rate <= 5644800 * 1.1) return 16; /* DSD128 -> 352.8kHz */
    return 0; /* DSD256+ not supported */
}

static bool finish_setup(dsd_decoder_t * dec, uint64_t total_dsd_samples_per_channel) {
    dec->decimation_factor = pick_decimation_factor(dec->dsd_sample_rate);
    if (dec->decimation_factor == 0 || dec->channels == 0 || dec->channels > DSD_MAX_CHANNELS) return false;

    dec->pcm_sample_rate = dec->dsd_sample_rate / (unsigned int) dec->decimation_factor;
    dec->total_pcm_frames = total_dsd_samples_per_channel / (uint64_t) dec->decimation_factor;

    int num_taps = (dec->decimation_factor == 8) ? 192 : 256;
    dsd_filter_design(&dec->filter_design, num_taps, dec->decimation_factor);
    for (unsigned int ch = 0; ch < dec->channels; ch++) dsd_channel_reset(&dec->channel_states[ch]);

    return true;
}

static dsd_decoder_t * open_dsf(FILE * f) {
    /* "DSD " chunk: magic(4) + chunkSize u64LE(8, =28) + totalFileSize u64LE(8) + metadataPointer u64LE(8).
     * The caller only peeked at the magic and rewound, so it's still unread here. */
    uint8_t dsd_chunk[28];
    if (fread(dsd_chunk, 1, sizeof(dsd_chunk), f) != sizeof(dsd_chunk)) return NULL;
    if (memcmp(dsd_chunk, "DSD ", 4) != 0) return NULL;

    /* "fmt " chunk: magic(4) + chunkSize u64LE(8, =52) + 40 bytes of fields */
    uint8_t fmt_header[12];
    if (fread(fmt_header, 1, sizeof(fmt_header), f) != sizeof(fmt_header)) return NULL;
    if (memcmp(fmt_header, "fmt ", 4) != 0) return NULL;

    uint8_t fmt[40];
    if (fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) return NULL;

    unsigned int channels = read_u32le(fmt + 12);
    unsigned int sample_rate = read_u32le(fmt + 16);
    uint64_t sample_count = read_u64le(fmt + 24);
    uint32_t block_size = read_u32le(fmt + 32);

    /* "data" chunk: magic(4) + chunkSize u64LE(8) + raw block-interleaved DSD data */
    uint8_t data_header[12];
    if (fread(data_header, 1, sizeof(data_header), f) != sizeof(data_header)) return NULL;
    if (memcmp(data_header, "data", 4) != 0) return NULL;

    dsd_decoder_t * dec = calloc(1, sizeof(*dec));
    dec->f = f;
    dec->container = DSD_CONTAINER_DSF;
    dec->channels = channels;
    dec->dsd_sample_rate = sample_rate;
    dec->dsf_block_size = block_size;
    dec->data_start_offset = ftell(f);
    dec->dsf_block_pos = block_size; /* forces a fresh block load on first read */
    dec->dsf_block_buffer = malloc((size_t) channels * block_size);

    if (!finish_setup(dec, sample_count)) {
        free(dec->dsf_block_buffer);
        free(dec);
        return NULL;
    }
    return dec;
}

/* Reads a DFF chunk header (big-endian, 64-bit size -- unlike AIFF's 32-bit). */
static bool dff_read_chunk_header(FILE * f, char id_out[5], uint64_t * size_out) {
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) return false;
    memcpy(id_out, header, 4);
    id_out[4] = '\0';
    *size_out = read_u64be(header + 4);
    return true;
}

static dsd_decoder_t * open_dff(FILE * f) {
    /* "FRM8" master chunk: magic(4) + size u64BE(8) + formType(4, "DSD ").
     * The caller only peeked at the magic and rewound, so it's still unread here. */
    char frm8_id[5];
    uint64_t frm8_size;
    if (!dff_read_chunk_header(f, frm8_id, &frm8_size)) return NULL;
    if (strcmp(frm8_id, "FRM8") != 0) return NULL;

    uint8_t form_type[4];
    if (fread(form_type, 1, sizeof(form_type), f) != sizeof(form_type)) return NULL;
    if (memcmp(form_type, "DSD ", 4) != 0) return NULL;

    unsigned int channels = 0;
    unsigned int sample_rate = 0;
    long data_start_offset = 0;
    uint64_t data_size = 0;
    bool have_data = false;

    while (1) {
        char id[5];
        uint64_t size;
        long chunk_data_start = ftell(f);
        if (!dff_read_chunk_header(f, id, &size)) break;
        chunk_data_start = ftell(f);

        if (strcmp(id, "PROP") == 0) {
            uint8_t prop_type[4];
            if (fread(prop_type, 1, sizeof(prop_type), f) != sizeof(prop_type)) break;
            long prop_end = chunk_data_start + (long) size;

            while (ftell(f) < prop_end) {
                char sub_id[5];
                uint64_t sub_size;
                long sub_data_start = ftell(f);
                if (!dff_read_chunk_header(f, sub_id, &sub_size)) break;
                sub_data_start = ftell(f);

                if (strcmp(sub_id, "FS  ") == 0 && sub_size >= 4) { /* DFF chunk IDs are always 4 bytes: "FS" padded with two spaces */
                    uint8_t buf[4];
                    if (fread(buf, 1, 4, f) == 4) sample_rate = read_u32be(buf);
                } else if (strcmp(sub_id, "CHNL") == 0 && sub_size >= 2) {
                    uint8_t buf[2];
                    if (fread(buf, 1, 2, f) == 2) channels = read_u16be(buf);
                }

                long next = sub_data_start + (long) sub_size + (long) (sub_size & 1);
                if (fseek(f, next, SEEK_SET) != 0) break;
            }
            fseek(f, prop_end + (prop_end & 1), SEEK_SET);
        } else if (strcmp(id, "DSD ") == 0) {
            data_start_offset = chunk_data_start;
            data_size = size;
            have_data = true;
            fseek(f, chunk_data_start + (long) size + (long) (size & 1), SEEK_SET);
        } else {
            long next = chunk_data_start + (long) size + (long) (size & 1);
            if (fseek(f, next, SEEK_SET) != 0) break;
        }

        if (have_data && channels != 0 && sample_rate != 0) {
            /* Keep scanning isn't needed once we have everything we need
             * (DSD data is conventionally last anyway, but be defensive). */
        }
    }

    if (!have_data || channels == 0 || sample_rate == 0) return NULL;

    dsd_decoder_t * dec = calloc(1, sizeof(*dec));
    dec->f = f;
    dec->container = DSD_CONTAINER_DFF;
    dec->channels = channels;
    dec->dsd_sample_rate = sample_rate;
    dec->data_start_offset = data_start_offset;
    dec->dff_buffer_len = 0;
    dec->dff_buffer_pos = 0;

    uint64_t total_dsd_bytes = data_size;
    uint64_t total_samples_per_channel = (total_dsd_bytes / channels) * 8;

    if (!finish_setup(dec, total_samples_per_channel)) {
        free(dec);
        return NULL;
    }

    fseek(f, dec->data_start_offset, SEEK_SET);
    return dec;
}

dsd_decoder_t * dsd_open_file(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    uint8_t magic[4];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic)) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    dsd_decoder_t * dec = NULL;
    if (memcmp(magic, "DSD ", 4) == 0) {
        dec = open_dsf(f);
    } else if (memcmp(magic, "FRM8", 4) == 0) {
        dec = open_dff(f);
    }

    if (!dec) {
        fclose(f);
        return NULL;
    }
    return dec;
}

unsigned int dsd_get_channels(const dsd_decoder_t * dec) {
    return dec->channels;
}

unsigned int dsd_get_source_sample_rate(const dsd_decoder_t * dec) {
    return dec->dsd_sample_rate;
}

unsigned int dsd_get_pcm_sample_rate(const dsd_decoder_t * dec) {
    return dec->pcm_sample_rate;
}

uint64_t dsd_get_total_pcm_frame_count(const dsd_decoder_t * dec) {
    return dec->total_pcm_frames;
}

static bool dsf_get_channel_byte(dsd_decoder_t * dec, unsigned int ch, uint8_t * out_byte) {
    if (dec->dsf_block_pos >= dec->dsf_block_size) {
        size_t to_read = (size_t) dec->channels * dec->dsf_block_size;
        size_t got = fread(dec->dsf_block_buffer, 1, to_read, dec->f);
        if (got < to_read) memset(dec->dsf_block_buffer + got, 0, to_read - got); /* zero-pad a short final block */
        if (got == 0) return false;
        dec->dsf_block_pos = 0;
    }
    *out_byte = dec->dsf_block_buffer[ch * dec->dsf_block_size + dec->dsf_block_pos];
    return true;
}

static bool dff_get_channel_byte(dsd_decoder_t * dec, unsigned int ch, uint8_t * out_byte) {
    (void) ch; /* DFF interleaving is purely positional; the caller already iterates channels in file order */
    if (dec->dff_buffer_pos >= dec->dff_buffer_len) {
        dec->dff_buffer_len = fread(dec->dff_buffer, 1, sizeof(dec->dff_buffer), dec->f);
        dec->dff_buffer_pos = 0;
        if (dec->dff_buffer_len == 0) return false;
    }
    *out_byte = dec->dff_buffer[dec->dff_buffer_pos];
    return true;
}

uint64_t dsd_read_pcm_frames_s16(dsd_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    uint64_t frames_written = 0;
    bool lsb_first = (dec->container == DSD_CONTAINER_DSF);
    /* decimation_factor is always a multiple of 8 (8 or 16 -- see
     * pick_decimation_factor), so this is exact: every channel needs
     * exactly this many whole bytes, in lockstep, to produce one sample. */
    int bytes_per_output_frame = dec->decimation_factor / 8;

    while (frames_written < frames_to_read && dec->current_pcm_frame < dec->total_pcm_frames) {
        float frame_samples[DSD_MAX_CHANNELS];
        bool ok = true;

        for (int round = 0; round < bytes_per_output_frame && ok; round++) {
            for (unsigned int ch = 0; ch < dec->channels; ch++) {
                uint8_t byte;
                bool have_byte = (dec->container == DSD_CONTAINER_DSF)
                    ? dsf_get_channel_byte(dec, ch, &byte)
                    : dff_get_channel_byte(dec, ch, &byte);
                if (!have_byte) { ok = false; break; }

                for (int bit_i = 0; bit_i < 8; bit_i++) {
                    int bit_index = lsb_first ? bit_i : (7 - bit_i);
                    float bit_value = ((byte >> bit_index) & 1) ? 1.0f : -1.0f;
                    float out_sample;
                    if (dsd_channel_feed_bit(&dec->channel_states[ch], &dec->filter_design, bit_value, &out_sample)) {
                        frame_samples[ch] = out_sample; /* only fires on the very last bit of the last round */
                    }
                }

                if (dec->container == DSD_CONTAINER_DFF) dec->dff_buffer_pos++;
            }
            if (ok && dec->container == DSD_CONTAINER_DSF) dec->dsf_block_pos++;
        }

        if (!ok) break;

        for (unsigned int ch = 0; ch < dec->channels; ch++) {
            float v = frame_samples[ch] * 32767.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            buffer_out[frames_written * dec->channels + ch] = (int16_t) v;
        }

        frames_written++;
        dec->current_pcm_frame++;
    }

    return frames_written;
}

bool dsd_seek_to_pcm_frame(dsd_decoder_t * dec, uint64_t frame_index) {
    if (frame_index > dec->total_pcm_frames) frame_index = dec->total_pcm_frames;

    uint64_t dsd_byte_index = (frame_index * (uint64_t) dec->decimation_factor) / 8;

    for (unsigned int ch = 0; ch < dec->channels; ch++) dsd_channel_reset(&dec->channel_states[ch]);

    if (dec->container == DSD_CONTAINER_DSF) {
        uint64_t block_index = dsd_byte_index / dec->dsf_block_size;
        uint32_t byte_in_block = (uint32_t) (dsd_byte_index % dec->dsf_block_size);

        long block_offset = dec->data_start_offset + (long) (block_index * dec->channels * dec->dsf_block_size);
        if (fseek(dec->f, block_offset, SEEK_SET) != 0) return false;

        size_t to_read = (size_t) dec->channels * dec->dsf_block_size;
        size_t got = fread(dec->dsf_block_buffer, 1, to_read, dec->f);
        if (got < to_read) memset(dec->dsf_block_buffer + got, 0, to_read - got);
        dec->dsf_block_pos = byte_in_block;
    } else {
        long byte_offset = dec->data_start_offset + (long) (dsd_byte_index * dec->channels);
        if (fseek(dec->f, byte_offset, SEEK_SET) != 0) return false;
        dec->dff_buffer_len = 0;
        dec->dff_buffer_pos = 0;
    }

    dec->current_pcm_frame = frame_index;
    return true;
}

void dsd_close(dsd_decoder_t * dec) {
    if (!dec) return;
    fclose(dec->f);
    free(dec->dsf_block_buffer);
    free(dec);
}
