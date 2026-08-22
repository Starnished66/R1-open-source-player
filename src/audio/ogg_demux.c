#include "ogg_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An Ogg page's segment table has at most 255 entries, each a lacing value
 * 0-255; the largest possible single page is therefore the 27-byte fixed
 * header + a 255-byte segment table + up to 255*255 bytes of payload. Pages
 * are buffered whole (see load_page_at()) rather than streamed segment by
 * segment, so the page's CRC -- which covers the entire page with the
 * checksum field itself zeroed -- can be verified before any of its bytes
 * are handed to a caller. */
#define OGG_MAX_PAGE_SEGMENTS 255
#define OGG_MAX_PAGE_SIZE (27 + OGG_MAX_PAGE_SEGMENTS + OGG_MAX_PAGE_SEGMENTS * 255)

struct ogg_demux {
    FILE * f;
    uint32_t serial_number;

    uint8_t channels;
    uint16_t pre_skip;
    int16_t output_gain_q78;

    char * vendor_string;
    char ** comments;
    uint32_t * comment_lens;
    unsigned int comment_count;

    /* One entry per packet-boundary page (header_type's continued-packet
     * bit clear) belonging to this file -- see ogg_demux_seek_near_granule()
     * for why only packet-boundary pages are indexed. */
    struct {
        long offset;
        uint64_t granule;
    } * page_index;
    uint32_t page_index_count;
    uint64_t total_granule;

    uint32_t crc_table[256];
    uint8_t * page_buf; /* OGG_MAX_PAGE_SIZE bytes, one whole buffered page */

    /* Current page's parse state, consumed segment by segment across calls
     * to next_segment(). segment_index >= segment_count means the current
     * page is exhausted (or none has been loaded yet -- true from a fresh
     * calloc()) and the next read must load next_page_offset first. */
    long current_page_offset;
    uint8_t segment_table[OGG_MAX_PAGE_SEGMENTS];
    int segment_count;
    int segment_index;
    uint32_t payload_cursor;
    uint8_t header_type;
    long next_page_offset;
};

static uint16_t le16(const uint8_t * p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint64_t le64(const uint8_t * p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

ogg_codec_t ogg_detect_codec(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return OGG_CODEC_UNKNOWN;

    uint8_t header[27];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "OggS", 4) != 0 || header[4] != 0 || !(header[5] & 0x02)) {
        fclose(f);
        return OGG_CODEC_UNKNOWN;
    }

    uint8_t segment_count = header[26];
    uint8_t first_lacing = 0;
    if (segment_count == 0 || fread(&first_lacing, 1, 1, f) != 1) {
        fclose(f);
        return OGG_CODEC_UNKNOWN;
    }
    if (segment_count > 1 && fseek(f, segment_count - 1, SEEK_CUR) != 0) {
        fclose(f);
        return OGG_CODEC_UNKNOWN;
    }

    uint8_t signature[8];
    if (first_lacing < sizeof(signature) || fread(signature, 1, sizeof(signature), f) != sizeof(signature)) {
        fclose(f);
        return OGG_CODEC_UNKNOWN;
    }
    fclose(f);

    if (memcmp(signature, "OpusHead", 8) == 0) return OGG_CODEC_OPUS;
    if (signature[0] == 1 && memcmp(signature + 1, "vorbis", 6) == 0) return OGG_CODEC_VORBIS;
    return OGG_CODEC_UNKNOWN;
}

/* Ogg's own CRC-32 variant (RFC 3533 Section 5): polynomial 0x04C11DB7,
 * MSB-first (not reflected), initial value 0, no final XOR -- distinct from
 * the reflected CRC-32 (zlib/ISO-HDLC) used elsewhere in computing; a table
 * built per-instance at open() rather than a shared global avoids any
 * lazy-init race between concurrently open decoders. */
static void ogg_crc_init_table(uint32_t table[256]) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t r = i << 24;
        for (int j = 0; j < 8; j++) {
            r = (r & 0x80000000UL) ? ((r << 1) ^ 0x04C11DB7UL) : (r << 1);
        }
        table[i] = r;
    }
}

static uint32_t ogg_crc32(const uint32_t table[256], const uint8_t * data, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ table[((crc >> 24) & 0xFF) ^ data[i]];
    }
    return crc;
}

/* Reads one whole Ogg page starting at file offset `offset` into
 * d->page_buf, verifies its CRC, and updates the current-page parse state
 * (segment_table/segment_count/segment_index/payload_cursor/header_type/
 * next_page_offset) to the start of that page's first segment. Returns
 * false on a short read, bad capture pattern/version, or CRC mismatch --
 * all treated the same by callers (end of usable stream), since this
 * demuxer doesn't attempt resync past a corrupt page. */
static bool load_page_at(ogg_demux_t * d, long offset) {
    if (fseek(d->f, offset, SEEK_SET) != 0) return false;

    uint8_t hdr[27];
    if (fread(hdr, 1, 27, d->f) != 27) return false;
    if (memcmp(hdr, "OggS", 4) != 0 || hdr[4] != 0) return false;

    uint8_t page_segments = hdr[26];
    uint8_t seg_table[OGG_MAX_PAGE_SEGMENTS];
    if (page_segments > 0 && fread(seg_table, 1, page_segments, d->f) != page_segments) return false;

    uint32_t payload_size = 0;
    for (int i = 0; i < page_segments; i++) payload_size += seg_table[i];
    uint32_t total_size = 27 + page_segments + payload_size;

    memcpy(d->page_buf, hdr, 27);
    memcpy(d->page_buf + 27, seg_table, page_segments);
    if (payload_size > 0 && fread(d->page_buf + 27 + page_segments, 1, payload_size, d->f) != payload_size) return false;

    uint8_t saved_crc[4];
    memcpy(saved_crc, d->page_buf + 22, 4);
    memset(d->page_buf + 22, 0, 4);
    uint32_t computed = ogg_crc32(d->crc_table, d->page_buf, total_size);
    memcpy(d->page_buf + 22, saved_crc, 4);
    if (computed != le32(saved_crc)) return false;

    d->current_page_offset = offset;
    memcpy(d->segment_table, seg_table, page_segments);
    d->segment_count = page_segments;
    d->segment_index = 0;
    d->payload_cursor = 0;
    d->header_type = hdr[5];
    d->next_page_offset = offset + (long) total_size;
    return true;
}

/* Hands back the next lacing segment (length + pointer into d->page_buf),
 * loading a fresh page via load_page_at() whenever the current one is
 * exhausted. This is the single point both the bounded public
 * ogg_demux_read_packet() and the growable header-packet reader below
 * consume pages/segments through. */
static bool next_segment(ogg_demux_t * d, uint8_t * out_len, const uint8_t ** out_data) {
    if (d->segment_index >= d->segment_count) {
        if (!load_page_at(d, d->next_page_offset)) return false;
    }
    uint8_t seg_len = d->segment_table[d->segment_index];
    const uint8_t * seg_data = d->page_buf + 27 + d->segment_count + d->payload_cursor;
    d->segment_index++;
    d->payload_cursor += seg_len;
    *out_len = seg_len;
    *out_data = seg_data;
    return true;
}

/* Growable-buffer packet read, used only for OpusHead/OpusTags during
 * open() -- OpusTags in particular can be arbitrarily large (an embedded
 * METADATA_BLOCK_PICTURE cover image, base64-encoded), so a fixed cap would
 * either reject legitimate files or need guessing at a "big enough" size.
 * Same realloc-into-a-temporary safety pattern aac_decoder.c's
 * prescan_adts() uses (a failed realloc must not lose the caller's only
 * pointer to the still-valid original block). */
static bool read_packet_growable(ogg_demux_t * d, uint8_t ** out_buf, uint32_t * out_size) {
    uint32_t capacity = 4096;
    uint32_t written = 0;
    uint8_t * buf = malloc(capacity);
    if (!buf) return false;

    for (;;) {
        uint8_t seg_len;
        const uint8_t * seg_data;
        if (!next_segment(d, &seg_len, &seg_data)) {
            free(buf);
            return false;
        }

        if (written + seg_len > capacity) {
            uint32_t new_capacity = capacity * 2;
            while (written + seg_len > new_capacity) new_capacity *= 2;
            uint8_t * grown = realloc(buf, new_capacity);
            if (!grown) {
                free(buf);
                return false;
            }
            buf = grown;
            capacity = new_capacity;
        }
        if (seg_len > 0) {
            memcpy(buf + written, seg_data, seg_len);
            written += seg_len;
        }
        if (seg_len < 255) {
            *out_buf = buf;
            *out_size = written;
            return true;
        }
    }
}

static bool parse_opus_head(ogg_demux_t * d, const uint8_t * data, uint32_t size) {
    if (size < 19 || memcmp(data, "OpusHead", 8) != 0) return false;
    uint8_t version = data[8];
    if ((version & 0xF0) != 0) return false; /* incompatible major version, per RFC 7845 5.1 */

    d->channels = data[9];
    d->pre_skip = le16(data + 10);
    d->output_gain_q78 = (int16_t) le16(data + 16);
    uint8_t channel_mapping_family = data[18];

    /* Mapping family 0 (mono/stereo, no extra channel-mapping table) is the
     * only layout this demuxer supports -- matches its single-logical-
     * stream, no-multistream scope. */
    if (channel_mapping_family != 0) return false;
    if (d->channels == 0 || d->channels > 2) return false;
    return true;
}

/* Best-effort: a missing or malformed OpusTags packet leaves vendor_string
 * NULL and comment_count 0, which read_opus_metadata() (metadata.c) already
 * treats the same as any other unhandled/failed tag read -- never fatal to
 * opening the file for playback. */
static void parse_opus_tags(ogg_demux_t * d, const uint8_t * data, uint32_t size) {
    if (size < 8 || memcmp(data, "OpusTags", 8) != 0) return;

    uint32_t pos = 8;
    if (pos + 4 > size) return;
    uint32_t vendor_len = le32(data + pos);
    pos += 4;
    if (pos + vendor_len > size) return;

    d->vendor_string = malloc((size_t) vendor_len + 1);
    if (d->vendor_string) {
        memcpy(d->vendor_string, data + pos, vendor_len);
        d->vendor_string[vendor_len] = '\0';
    }
    pos += vendor_len;

    if (pos + 4 > size) return;
    uint32_t comment_count = le32(data + pos);
    pos += 4;

    /* Bound a corrupt/hostile huge count against what's actually left in
     * the packet (each comment needs at least its own 4-byte length
     * prefix) before allocating its index arrays. */
    uint32_t max_possible = (size - pos) / 4;
    if (comment_count > max_possible) comment_count = max_possible;
    if (comment_count == 0) return;

    d->comments = calloc(comment_count, sizeof(char *));
    d->comment_lens = calloc(comment_count, sizeof(uint32_t));
    if (!d->comments || !d->comment_lens) return;

    unsigned int stored = 0;
    for (uint32_t i = 0; i < comment_count; i++) {
        if (pos + 4 > size) break;
        uint32_t clen = le32(data + pos);
        pos += 4;
        if (pos + clen > size) break;

        char * copy = malloc((size_t) clen + 1);
        if (copy) {
            memcpy(copy, data + pos, clen);
            copy[clen] = '\0';
            d->comments[stored] = copy;
            d->comment_lens[stored] = clen;
            stored++;
        }
        pos += clen;
    }
    d->comment_count = stored;
}

/* Full page-header-only linear scan (segment table read for its length sum,
 * payload bytes skipped rather than read -- same cheap cost class as
 * aac_decoder.c's ADTS-header-only prescan_adts()) building the
 * granule-position seek index and total_granule. Runs once at open() over
 * the whole file; ogg_demux_open() repositions the live read cursor back to
 * the first audio packet afterward, since this scan's own fseek()s move it. */
static bool build_page_index(ogg_demux_t * d) {
    uint32_t capacity = 64;
    d->page_index = malloc(sizeof(*d->page_index) * capacity);
    if (!d->page_index) return false;
    d->page_index_count = 0;

    long offset = 0;
    for (;;) {
        if (fseek(d->f, offset, SEEK_SET) != 0) break;
        uint8_t hdr[27];
        if (fread(hdr, 1, 27, d->f) != 27) break;
        if (memcmp(hdr, "OggS", 4) != 0 || hdr[4] != 0) break;

        uint8_t header_type = hdr[5];
        uint64_t granule = le64(hdr + 6);
        uint8_t page_segments = hdr[26];
        uint8_t seg_table[OGG_MAX_PAGE_SEGMENTS];
        if (page_segments > 0 && fread(seg_table, 1, page_segments, d->f) != page_segments) break;

        uint32_t payload_size = 0;
        for (int i = 0; i < page_segments; i++) payload_size += seg_table[i];

        d->total_granule = granule; /* last page scanned wins -- the EOS page's granule is authoritative regardless of its continued-bit */

        if (!(header_type & 0x01)) { /* packet-boundary page: safe seek target */
            if (d->page_index_count == capacity) {
                uint32_t new_capacity = capacity * 2;
                void * grown = realloc(d->page_index, sizeof(*d->page_index) * new_capacity);
                if (!grown) break; /* keep whatever's indexed so far -- same graceful-degrade shape as prescan_adts */
                d->page_index = grown;
                capacity = new_capacity;
            }
            d->page_index[d->page_index_count].offset = offset;
            d->page_index[d->page_index_count].granule = granule;
            d->page_index_count++;
        }

        long next = offset + (long) (27 + page_segments + payload_size);
        if (next <= offset) break; /* guard against a corrupt zero-size page looping forever */
        offset = next;
    }

    return d->page_index_count > 0;
}

ogg_demux_t * ogg_demux_open(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    ogg_demux_t * d = calloc(1, sizeof(*d));
    if (!d) {
        fclose(f);
        return NULL;
    }
    d->f = f;
    ogg_crc_init_table(d->crc_table);

    d->page_buf = malloc(OGG_MAX_PAGE_SIZE);
    if (!d->page_buf) {
        ogg_demux_close(d);
        return NULL;
    }

    /* Page 0 must be the OpusHead page (BOS set, exactly one packet), per
     * RFC 7845 Section 3 -- no scanning, see ogg_demux.h. */
    if (!load_page_at(d, 0) || !(d->header_type & 0x02)) {
        ogg_demux_close(d);
        return NULL;
    }
    d->serial_number = le32(d->page_buf + 14);

    uint8_t * head_packet;
    uint32_t head_size;
    if (!read_packet_growable(d, &head_packet, &head_size)) {
        ogg_demux_close(d);
        return NULL;
    }
    bool head_ok = parse_opus_head(d, head_packet, head_size);
    free(head_packet);
    if (!head_ok) {
        ogg_demux_close(d);
        return NULL;
    }

    uint8_t * tags_packet;
    uint32_t tags_size;
    if (!read_packet_growable(d, &tags_packet, &tags_size)) {
        ogg_demux_close(d);
        return NULL;
    }
    parse_opus_tags(d, tags_packet, tags_size);
    free(tags_packet);

    /* We're positioned exactly at the first audio packet right now --
     * remember it so build_page_index()'s own file seeking (below) can be
     * undone afterward. */
    long first_audio_page_offset = d->current_page_offset;
    int first_audio_segment_index = d->segment_index;
    uint32_t first_audio_payload_cursor = d->payload_cursor;

    if (!build_page_index(d)) {
        ogg_demux_close(d);
        return NULL;
    }

    if (!load_page_at(d, first_audio_page_offset)) {
        ogg_demux_close(d);
        return NULL;
    }
    d->segment_index = first_audio_segment_index;
    d->payload_cursor = first_audio_payload_cursor;

    return d;
}

unsigned int ogg_demux_get_opus_channels(const ogg_demux_t * d) {
    return d->channels;
}

uint16_t ogg_demux_get_opus_pre_skip(const ogg_demux_t * d) {
    return d->pre_skip;
}

int16_t ogg_demux_get_opus_output_gain_q78(const ogg_demux_t * d) {
    return d->output_gain_q78;
}

uint64_t ogg_demux_get_total_granule(const ogg_demux_t * d) {
    return d->total_granule;
}

const char * ogg_demux_get_vendor_string(const ogg_demux_t * d) {
    return d->vendor_string;
}

unsigned int ogg_demux_get_comment_count(const ogg_demux_t * d) {
    return d->comment_count;
}

const char * ogg_demux_get_comment(const ogg_demux_t * d, unsigned int index, uint32_t * out_len) {
    if (index >= d->comment_count) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = d->comment_lens[index];
    return d->comments[index];
}

uint32_t ogg_demux_read_packet(ogg_demux_t * d, uint8_t * buf, uint32_t buf_size) {
    uint32_t written = 0;
    for (;;) {
        uint8_t seg_len;
        const uint8_t * seg_data;
        if (!next_segment(d, &seg_len, &seg_data)) return 0;

        if (seg_len > 0) {
            if (written + seg_len > buf_size) return 0;
            memcpy(buf + written, seg_data, seg_len);
            written += seg_len;
        }
        if (seg_len < 255) return written;
    }
}

uint64_t ogg_demux_seek_near_granule(ogg_demux_t * d, uint64_t target_granule) {
    if (d->page_index_count == 0) return 0;

    uint32_t lo = 0, hi = d->page_index_count - 1, result = 0;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (d->page_index[mid].granule <= target_granule) {
            result = mid;
            if (mid == d->page_index_count - 1) break;
            lo = mid + 1;
        } else {
            if (mid == 0) break;
            hi = mid - 1;
        }
    }

    d->next_page_offset = d->page_index[result].offset;
    d->segment_index = 0;
    d->segment_count = 0; /* forces next_segment() to load the target page fresh */
    d->payload_cursor = 0;

    return d->page_index[result].granule;
}

void ogg_demux_close(ogg_demux_t * d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    free(d->page_buf);
    free(d->page_index);
    free(d->vendor_string);
    if (d->comments) {
        for (unsigned int i = 0; i < d->comment_count; i++) free(d->comments[i]);
        free(d->comments);
    }
    free(d->comment_lens);
    free(d);
}
