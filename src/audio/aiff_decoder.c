#include "aiff_decoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AIFF_MAX_CHUNK_FRAMES 8192

struct aiff_decoder {
    FILE * f;
    unsigned int channels;
    unsigned int sample_rate;
    unsigned int bits_per_sample;
    bool little_endian; /* AIFC 'sowt' compression type: PCM stored little-endian */
    uint64_t total_frames;
    long data_start_offset; /* file offset where SSND sample data begins */
    uint64_t current_frame;

    uint8_t * raw_buffer;
    size_t raw_buffer_capacity_bytes;
    uint64_t max_chunk_frames;
};

static bool checked_mul_size(size_t a, size_t b, size_t * out) {
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static uint32_t read_u32be(const uint8_t * b) {
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | (uint32_t) b[3];
}

static uint16_t read_u16be(const uint8_t * b) {
    return (uint16_t) (((uint16_t) b[0] << 8) | b[1]);
}

/* AIFF stores sample rate as an 80-bit IEEE 754 extended precision float. */
static double read_ieee80_extended(const uint8_t bytes[10]) {
    int sign = (bytes[0] & 0x80) ? -1 : 1;
    int exponent = ((bytes[0] & 0x7F) << 8) | bytes[1];

    uint64_t mantissa = 0;
    for (int i = 0; i < 8; i++) mantissa = (mantissa << 8) | bytes[2 + i];

    if (exponent == 0 && mantissa == 0) return 0.0;

    double value = (double) mantissa * pow(2.0, (double) (exponent - 16383 - 63));
    return sign * value;
}

aiff_decoder_t * aiff_open_file(const char * path) {
    if (!path) return NULL;
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "FORM", 4) != 0 ||
        (memcmp(header + 8, "AIFF", 4) != 0 && memcmp(header + 8, "AIFC", 4) != 0)) {
        fclose(f);
        return NULL;
    }

    aiff_decoder_t * dec = calloc(1, sizeof(*dec));
    if (!dec) {
        fclose(f);
        return NULL;
    }
    dec->f = f;

    bool have_comm = false;
    bool have_ssnd = false;

    while (1) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header)) break;

        char chunk_id[5] = {0};
        memcpy(chunk_id, chunk_header, 4);
        uint32_t chunk_size = read_u32be(chunk_header + 4);
        long chunk_data_start = ftell(f);

        if (strcmp(chunk_id, "COMM") == 0 && chunk_size >= 18) {
            uint8_t comm[18];
            if (fread(comm, 1, sizeof(comm), f) != sizeof(comm)) break;

            dec->channels = read_u16be(comm);
            dec->total_frames = read_u32be(comm + 2);
            dec->bits_per_sample = read_u16be(comm + 6);
            dec->sample_rate = (unsigned int) read_ieee80_extended(comm + 8);
            dec->little_endian = false;

            /* AIFC: a compressionType follows COMM's normal 18 bytes. 'sowt' is
             * "PCM, but little-endian" -- common from macOS-authored files.
             * Anything else compressed is not supported. */
            if (memcmp(header + 8, "AIFC", 4) == 0 && chunk_size >= 22) {
                uint8_t comp_type[4];
                if (fread(comp_type, 1, sizeof(comp_type), f) == sizeof(comp_type)) {
                    if (memcmp(comp_type, "sowt", 4) == 0) {
                        dec->little_endian = true;
                    } else if (memcmp(comp_type, "NONE", 4) != 0) {
                        fclose(f);
                        free(dec);
                        return NULL; /* unsupported compressed AIFC variant */
                    }
                }
            }

            have_comm = true;
        } else if (strcmp(chunk_id, "SSND") == 0) {
            uint8_t ssnd_header[8];
            if (fread(ssnd_header, 1, sizeof(ssnd_header), f) != sizeof(ssnd_header)) break;
            uint32_t data_offset = read_u32be(ssnd_header);
            dec->data_start_offset = chunk_data_start + 8 + (long) data_offset;
            have_ssnd = true;
            /* Sample data itself doesn't need parsing here -- reads happen on demand. */
        }

        if (have_comm && have_ssnd) break;

        /* Chunks are padded to an even number of bytes. */
        long next = chunk_data_start + (long) chunk_size + (long) (chunk_size & 1);
        if (fseek(f, next, SEEK_SET) != 0) break;
    }

    if (!have_comm || !have_ssnd || dec->channels == 0 || dec->channels > 8 ||
        (dec->bits_per_sample != 8 && dec->bits_per_sample != 16 && dec->bits_per_sample != 24)) {
        fclose(f);
        free(dec);
        return NULL;
    }

    unsigned int bytes_per_sample = dec->bits_per_sample / 8;
    size_t frame_bytes = 0;
    if (!checked_mul_size((size_t) dec->channels, (size_t) bytes_per_sample, &frame_bytes) ||
        !checked_mul_size((size_t) AIFF_MAX_CHUNK_FRAMES, frame_bytes, &dec->raw_buffer_capacity_bytes)) {
        fclose(f);
        free(dec);
        return NULL;
    }

    dec->raw_buffer = malloc(dec->raw_buffer_capacity_bytes);
    if (!dec->raw_buffer) {
        fclose(f);
        free(dec);
        return NULL;
    }
    dec->max_chunk_frames = AIFF_MAX_CHUNK_FRAMES;

    fseek(f, dec->data_start_offset, SEEK_SET);
    return dec;
}

unsigned int aiff_get_channels(const aiff_decoder_t * dec) {
    return dec ? dec->channels : 0;
}

unsigned int aiff_get_sample_rate(const aiff_decoder_t * dec) {
    return dec ? dec->sample_rate : 0;
}

unsigned int aiff_get_bits_per_sample(const aiff_decoder_t * dec) {
    return dec ? dec->bits_per_sample : 0;
}

uint64_t aiff_get_total_pcm_frame_count(const aiff_decoder_t * dec) {
    return dec ? dec->total_frames : 0;
}

decoder_read_result_t aiff_read_pcm_frames_s16(aiff_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !dec->f || !dec->raw_buffer || !buffer_out) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    if (dec->current_frame >= dec->total_frames) {
        res.status = DECODER_READ_EOF;
        return res;
    }

    unsigned int bytes_per_sample = dec->bits_per_sample / 8;
    size_t frame_bytes = (size_t) dec->channels * bytes_per_sample;
    if (frame_bytes == 0) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    uint64_t total_frames_left = dec->total_frames - dec->current_frame;
    if (frames_to_read > total_frames_left) {
        frames_to_read = total_frames_left;
    }

    while (res.frames < frames_to_read) {
        uint64_t chunk = frames_to_read - res.frames;
        if (chunk > dec->max_chunk_frames) {
            chunk = dec->max_chunk_frames;
        }

        size_t bytes_to_read = (size_t) chunk * frame_bytes;
        size_t bytes_read = fread(dec->raw_buffer, 1, bytes_to_read, dec->f);
        uint64_t frames_read = bytes_read / frame_bytes;

        if (frames_read == 0) {
            if (feof(dec->f)) {
                res.status = DECODER_READ_EOF;
                break;
            } else if (ferror(dec->f)) {
                res.status = DECODER_READ_FATAL_ERROR;
                break;
            } else {
                res.status = DECODER_READ_EOF;
                break;
            }
        }

        size_t sample_count = (size_t) frames_read * dec->channels;
        int16_t * out_ptr = buffer_out + (size_t) res.frames * dec->channels;

        for (size_t i = 0; i < sample_count; i++) {
            const uint8_t * s = dec->raw_buffer + i * bytes_per_sample;
            int32_t sample;

            if (dec->bits_per_sample == 8) {
                sample = (int32_t) ((int8_t) s[0]) << 8; /* AIFF 8-bit PCM is signed */
            } else if (dec->bits_per_sample == 16) {
                sample = dec->little_endian ? (int16_t) (s[0] | (s[1] << 8))
                                             : (int16_t) ((s[0] << 8) | s[1]);
            } else { /* 24-bit: keep the top 16 bits, same precision-reduction dr_libs uses */
                int32_t s24 = dec->little_endian
                    ? (int32_t) ((uint32_t) s[0] | ((uint32_t) s[1] << 8) | ((uint32_t) s[2] << 16))
                    : (int32_t) (((uint32_t) s[0] << 16) | ((uint32_t) s[1] << 8) | (uint32_t) s[2]);
                if (s24 & 0x800000) s24 |= (int32_t) 0xFF000000; /* sign-extend */
                sample = s24 >> 8;
            }

            out_ptr[i] = (int16_t) sample;
        }

        res.frames += frames_read;
        dec->current_frame += frames_read;

        if (frames_read < chunk) {
            if (feof(dec->f)) {
                res.status = DECODER_READ_EOF;
            } else if (ferror(dec->f)) {
                res.status = DECODER_READ_FATAL_ERROR;
            }
            break;
        }
    }

    if (res.frames > 0) {
        res.status = DECODER_READ_OK;
    } else if (res.status == DECODER_READ_OK) {
        res.status = (dec->current_frame >= dec->total_frames) ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
    }

    return res;
}

decoder_read_result_t aiff_read_pcm_frames_s32(aiff_decoder_t * dec, uint64_t frames_to_read, int32_t * buffer_out) {
    decoder_read_result_t res = { .frames = 0, .status = DECODER_READ_OK };
    if (!dec || !dec->f || !buffer_out) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    if (dec->current_frame >= dec->total_frames) {
        res.status = DECODER_READ_EOF;
        return res;
    }

    unsigned int bytes_per_sample = dec->bits_per_sample / 8;
    size_t frame_bytes = (size_t) dec->channels * bytes_per_sample;
    if (frame_bytes == 0) {
        res.status = DECODER_READ_FATAL_ERROR;
        return res;
    }

    uint64_t total_frames_left = dec->total_frames - dec->current_frame;
    if (frames_to_read > total_frames_left) {
        frames_to_read = total_frames_left;
    }

    while (res.frames < frames_to_read) {
        uint64_t chunk = frames_to_read - res.frames;
        if (chunk > dec->max_chunk_frames) {
            chunk = dec->max_chunk_frames;
        }

        size_t bytes_to_read = (size_t) chunk * frame_bytes;
        size_t bytes_read = fread(dec->raw_buffer, 1, bytes_to_read, dec->f);
        uint64_t frames_read = bytes_read / frame_bytes;

        if (frames_read == 0) {
            if (feof(dec->f)) {
                res.status = DECODER_READ_EOF;
                break;
            } else if (ferror(dec->f)) {
                res.status = DECODER_READ_FATAL_ERROR;
                break;
            } else {
                res.status = DECODER_READ_EOF;
                break;
            }
        }

        size_t sample_count = (size_t) frames_read * dec->channels;
        int32_t * out_ptr = buffer_out + (size_t) res.frames * dec->channels;

        for (size_t i = 0; i < sample_count; i++) {
            const uint8_t * s = dec->raw_buffer + i * bytes_per_sample;
            int32_t sample;

            if (dec->bits_per_sample == 8) {
                sample = (int32_t) ((int8_t) s[0]) << 8; /* AIFF 8-bit PCM is signed */
            } else if (dec->bits_per_sample == 16) {
                sample = dec->little_endian ? (int16_t) (s[0] | (s[1] << 8))
                                             : (int16_t) ((s[0] << 8) | s[1]);
            } else { /* 24-bit: sign-extended right-justified int32 directly in low 24 bits for S24_LE (no >> 8) */
                int32_t s24 = dec->little_endian
                    ? (int32_t) ((uint32_t) s[0] | ((uint32_t) s[1] << 8) | ((uint32_t) s[2] << 16))
                    : (int32_t) (((uint32_t) s[0] << 16) | ((uint32_t) s[1] << 8) | (uint32_t) s[2]);
                if (s24 & 0x800000) s24 |= (int32_t) 0xFF000000; /* sign-extend */
                sample = s24;
            }

            out_ptr[i] = sample;
        }

        res.frames += frames_read;
        dec->current_frame += frames_read;

        if (frames_read < chunk) {
            if (feof(dec->f)) {
                res.status = DECODER_READ_EOF;
            } else if (ferror(dec->f)) {
                res.status = DECODER_READ_FATAL_ERROR;
            }
            break;
        }
    }

    if (res.frames > 0) {
        res.status = DECODER_READ_OK;
    } else if (res.status == DECODER_READ_OK) {
        res.status = (dec->current_frame >= dec->total_frames) ? DECODER_READ_EOF : DECODER_READ_FATAL_ERROR;
    }

    return res;
}

bool aiff_seek_to_pcm_frame(aiff_decoder_t * dec, uint64_t frame_index) {
    if (!dec || !dec->f) return false;
    if (frame_index > dec->total_frames) frame_index = dec->total_frames;

    unsigned int bytes_per_sample = dec->bits_per_sample / 8;
    size_t frame_bytes = (size_t) dec->channels * bytes_per_sample;
    long byte_offset = dec->data_start_offset + (long) (frame_index * frame_bytes);

    if (fseek(dec->f, byte_offset, SEEK_SET) != 0) return false;
    dec->current_frame = frame_index;
    return true;
}

void aiff_close(aiff_decoder_t * dec) {
    if (!dec) return;
    if (dec->f) fclose(dec->f);
    free(dec->raw_buffer);
    free(dec);
}
