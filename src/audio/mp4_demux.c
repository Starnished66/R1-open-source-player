#include "mp4_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char type[5];
    uint64_t size;   /* total box size, including header */
    long header_size; /* 8 (normal) or 16 (64-bit extended size) */
    long data_start;  /* file offset where the box's payload begins */
} box_header_t;

typedef struct {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
} stsc_entry_t;

/* Expanded per-sample tables are ~12 bytes each. A 25h AAC audiobook is
 * millions of access units -- tens of MB, enough to OOM this 56 MiB
 * device on open (Linux overcommit makes malloc succeed, then the first
 * write is SIGKILL). Keep ISO-BMFF stsz/stco/stsc compact past this. */
#define MP4_EXPAND_MAX_SAMPLES 65536
#define MP4_MAX_STSC_ENTRIES 4096

struct mp4_demux {
    FILE * f;

    char codec_fourcc[5];
    uint8_t * codec_config;
    uint32_t codec_config_size;

    unsigned int channels;
    unsigned int sample_rate;

    uint32_t sample_count;
    uint32_t uniform_size; /* stsz default; 0 = per-sample table on disk */
    long stsz_table_offset;
    uint32_t * sample_sizes; /* NULL in compact mode */

    uint32_t chunk_count;
    bool stco_is64;
    long stco_table_offset;
    uint32_t stsc_count;
    stsc_entry_t * stsc;
    uint64_t * sample_offsets; /* NULL in compact mode */

    uint32_t cursor_index;
    uint64_t cursor_offset;

    uint32_t frames_per_sample;
    uint64_t total_pcm_frames;
};

static uint32_t read_u32be(const uint8_t * b) {
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | (uint32_t) b[3];
}

static uint64_t read_u64be(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

static uint16_t read_u16be(const uint8_t * b) {
    return (uint16_t) (((uint16_t) b[0] << 8) | b[1]);
}

static bool read_box_header(FILE * f, box_header_t * out) {
    long start = ftell(f);
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;

    uint32_t size32 = read_u32be(hdr);
    memcpy(out->type, hdr + 4, 4);
    out->type[4] = '\0';

    if (size32 == 1) {
        uint8_t ext[8];
        if (fread(ext, 1, sizeof(ext), f) != sizeof(ext)) return false;
        out->size = read_u64be(ext);
        out->header_size = 16;
    } else if (size32 == 0) {
        long cur = ftell(f);
        fseek(f, 0, SEEK_END);
        long end = ftell(f);
        fseek(f, cur, SEEK_SET);
        out->size = (uint64_t) (end - start);
        out->header_size = 8;
    } else {
        out->size = size32;
        out->header_size = 8;
    }

    out->data_start = start + out->header_size;
    return true;
}

/* Scans children of a plain container box (moov/trak/mdia/minf/stbl/udta/...
 * -- no extra fields before the first child) for one with the given type,
 * within [container_start, container_start + container_size). */
static bool find_child_box(FILE * f, long container_start, uint64_t container_size, const char * type, box_header_t * out) {
    long end = container_start + (long) container_size;
    fseek(f, container_start, SEEK_SET);

    while (ftell(f) < end) {
        box_header_t box;
        if (!read_box_header(f, &box)) return false;
        if (strcmp(box.type, type) == 0) {
            *out = box;
            return true;
        }
        long next = (long) (box.data_start - box.header_size) + (long) box.size;
        if (fseek(f, next, SEEK_SET) != 0) return false;
    }
    return false;
}


/* Finds the first "trak" box whose mdia/hdlr declares handler_type "soun". */
static bool find_audio_trak(FILE * f, box_header_t moov, box_header_t * out_trak) {
    long end = moov.data_start + (long) moov.size - moov.header_size;
    fseek(f, moov.data_start, SEEK_SET);

    while (ftell(f) < end) {
        box_header_t trak;
        if (!read_box_header(f, &trak)) return false;

        if (strcmp(trak.type, "trak") == 0) {
            box_header_t mdia, hdlr;
            if (find_child_box(f, trak.data_start, trak.size - (uint64_t) trak.header_size, "mdia", &mdia) &&
                find_child_box(f, mdia.data_start, mdia.size - (uint64_t) mdia.header_size, "hdlr", &hdlr)) {
                uint8_t buf[12];
                fseek(f, hdlr.data_start + 4, SEEK_SET); /* skip version/flags(4), then predefined(4) */
                if (fread(buf, 1, 8, f) == 8 && memcmp(buf + 4, "soun", 4) == 0) {
                    *out_trak = trak;
                    return true;
                }
            }
        }

        long next = (long) (trak.data_start - trak.header_size) + (long) trak.size;
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    return false;
}

/* Parses the single audio sample entry inside stsd: the fixed 36-byte
 * AudioSampleEntry fields, followed by the codec-specific config (the ALAC
 * magic cookie box, or an "esds" box for AAC) -- see
 * ALACMagicCookieDescription.txt for the authoritative byte layout this
 * matches. */
static bool parse_stsd(mp4_demux_t * d, box_header_t stsd) {
    /* stsd is a FullBox: version/flags(4) + entry_count(4), then entries */
    fseek(d->f, stsd.data_start + 8, SEEK_SET);

    uint8_t entry_header[36];
    if (fread(entry_header, 1, sizeof(entry_header), d->f) != sizeof(entry_header)) return false;

    uint32_t entry_size = read_u32be(entry_header);
    memcpy(d->codec_fourcc, entry_header + 4, 4);
    d->codec_fourcc[4] = '\0';
    d->channels = read_u16be(entry_header + 16);
    /* entry_header+24 (sample_rate) is a 16.16 fixed-point mirror of the
     * real rate; the codec's own config (ALACSpecificConfig, or the AAC
     * ASC's sampling frequency index) is authoritative and read separately
     * after Init()/parsing, so this is just a fallback. */
    d->sample_rate = read_u32be(entry_header + 24) >> 16;

    if (entry_size <= 36) return false;
    uint32_t config_region_size = entry_size - 36;

    uint8_t * config_region = malloc(config_region_size);
    if (!config_region) return false;
    if (fread(config_region, 1, config_region_size, d->f) != config_region_size) {
        free(config_region);
        return false;
    }

    if (strcmp(d->codec_fourcc, "alac") == 0) {
        /* The whole region is the nested 'alac' box containing the magic
         * cookie; ALACDecoder::Init() parses this directly (it knows how
         * to skip the box header itself). */
        d->codec_config = config_region;
        d->codec_config_size = config_region_size;
        return true;
    }

    if (strcmp(d->codec_fourcc, "mp4a") == 0) {
        /* Find the nested "esds" box and extract the DecoderSpecificInfo
         * payload from its MPEG-4 descriptor tags (a small tag+length+value
         * structure, not a plain sub-box). */
        for (uint32_t pos = 0; pos + 8 <= config_region_size;) {
            uint32_t box_size = read_u32be(config_region + pos);
            if (box_size < 8 || pos + box_size > config_region_size) break;

            if (memcmp(config_region + pos + 4, "esds", 4) == 0) {
                uint32_t p = pos + 8 + 4; /* box header(8) + FullBox version/flags(4) */
                /* Walk descriptor tags: tag(1) + size (variable-length, top
                 * bit continuation encoding) + value. We want tag 0x05
                 * (DecoderSpecificInfo), nested inside tag 0x03 (ESDescriptor)
                 * -> tag 0x04 (DecoderConfigDescriptor). */
                while (p + 2 <= pos + box_size) {
                    uint8_t tag = config_region[p++];
                    uint32_t desc_len = 0;
                    for (int i = 0; i < 4 && p < pos + box_size; i++) {
                        uint8_t b = config_region[p++];
                        desc_len = (desc_len << 7) | (b & 0x7F);
                        if (!(b & 0x80)) break;
                    }
                    if (tag == 0x05) { /* DecoderSpecificInfo: this is the raw ASC */
                        if (p + desc_len > config_region_size) break;
                        d->codec_config = malloc(desc_len);
                        if (!d->codec_config) { free(config_region); return false; }
                        memcpy(d->codec_config, config_region + p, desc_len);
                        d->codec_config_size = desc_len;
                        free(config_region);
                        return true;
                    }
                    if (tag == 0x03) { p += 3; continue; } /* ES_ID(2) + flags(1), then nested descriptors */
                    if (tag == 0x04) { p += 13; continue; } /* objectTypeIndication..avgBitrate, then nested descriptors */
                    p += desc_len; /* skip anything else */
                }
            }

            pos += box_size;
        }
    }

    free(config_region);
    return false;
}

static bool read_u32be_file(FILE * f, uint32_t * out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = read_u32be(b);
    return true;
}

static bool sample_size_at(mp4_demux_t * d, uint32_t i, uint32_t * out) {
    if (i >= d->sample_count) return false;
    if (d->sample_sizes) {
        *out = d->sample_sizes[i];
        return true;
    }
    if (d->uniform_size != 0) {
        *out = d->uniform_size;
        return true;
    }
    if (fseek(d->f, d->stsz_table_offset + (long) i * 4, SEEK_SET) != 0) return false;
    return read_u32be_file(d->f, out);
}

static bool chunk_offset_at(mp4_demux_t * d, uint32_t chunk_index0, uint64_t * out) {
    if (chunk_index0 >= d->chunk_count) return false;
    long stride = d->stco_is64 ? 8L : 4L;
    if (fseek(d->f, d->stco_table_offset + (long) chunk_index0 * stride, SEEK_SET) != 0) return false;
    uint8_t buf[8];
    size_t n = d->stco_is64 ? 8 : 4;
    if (fread(buf, 1, n, d->f) != n) return false;
    *out = d->stco_is64 ? read_u64be(buf) : (uint64_t) read_u32be(buf);
    return true;
}

static bool sample_offset_at(mp4_demux_t * d, uint32_t i, uint64_t * out) {
    if (i >= d->sample_count) return false;
    if (d->sample_offsets) {
        *out = d->sample_offsets[i];
        return true;
    }
    if (i == d->cursor_index) {
        *out = d->cursor_offset;
        return true;
    }
    if (i == d->cursor_index + 1) {
        uint32_t sz;
        if (!sample_size_at(d, d->cursor_index, &sz)) return false;
        d->cursor_index = i;
        d->cursor_offset += sz;
        *out = d->cursor_offset;
        return true;
    }

    uint32_t remaining = i;
    uint32_t chunk_1based = 1;
    uint32_t index_in_chunk = 0;
    bool found = false;
    for (uint32_t e = 0; e < d->stsc_count; e++) {
        uint32_t first = d->stsc[e].first_chunk;
        uint32_t spc = d->stsc[e].samples_per_chunk;
        if (spc == 0) return false;
        uint32_t next_first = (e + 1 < d->stsc_count) ? d->stsc[e + 1].first_chunk : d->chunk_count + 1;
        if (next_first <= first) return false;
        uint64_t nsamples = (uint64_t) (next_first - first) * spc;
        if ((uint64_t) remaining < nsamples) {
            chunk_1based = first + remaining / spc;
            index_in_chunk = remaining % spc;
            found = true;
            break;
        }
        remaining -= (uint32_t) nsamples;
    }
    if (!found || chunk_1based == 0) return false;

    uint64_t off;
    if (!chunk_offset_at(d, chunk_1based - 1, &off)) return false;
    uint32_t first_in_chunk = i - index_in_chunk;
    for (uint32_t s = 0; s < index_in_chunk; s++) {
        uint32_t sz;
        if (!sample_size_at(d, first_in_chunk + s, &sz)) return false;
        off += sz;
    }
    d->cursor_index = i;
    d->cursor_offset = off;
    *out = off;
    return true;
}

static bool parse_stsz(mp4_demux_t * d, box_header_t stsz) {
    uint8_t hdr[12];
    fseek(d->f, stsz.data_start, SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), d->f) != sizeof(hdr)) return false;

    d->uniform_size = read_u32be(hdr + 4);
    uint32_t count = read_u32be(hdr + 8);
    if (count == 0) return false;
    d->sample_count = count;
    d->stsz_table_offset = stsz.data_start + 12;

    if (count > MP4_EXPAND_MAX_SAMPLES) return true;

    d->sample_sizes = malloc(sizeof(uint32_t) * (size_t) count);
    if (!d->sample_sizes) return false;
    if (d->uniform_size != 0) {
        for (uint32_t i = 0; i < count; i++) d->sample_sizes[i] = d->uniform_size;
        return true;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!read_u32be_file(d->f, &d->sample_sizes[i])) return false;
    }
    return true;
}

/* Combines stco/co64 (chunk offsets) with stsc (samples-per-chunk ranges).
 * Short files still expand every sample offset into RAM. Long audiobooks
 * keep the compact boxes and resolve offsets on demand -- sequential
 * playback is a running cursor, seeks walk stsc then one chunk. */
static bool parse_sample_offsets(mp4_demux_t * d, box_header_t stbl) {
    box_header_t stco_box;
    d->stco_is64 = false;
    if (!find_child_box(d->f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stco", &stco_box)) {
        if (!find_child_box(d->f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "co64", &stco_box)) return false;
        d->stco_is64 = true;
    }

    box_header_t stsc_box;
    if (!find_child_box(d->f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stsc", &stsc_box)) return false;

    uint8_t hdr[8];
    fseek(d->f, stco_box.data_start, SEEK_SET);
    if (fread(hdr, 1, 8, d->f) != 8) return false;
    d->chunk_count = read_u32be(hdr + 4);
    if (d->chunk_count == 0) return false;
    d->stco_table_offset = stco_box.data_start + 8;

    fseek(d->f, stsc_box.data_start, SEEK_SET);
    if (fread(hdr, 1, 8, d->f) != 8) return false;
    d->stsc_count = read_u32be(hdr + 4);
    if (d->stsc_count == 0 || d->stsc_count > MP4_MAX_STSC_ENTRIES) return false;

    d->stsc = malloc(sizeof(stsc_entry_t) * (size_t) d->stsc_count);
    if (!d->stsc) return false;
    for (uint32_t i = 0; i < d->stsc_count; i++) {
        uint8_t buf[12];
        if (fread(buf, 1, 12, d->f) != 12) return false;
        d->stsc[i].first_chunk = read_u32be(buf);
        d->stsc[i].samples_per_chunk = read_u32be(buf + 4);
        if (d->stsc[i].first_chunk == 0 || d->stsc[i].samples_per_chunk == 0) return false;
    }

    if (d->sample_count > MP4_EXPAND_MAX_SAMPLES) {
        if (!chunk_offset_at(d, 0, &d->cursor_offset)) return false;
        d->cursor_index = 0;
        return true;
    }

    uint64_t * chunk_offsets = malloc(sizeof(uint64_t) * (size_t) d->chunk_count);
    if (!chunk_offsets) return false;
    fseek(d->f, d->stco_table_offset, SEEK_SET);
    for (uint32_t i = 0; i < d->chunk_count; i++) {
        uint8_t buf[8];
        size_t n = d->stco_is64 ? 8 : 4;
        if (fread(buf, 1, n, d->f) != n) {
            free(chunk_offsets);
            return false;
        }
        chunk_offsets[i] = d->stco_is64 ? read_u64be(buf) : (uint64_t) read_u32be(buf);
    }

    d->sample_offsets = malloc(sizeof(uint64_t) * (size_t) d->sample_count);
    if (!d->sample_offsets) {
        free(chunk_offsets);
        return false;
    }

    uint32_t sample_index = 0;
    for (uint32_t chunk = 1; chunk <= d->chunk_count && sample_index < d->sample_count; chunk++) {
        uint32_t samples_per_chunk = d->stsc[d->stsc_count - 1].samples_per_chunk;
        for (uint32_t e = 0; e < d->stsc_count; e++) {
            uint32_t range_end = (e + 1 < d->stsc_count) ? d->stsc[e + 1].first_chunk : 0xFFFFFFFFu;
            if (chunk >= d->stsc[e].first_chunk && chunk < range_end) {
                samples_per_chunk = d->stsc[e].samples_per_chunk;
                break;
            }
        }

        uint64_t offset = chunk_offsets[chunk - 1];
        for (uint32_t s = 0; s < samples_per_chunk && sample_index < d->sample_count; s++, sample_index++) {
            uint32_t sz;
            if (!sample_size_at(d, sample_index, &sz)) {
                free(chunk_offsets);
                return false;
            }
            d->sample_offsets[sample_index] = offset;
            offset += sz;
        }
    }

    free(chunk_offsets);
    return sample_index == d->sample_count;
}

static bool parse_stts(mp4_demux_t * d, box_header_t stbl) {
    box_header_t stts_box;
    if (!find_child_box(d->f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stts", &stts_box)) return false;

    uint8_t hdr[8];
    fseek(d->f, stts_box.data_start, SEEK_SET);
    if (fread(hdr, 1, 8, d->f) != 8) return false;
    uint32_t entry_count = read_u32be(hdr + 4);
    if (entry_count == 0) return false;

    uint64_t total = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint8_t entry[8];
        if (fread(entry, 1, 8, d->f) != 8) return false;
        uint32_t sample_count = read_u32be(entry);
        uint32_t sample_delta = read_u32be(entry + 4);
        if (i == 0) d->frames_per_sample = sample_delta; /* first entry covers the vast majority of samples */
        total += (uint64_t) sample_count * sample_delta;
    }

    d->total_pcm_frames = total;
    return d->frames_per_sample > 0 && total > 0;
}

mp4_demux_t * mp4_demux_open(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    box_header_t moov;
    bool found_moov = false;
    while (ftell(f) < file_size) {
        box_header_t box;
        if (!read_box_header(f, &box)) break;
        if (strcmp(box.type, "moov") == 0) {
            moov = box;
            found_moov = true;
            break;
        }
        long next = (long) (box.data_start - box.header_size) + (long) box.size;
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    if (!found_moov) {
        fclose(f);
        return NULL;
    }

    box_header_t trak;
    if (!find_audio_trak(f, moov, &trak)) {
        fclose(f);
        return NULL;
    }

    box_header_t mdia, minf, stbl;
    if (!find_child_box(f, trak.data_start, trak.size - (uint64_t) trak.header_size, "mdia", &mdia) ||
        !find_child_box(f, mdia.data_start, mdia.size - (uint64_t) mdia.header_size, "minf", &minf) ||
        !find_child_box(f, minf.data_start, minf.size - (uint64_t) minf.header_size, "stbl", &stbl)) {
        fclose(f);
        return NULL;
    }

    box_header_t stsd, stsz;
    if (!find_child_box(f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stsd", &stsd) ||
        !find_child_box(f, stbl.data_start, stbl.size - (uint64_t) stbl.header_size, "stsz", &stsz)) {
        fclose(f);
        return NULL;
    }

    mp4_demux_t * d = calloc(1, sizeof(*d));
    if (!d) {
        fclose(f);
        return NULL;
    }
    d->f = f;

    if (!parse_stsd(d, stsd) || !parse_stsz(d, stsz) || !parse_sample_offsets(d, stbl) || !parse_stts(d, stbl)) {
        mp4_demux_close(d);
        return NULL;
    }

    return d;
}

bool mp4_demux_peek_codec(const char * path, char out_fourcc[5]) {
    mp4_demux_t * d = mp4_demux_open(path);
    if (!d) return false;
    memcpy(out_fourcc, d->codec_fourcc, 5);
    mp4_demux_close(d);
    return true;
}

void mp4_demux_get_codec_fourcc(const mp4_demux_t * d, char out_fourcc[5]) {
    memcpy(out_fourcc, d->codec_fourcc, 5);
}

const uint8_t * mp4_demux_get_codec_config(const mp4_demux_t * d, uint32_t * out_size) {
    *out_size = d->codec_config_size;
    return d->codec_config;
}

unsigned int mp4_demux_get_channels(const mp4_demux_t * d) {
    return d->channels;
}

unsigned int mp4_demux_get_sample_rate(const mp4_demux_t * d) {
    return d->sample_rate;
}

uint32_t mp4_demux_get_sample_count(const mp4_demux_t * d) {
    return d->sample_count;
}

uint32_t mp4_demux_get_frames_per_sample(const mp4_demux_t * d) {
    return d->frames_per_sample;
}

uint64_t mp4_demux_get_total_pcm_frame_count(const mp4_demux_t * d) {
    return d->total_pcm_frames;
}

bool mp4_demux_read_sample(mp4_demux_t * d, uint32_t sample_index, uint8_t * buf, uint32_t buf_size, uint32_t * out_size) {
    uint32_t size;
    uint64_t offset;
    if (!sample_size_at(d, sample_index, &size) || !sample_offset_at(d, sample_index, &offset)) return false;
    if (size > buf_size) return false;

    if (fseek(d->f, (long) offset, SEEK_SET) != 0) return false;
    if (fread(buf, 1, size, d->f) != size) return false;

    d->cursor_index = sample_index + 1;
    d->cursor_offset = offset + size;
    *out_size = size;
    return true;
}

void mp4_demux_close(mp4_demux_t * d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    free(d->codec_config);
    free(d->sample_sizes);
    free(d->sample_offsets);
    free(d->stsc);
    free(d);
}
