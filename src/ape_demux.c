#include "ape_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t pos;
    uint64_t size;
    uint32_t nblocks;
    uint32_t skip;
} ape_frame_t;

struct ape_demux {
    FILE * f;

    int fileversion;
    uint16_t compressiontype;
    uint16_t formatflags;
    uint32_t blocksperframe;
    uint32_t finalframeblocks;
    uint32_t totalframes;
    uint16_t bps;
    uint16_t channels;
    uint32_t samplerate;

    uint64_t total_samples;
    ape_frame_t * frames;
};

static uint16_t read_u16le(const uint8_t * b) {
    return (uint16_t) (b[0] | (b[1] << 8));
}

static uint32_t read_u32le(const uint8_t * b) {
    return (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
}

ape_demux_t * ape_demux_open(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    uint8_t hdr[4];
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "MAC ", 4) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t buf[36];
    if (fread(buf, 1, 2, f) != 2) { fclose(f); return NULL; }
    int fileversion = read_u16le(buf);
    /* Only the modern (fileversion >= 3980) descriptor+header layout is
     * supported -- see ape_demux.h for why that's not a practical
     * limitation. */
    if (fileversion < 3980 || fileversion > 3990) {
        fclose(f);
        return NULL;
    }

    /* The rest of the fixed 52-byte descriptor (52 = 4 "MAC " + 2
     * fileversion + this 46-byte tail): padding1(2, ignored),
     * descriptorlength(4), headerlength(4), seektablelength(4),
     * wavheaderlength(4), audiodatalength(4), audiodatalength_high(4,
     * ignored), wavtaillength(4), md5(16, ignored). */
    uint8_t desc[46];
    if (fread(desc, 1, 46, f) != 46) { fclose(f); return NULL; }
    uint32_t descriptorlength = read_u32le(desc + 2);
    uint32_t headerlength = read_u32le(desc + 6);
    uint32_t seektablelength = read_u32le(desc + 10);
    uint32_t wavheaderlength = read_u32le(desc + 14);
    uint32_t wavtaillength = read_u32le(desc + 26);

    if (descriptorlength > 52) {
        if (fseek(f, (long) (descriptorlength - 52), SEEK_CUR) != 0) { fclose(f); return NULL; }
    }

    uint8_t hb[24];
    if (fread(hb, 1, 24, f) != 24) { fclose(f); return NULL; }

    ape_demux_t * d = calloc(1, sizeof(*d));
    d->f = f;
    d->fileversion = fileversion;
    d->compressiontype = read_u16le(hb + 0);
    d->formatflags = read_u16le(hb + 2);
    d->blocksperframe = read_u32le(hb + 4);
    d->finalframeblocks = read_u32le(hb + 8);
    d->totalframes = read_u32le(hb + 12);
    d->bps = read_u16le(hb + 16);
    d->channels = read_u16le(hb + 18);
    d->samplerate = read_u32le(hb + 20);

    if (!d->totalframes || d->channels == 0 || d->channels > 2 ||
        (d->bps != 16 && d->bps != 24)) {
        fclose(f);
        free(d);
        return NULL;
    }

    d->total_samples = d->finalframeblocks;
    if (d->totalframes > 1) d->total_samples += (uint64_t) d->blocksperframe * (d->totalframes - 1);

    d->frames = malloc(sizeof(ape_frame_t) * d->totalframes);
    if (!d->frames) {
        fclose(f);
        free(d);
        return NULL;
    }

    long firstframe = (long) descriptorlength + (long) headerlength + (long) seektablelength + (long) wavheaderlength;

    d->frames[0].pos = (uint64_t) firstframe;
    d->frames[0].nblocks = d->blocksperframe;

    uint8_t entry[4];
    if (fread(entry, 1, 4, f) != 4) { /* seektable[0], unused -- frames[0].pos is computed directly */
        ape_demux_close(d);
        return NULL;
    }
    for (uint32_t i = 1; i < d->totalframes; i++) {
        if (fread(entry, 1, 4, f) != 4) { ape_demux_close(d); return NULL; }
        uint32_t seektable_entry = read_u32le(entry);
        d->frames[i].pos = seektable_entry;
        d->frames[i].nblocks = d->blocksperframe;
        d->frames[i - 1].size = d->frames[i].pos - d->frames[i - 1].pos;
        d->frames[i].skip = (uint32_t) ((d->frames[i].pos - d->frames[0].pos) & 3);
    }
    d->frames[0].skip = 0;

    uint32_t seektable_entries_read = d->totalframes;
    uint32_t seektable_total_entries = seektablelength / 4;
    if (seektable_total_entries > seektable_entries_read) {
        if (fseek(f, (long) (seektable_total_entries - seektable_entries_read) * 4, SEEK_CUR) != 0) {
            ape_demux_close(d);
            return NULL;
        }
    }

    d->frames[d->totalframes - 1].nblocks = d->finalframeblocks;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    int64_t final_size = 0;
    if (file_size > 0) {
        final_size = (int64_t) file_size - (int64_t) d->frames[d->totalframes - 1].pos - wavtaillength;
        final_size -= final_size & 3;
    }
    if (file_size <= 0 || final_size <= 0) final_size = (int64_t) d->finalframeblocks * 8;
    d->frames[d->totalframes - 1].size = (uint64_t) final_size;

    for (uint32_t i = 0; i < d->totalframes; i++) {
        if (d->frames[i].skip) {
            d->frames[i].pos -= d->frames[i].skip;
            d->frames[i].size += d->frames[i].skip;
        }
        d->frames[i].size = (d->frames[i].size + 3) & ~(uint64_t) 3;
    }

    return d;
}

unsigned int ape_demux_get_channels(const ape_demux_t * d) { return d->channels; }
unsigned int ape_demux_get_sample_rate(const ape_demux_t * d) { return d->samplerate; }
unsigned int ape_demux_get_bits_per_sample(const ape_demux_t * d) { return d->bps; }
int ape_demux_get_fileversion(const ape_demux_t * d) { return d->fileversion; }
int ape_demux_get_compression_level(const ape_demux_t * d) { return d->compressiontype; }
uint64_t ape_demux_get_total_samples(const ape_demux_t * d) { return d->total_samples; }
uint32_t ape_demux_get_frame_count(const ape_demux_t * d) { return d->totalframes; }
uint32_t ape_demux_get_blocks_per_frame(const ape_demux_t * d) { return d->blocksperframe; }
uint32_t ape_demux_get_final_frame_blocks(const ape_demux_t * d) { return d->finalframeblocks; }

bool ape_demux_read_frame(ape_demux_t * d, uint32_t frame_index, uint8_t * buf, uint32_t buf_size, uint32_t * out_size) {
    if (frame_index >= d->totalframes) return false;
    uint64_t size = d->frames[frame_index].size;
    if (size > buf_size) return false;

    if (fseek(d->f, (long) d->frames[frame_index].pos, SEEK_SET) != 0) return false;
    if (fread(buf, 1, (size_t) size, d->f) != (size_t) size) return false;

    *out_size = (uint32_t) size;
    return true;
}

uint32_t ape_demux_get_frame_skip(const ape_demux_t * d, uint32_t frame_index) {
    if (frame_index >= d->totalframes) return 0;
    return d->frames[frame_index].skip;
}

void ape_demux_close(ape_demux_t * d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    free(d->frames);
    free(d);
}
