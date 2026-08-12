#include "asf_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ASF object-type GUIDs, straight from Microsoft's own published "Advanced
 * Systems Format (ASF) Specification" (a plain container-format identifier,
 * not creative content -- these values are cross-checked against FFmpeg's
 * own asf_tags.c purely as a second reference, not copied logic). */
typedef uint8_t asf_guid_t[16];

static const asf_guid_t GUID_HEADER = {
    0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};
static const asf_guid_t GUID_FILE_PROPERTIES = {
    0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65
};
static const asf_guid_t GUID_STREAM_PROPERTIES = {
    0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11, 0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65
};
static const asf_guid_t GUID_AUDIO_STREAM = {
    0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11, 0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B
};
static const asf_guid_t GUID_DATA = {
    0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};

struct asf_demux {
    FILE * f;

    uint16_t codec_id;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint8_t codec_data[64];
    uint32_t codec_data_size;
    int audio_stream_number; /* -1 until a Stream Properties Object with the audio type GUID is found */

    uint32_t default_packet_size; /* from File Properties' min packet size; used when a packet's own length field defaults to it */
    uint64_t data_packets_total;
    uint64_t data_packets_read;
    uint64_t duration_100ns;
    long first_packet_pos;

    /* Packets are fixed-size slots (default_packet_size, per the File
     * Properties Object) padded out with padsize trailing junk bytes we
     * never read -- so the next packet does NOT begin wherever our own
     * segment-by-segment reads happen to leave the file position. Each
     * packet's true start must be located explicitly. */
    long next_packet_pos;

    /* Current-packet parsing state, persists across asf_demux_read_frame()
     * calls whenever a packet has more payload segments left to process. */
    int packet_segments_remaining;
    uint32_t packet_size_left;
    uint8_t packet_flags;
    uint8_t packet_property;
    uint8_t packet_segsizetype;

    /* Reassembly buffer for a media object fragmented across multiple
     * payload segments/packets (WMA audio objects are one block_align-sized
     * chunk each; fragmentation across packets is rare for audio but the
     * container format allows it generically, so this is handled either
     * way rather than assumed away). */
    uint8_t * partial;
    uint32_t partial_capacity;
    uint32_t partial_filled;
    uint32_t partial_object_size;
    bool partial_active;
};

static bool guid_eq(const uint8_t * a, const asf_guid_t b) {
    return memcmp(a, b, 16) == 0;
}

static uint8_t read_u8(FILE * f) {
    uint8_t b = 0;
    if (fread(&b, 1, 1, f) != 1) return 0;
    return b;
}

static uint16_t read_u16le(FILE * f) {
    uint8_t b[2] = { 0 };
    if (fread(b, 1, 2, f) != 2) return 0;
    return (uint16_t) (b[0] | (b[1] << 8));
}

static uint32_t read_u32le(FILE * f) {
    uint8_t b[4] = { 0 };
    if (fread(b, 1, 4, f) != 4) return 0;
    return (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
}

static uint64_t read_u64le(FILE * f) {
    uint8_t b[8] = { 0 };
    if (fread(b, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

/* Mirrors the DO_2BITS pattern every ASF packet/payload header field uses:
 * a 2-bit selector picks whether the actual field is 0/1/2/4 bytes wide
 * (0 bytes meaning "use defval, nothing follows in the stream"). Adds
 * however many bytes it consumed onto *rsize_accum, matching every ASF
 * demuxer's own byte-accounting so packet/segment boundaries line up. */
static uint32_t read_variable_field(FILE * f, int selector2bits, uint32_t defval, int * rsize_accum) {
    switch (selector2bits & 3) {
        case 3: *rsize_accum += 4; return read_u32le(f);
        case 2: *rsize_accum += 2; return read_u16le(f);
        case 1: *rsize_accum += 1; return read_u8(f);
        default: return defval;
    }
}

typedef struct {
    asf_guid_t guid;
    uint64_t size; /* includes this 24-byte GUID+size prefix itself, per ASF convention */
    long data_start;
} asf_object_header_t;

static bool read_object_header(FILE * f, asf_object_header_t * out) {
    if (fread(out->guid, 1, 16, f) != 16) return false;
    out->size = read_u64le(f);
    out->data_start = ftell(f);
    return out->size >= 24;
}

/* Reads one ASF Data Packet's fixed-position header (Error Correction Data,
 * Packet Flags/Property, timestamp/duration, and the payload segment
 * count), leaving the file position at the first payload segment. Sets up
 * packet_segments_remaining/packet_size_left/packet_flags/packet_property/
 * packet_segsizetype for read_next_segment() to consume. */
static bool read_packet_header(asf_demux_t * d) {
    if (d->data_packets_total && d->data_packets_read >= d->data_packets_total) return false;

    fseek(d->f, d->next_packet_pos, SEEK_SET);
    long packet_start = d->next_packet_pos;

    int rsize = 8; /* fixed baseline: packet_flags(1) + packet_property(1) + timestamp(4) + duration(2) */
    int c = read_u8(d->f);
    if (feof(d->f)) return false;

    int flags_byte;
    if (c & 0x80) {
        /* Error Correction Data present -- 'c' was the ECC flags byte, not
         * the real packet flags. If the "opaque data present" bits are
         * clear, 2 peeked bytes plus any remaining declared ECC length
         * follow and are simply skipped (we don't do error correction). */
        rsize++;
        if ((c & 0x60) == 0) {
            read_u8(d->f);
            read_u8(d->f);
            int extra = (c & 0xF) - 2;
            if (extra > 0) fseek(d->f, extra, SEEK_CUR);
            rsize += (c & 0xF);
        }
        flags_byte = read_u8(d->f);
    } else {
        flags_byte = c;
    }
    int property_byte = read_u8(d->f);

    d->packet_flags = (uint8_t) flags_byte;
    d->packet_property = (uint8_t) property_byte;

    uint32_t packet_length = read_variable_field(d->f, flags_byte >> 5, d->default_packet_size, &rsize);
    read_variable_field(d->f, flags_byte >> 1, 0, &rsize); /* sequence number -- unused */
    uint32_t padsize = read_variable_field(d->f, flags_byte >> 3, 0, &rsize);

    read_u32le(d->f); /* packet send time -- unused, we don't do timestamp-based seeking */
    read_u16le(d->f); /* packet duration -- unused */

    if (flags_byte & 0x01) {
        d->packet_segsizetype = read_u8(d->f);
        rsize++;
        d->packet_segments_remaining = d->packet_segsizetype & 0x3F;
    } else {
        d->packet_segments_remaining = 1;
        d->packet_segsizetype = 0x80;
    }

    if (padsize >= packet_length || (uint32_t) rsize > packet_length - padsize) return false;
    d->packet_size_left = packet_length - padsize - (uint32_t) rsize;
    d->next_packet_pos = packet_start + (long) packet_length;

    d->data_packets_read++;
    return true;
}

/* Reads one payload segment from the packet currently being parsed. If it
 * belongs to a stream other than our chosen audio stream, its bytes are
 * skipped. If it completes a media object (the common case: one payload,
 * no fragmentation, since WMA audio objects are small), the object's bytes
 * land in out_buf and *got_frame is set. If it's one fragment of a larger
 * object split across payloads/packets, the bytes accumulate in the
 * demuxer's own partial-object buffer instead, and the caller should loop
 * to read more segments/packets. Returns false only on an actual parse
 * error (out-of-order fragments are handled by discarding, not failing). */
static bool read_next_segment(asf_demux_t * d, uint8_t * out_buf, uint32_t out_buf_size, uint32_t * out_size,
                               bool * got_frame) {
    *got_frame = false;

    int rsize = 1;
    int num = read_u8(d->f);
    d->packet_segments_remaining--;
    int stream_number = num & 0x7F;

    read_variable_field(d->f, d->packet_property >> 4, 0, &rsize); /* media object number -- unused */
    uint32_t frag_offset = read_variable_field(d->f, d->packet_property >> 2, 0, &rsize);
    uint32_t replic_size = read_variable_field(d->f, d->packet_property, 0, &rsize);

    uint32_t media_object_size = 0;
    bool compressed_mode = false;

    if (replic_size >= 8) {
        media_object_size = read_u32le(d->f);
        read_u32le(d->f); /* presentation time -- unused */
        if (replic_size > 8) fseek(d->f, (long) (replic_size - 8), SEEK_CUR); /* payload extension system data, if any */
        rsize += (int) replic_size;
    } else if (replic_size == 1) {
        /* "Compressed payload" shorthand for packing several small,
         * complete, unfragmented objects back-to-back -- just a 1-byte
         * time delta, no media-object-size/offset bookkeeping needed. */
        compressed_mode = true;
        read_u8(d->f);
        rsize++;
        frag_offset = 0;
    } else if (replic_size != 0) {
        return false;
    }

    uint32_t frag_size;
    if (d->packet_flags & 0x01) {
        frag_size = read_variable_field(d->f, d->packet_segsizetype >> 6, 0, &rsize);
    } else {
        frag_size = d->packet_size_left >= (uint32_t) rsize ? d->packet_size_left - (uint32_t) rsize : 0;
    }

    d->packet_size_left = d->packet_size_left >= (uint32_t) rsize ? d->packet_size_left - (uint32_t) rsize : 0;

    if (stream_number != d->audio_stream_number) {
        fseek(d->f, (long) frag_size, SEEK_CUR);
        return true;
    }

    if (compressed_mode) {
        if (frag_size > out_buf_size) {
            fseek(d->f, (long) frag_size, SEEK_CUR);
            return false;
        }
        if (fread(out_buf, 1, frag_size, d->f) != frag_size) return false;
        *out_size = frag_size;
        *got_frame = true;
        return true;
    }

    if (frag_offset == 0) {
        if (media_object_size > d->partial_capacity) {
            free(d->partial);
            d->partial = malloc(media_object_size);
            d->partial_capacity = d->partial ? media_object_size : 0;
        }
        d->partial_filled = 0;
        d->partial_object_size = media_object_size;
        d->partial_active = d->partial != NULL;
    }

    if (!d->partial_active || frag_offset != d->partial_filled || frag_offset + frag_size > d->partial_capacity) {
        /* Out-of-order/unexpected fragment -- discard defensively rather
         * than risk writing past the buffer or reassembling garbage. */
        fseek(d->f, (long) frag_size, SEEK_CUR);
        d->partial_active = false;
        return true;
    }

    if (fread(d->partial + frag_offset, 1, frag_size, d->f) != frag_size) return false;
    d->partial_filled += frag_size;

    if (d->partial_filled >= d->partial_object_size) {
        if (d->partial_object_size > out_buf_size) {
            d->partial_active = false;
            return false;
        }
        memcpy(out_buf, d->partial, d->partial_object_size);
        *out_size = d->partial_object_size;
        *got_frame = true;
        d->partial_active = false;
    }

    return true;
}

asf_demux_t * asf_demux_open(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;

    asf_object_header_t top;
    if (!read_object_header(f, &top) || !guid_eq(top.guid, GUID_HEADER)) {
        fclose(f);
        return NULL;
    }

    uint32_t num_objects = read_u32le(f);
    fseek(f, 2, SEEK_CUR); /* reserved */

    asf_demux_t * d = calloc(1, sizeof(*d));
    if (!d) {
        fclose(f);
        return NULL;
    }
    d->f = f;
    d->audio_stream_number = -1;

    long pos = ftell(f);
    for (uint32_t i = 0; i < num_objects; i++) {
        fseek(f, pos, SEEK_SET);
        asf_object_header_t sub;
        if (!read_object_header(f, &sub)) break;
        long next = sub.data_start + (long) (sub.size - 24);

        if (guid_eq(sub.guid, GUID_FILE_PROPERTIES)) {
            fseek(f, sub.data_start + 32, SEEK_SET);
            d->data_packets_total = read_u64le(f);
            fseek(f, sub.data_start + 40, SEEK_SET);
            d->duration_100ns = read_u64le(f);
            fseek(f, sub.data_start + 68, SEEK_SET);
            d->default_packet_size = read_u32le(f);
        } else if (guid_eq(sub.guid, GUID_STREAM_PROPERTIES) && d->audio_stream_number < 0) {
            uint8_t stream_type_guid[16];
            fseek(f, sub.data_start, SEEK_SET);
            fread(stream_type_guid, 1, 16, f);
            fseek(f, 16 + 8, SEEK_CUR); /* error_correction_type_guid, time_offset */
            uint32_t type_specific_len = read_u32le(f);
            (void) type_specific_len;
            read_u32le(f); /* error_correction_data_length */
            uint16_t flags = read_u16le(f);
            fseek(f, 4, SEEK_CUR); /* reserved */

            if (guid_eq(stream_type_guid, GUID_AUDIO_STREAM)) {
                d->audio_stream_number = flags & 0x7F;
                d->codec_id = read_u16le(f);
                d->channels = read_u16le(f);
                d->sample_rate = read_u32le(f);
                d->byte_rate = read_u32le(f);
                d->block_align = read_u16le(f);
                d->bits_per_sample = read_u16le(f);
                uint16_t codec_specific_size = read_u16le(f);
                if (codec_specific_size > sizeof(d->codec_data)) codec_specific_size = sizeof(d->codec_data);
                fread(d->codec_data, 1, codec_specific_size, f);
                d->codec_data_size = codec_specific_size;
            }
        }

        pos = next;
    }

    if (d->audio_stream_number < 0 || d->default_packet_size == 0) {
        free(d->partial);
        free(d);
        fclose(f);
        return NULL;
    }

    /* The Data Object is a sibling of Header, immediately following it. */
    long header_end = (long) top.data_start + (long) (top.size - 24);
    fseek(f, header_end, SEEK_SET);
    asf_object_header_t data_obj;
    if (!read_object_header(f, &data_obj) || !guid_eq(data_obj.guid, GUID_DATA)) {
        free(d->partial);
        free(d);
        fclose(f);
        return NULL;
    }
    d->next_packet_pos = data_obj.data_start + 16 /* file id */ + 8 /* total data packets */ + 2 /* reserved */;
    d->first_packet_pos = d->next_packet_pos;

    return d;
}

uint16_t asf_demux_get_codec_id(const asf_demux_t * d) { return d->codec_id; }
unsigned int asf_demux_get_channels(const asf_demux_t * d) { return d->channels; }
unsigned int asf_demux_get_sample_rate(const asf_demux_t * d) { return d->sample_rate; }
uint32_t asf_demux_get_byte_rate(const asf_demux_t * d) { return d->byte_rate; }
uint16_t asf_demux_get_block_align(const asf_demux_t * d) { return d->block_align; }
uint16_t asf_demux_get_bits_per_sample(const asf_demux_t * d) { return d->bits_per_sample; }

const uint8_t * asf_demux_get_codec_data(const asf_demux_t * d, uint32_t * out_size) {
    *out_size = d->codec_data_size;
    return d->codec_data;
}

uint64_t asf_demux_get_duration_100ns(const asf_demux_t * d) { return d->duration_100ns; }
uint64_t asf_demux_get_packet_count(const asf_demux_t * d) { return d->data_packets_total; }

void asf_demux_seek_to_packet(asf_demux_t * d, uint64_t packet_index) {
    if (d->data_packets_total && packet_index >= d->data_packets_total) packet_index = d->data_packets_total - 1;
    d->next_packet_pos = d->first_packet_pos + (long) (packet_index * d->default_packet_size);
    d->data_packets_read = packet_index;
    d->packet_segments_remaining = 0;
    d->partial_active = false;
    d->partial_filled = 0;
    clearerr(d->f);
}

bool asf_demux_read_frame(asf_demux_t * d, uint8_t * buf, uint32_t buf_size, uint32_t * out_size) {
    for (;;) {
        if (d->packet_segments_remaining <= 0) {
            if (!read_packet_header(d)) return false;
        }

        bool got_frame = false;
        if (!read_next_segment(d, buf, buf_size, out_size, &got_frame)) return false;
        if (got_frame) return true;
        if (feof(d->f)) return false;
    }
}

void asf_demux_close(asf_demux_t * d) {
    if (!d) return;
    if (d->f) fclose(d->f);
    free(d->partial);
    free(d);
}
