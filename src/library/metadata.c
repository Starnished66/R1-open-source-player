#include "metadata.h"

#include "dr_flac.h"
#include "dr_wav.h"
#include "ogg_demux.h"
#include "mbedtls/base64.h"

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

static void copy_bounded(char * dst, size_t dst_size, const char * src, size_t src_len) {
    if (src_len >= dst_size) src_len = dst_size - 1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

/* Generic Vorbis-Comment "KEY=VALUE" field matching -- shared by FLAC's
 * VORBIS_COMMENT block (below, via dr_flac's own iterator) and Opus's
 * OpusTags (read_opus_metadata(), same comment-list layout per RFC 7845
 * 5.2). Only the iteration mechanism differs between formats; this matching
 * logic has no dr_flac-specific coupling. */
static void apply_vorbis_comment_field(track_metadata_t * out, const char * comment, size_t comment_len) {
    const char * eq = memchr(comment, '=', comment_len);
    if (!eq) return;

    size_t key_len = (size_t) (eq - comment);
    const char * value = eq + 1;
    size_t value_len = comment_len - key_len - 1;

    if (key_len == 5 && strncasecmp(comment, "TITLE", 5) == 0) {
        copy_bounded(out->title, sizeof(out->title), value, value_len);
        out->has_title = true;
    } else if (key_len == 6 && strncasecmp(comment, "ARTIST", 6) == 0) {
        copy_bounded(out->artist, sizeof(out->artist), value, value_len);
        out->has_artist = true;
    } else if (key_len == 5 && strncasecmp(comment, "ALBUM", 5) == 0) {
        copy_bounded(out->album, sizeof(out->album), value, value_len);
        out->has_album = true;
    } else if (key_len == 11 && strncasecmp(comment, "ALBUMARTIST", 11) == 0) {
        copy_bounded(out->album_artist, sizeof(out->album_artist), value, value_len);
        out->has_album_artist = true;
    } else if (key_len == 5 && strncasecmp(comment, "GENRE", 5) == 0) {
        copy_bounded(out->genre, sizeof(out->genre), value, value_len);
        out->has_genre = true;
    } else if (key_len == 21 && strncasecmp(comment, "REPLAYGAIN_TRACK_GAIN", 21) == 0) {
        char value_buf[32];
        copy_bounded(value_buf, sizeof(value_buf), value, value_len);
        out->replaygain_gain_db = strtod(value_buf, NULL); /* strtod stops at the trailing " dB", no need to strip it */
        out->has_replaygain = true;
    } else if (key_len == 21 && strncasecmp(comment, "REPLAYGAIN_TRACK_PEAK", 21) == 0) {
        char value_buf[32];
        copy_bounded(value_buf, sizeof(value_buf), value, value_len);
        out->replaygain_peak = strtod(value_buf, NULL);
        out->has_replaygain_peak = true;
    }
}

/* Parses a METADATA_BLOCK_PICTURE structure (https://xiph.org/flac/format.html#metadata_block_picture,
 * also used verbatim -- just base64-wrapped as an Ogg/Opus comment value --
 * by Xiph's own "PICTURE" tag convention for Vorbis-Comment-based formats):
 * a fixed run of big-endian fields (type, then length-prefixed MIME string,
 * length-prefixed description, width/height/depth/color-count) followed by
 * the length-prefixed image bytes themselves. dr_flac parses this same
 * layout internally for native FLAC picture blocks (exposed directly via
 * meta->data.picture in flac_meta_cb() above), but that parsing isn't
 * reusable source code -- this is a from-scratch equivalent for the
 * base64-decoded bytes the Opus path hands it, keeping only the final
 * image bytes since track_metadata_t has no fields for the others. */
static void parse_flac_picture_block(const uint8_t * data, size_t size, track_metadata_t * out) {
    if (out->picture_data != NULL) return; /* keep the first picture found, same as FLAC/MP3/M4A */

    size_t pos = 0;
    if (pos + 4 > size) return;
    pos += 4; /* picture type -- not discriminated on, matching FLAC/MP3/M4A's "first picture wins" */

    if (pos + 4 > size) return;
    uint32_t mime_len = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16) | ((uint32_t) data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (pos + mime_len > size) return;
    pos += mime_len;

    if (pos + 4 > size) return;
    uint32_t desc_len = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16) | ((uint32_t) data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (pos + desc_len > size) return;
    pos += desc_len;

    if (pos + 16 > size) return; /* width, height, depth, color-count -- 4 bytes each */
    pos += 16;

    if (pos + 4 > size) return;
    uint32_t pic_data_len = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16) | ((uint32_t) data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (pic_data_len == 0 || pos + pic_data_len > size) return;

    uint8_t * copy = malloc(pic_data_len);
    if (!copy) return;
    memcpy(copy, data + pos, pic_data_len);
    out->picture_data = copy;
    out->picture_size = pic_data_len;
}

/* ---- FLAC: VORBIS_COMMENT block ---- */

static void flac_meta_cb(void * user_data, drflac_metadata * meta) {
    track_metadata_t * out = (track_metadata_t *) user_data;

    if (meta->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
        /* dr_flac frees its own copy of pPictureData right after this
         * callback returns, so it has to be copied here, not just
         * pointed to. */
        if (out->picture_data == NULL && meta->data.picture.pictureDataSize > 0) {
            out->picture_data = malloc(meta->data.picture.pictureDataSize);
            if (out->picture_data) {
                memcpy(out->picture_data, meta->data.picture.pPictureData, meta->data.picture.pictureDataSize);
                out->picture_size = meta->data.picture.pictureDataSize;
            }
        }
        return;
    }

    if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;

    drflac_vorbis_comment_iterator iter;
    drflac_init_vorbis_comment_iterator(&iter, meta->data.vorbis_comment.commentCount, meta->data.vorbis_comment.pComments);

    drflac_uint32 comment_len;
    const char * comment;
    while ((comment = drflac_next_vorbis_comment(&iter, &comment_len)) != NULL) {
        apply_vorbis_comment_field(out, comment, comment_len);
    }
}

static void read_flac_metadata(const char * path, track_metadata_t * out) {
    drflac * flac = drflac_open_file_with_metadata(path, flac_meta_cb, out, NULL);
    if (flac) drflac_close(flac);
}

/* ---- WAV: RIFF LIST/INFO chunk (dr_wav parses this natively) ---- */

static void read_wav_metadata(const char * path, track_metadata_t * out) {
    drwav wav;
    if (!drwav_init_file_with_metadata(&wav, path, drwav_metadata_type_list_all_info_strings, NULL)) return;

    for (drwav_uint32 i = 0; i < wav.metadataCount; i++) {
        drwav_metadata * meta = &wav.pMetadata[i];
        switch (meta->type) {
            case drwav_metadata_type_list_info_title:
                copy_bounded(out->title, sizeof(out->title), meta->data.infoText.pString, meta->data.infoText.stringLength);
                out->has_title = true;
                break;
            case drwav_metadata_type_list_info_artist:
                copy_bounded(out->artist, sizeof(out->artist), meta->data.infoText.pString, meta->data.infoText.stringLength);
                out->has_artist = true;
                break;
            case drwav_metadata_type_list_info_album:
                copy_bounded(out->album, sizeof(out->album), meta->data.infoText.pString, meta->data.infoText.stringLength);
                out->has_album = true;
                break;
            default:
                break;
        }
    }

    drwav_uninit(&wav);
}

/* ---- MP3: ID3v2 (falling back to ID3v1) -- dr_mp3 has no tag support ---- */

static uint32_t read_be32(const uint8_t * b, bool synchsafe) {
    if (synchsafe) {
        return ((uint32_t) (b[0] & 0x7F) << 21) | ((uint32_t) (b[1] & 0x7F) << 14) |
               ((uint32_t) (b[2] & 0x7F) << 7) | (uint32_t) (b[3] & 0x7F);
    }
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | (uint32_t) b[3];
}

/* Decodes an ID3v2 text frame body (1 encoding byte + text) into UTF-8.
 * Handles ISO-8859-1/UTF-8 (copied as-is) and UTF-16/UTF-16BE (converted,
 * including surrogate pairs) -- the encodings actually used in practice.
 * Unsynchronization is not undone; this covers the common case (tags
 * written by mainstream taggers), not every ID3v2 edge case. */
static void decode_id3v2_text_frame(const uint8_t * data, uint32_t size, char * out, size_t out_size) {
    out[0] = '\0';
    if (size == 0) return;

    uint8_t encoding = data[0];
    const uint8_t * text = data + 1;
    uint32_t text_len = size - 1;

    if (encoding == 0x00 || encoding == 0x03) {
        copy_bounded(out, out_size, (const char *) text, text_len);
        return;
    }

    /* UTF-16 with BOM (0x01) or UTF-16BE without BOM (0x02) */
    bool big_endian = (encoding == 0x02);
    size_t src = 0;
    if (encoding == 0x01 && text_len >= 2) {
        if (text[0] == 0xFF && text[1] == 0xFE) { big_endian = false; src = 2; }
        else if (text[0] == 0xFE && text[1] == 0xFF) { big_endian = true; src = 2; }
    }

    size_t out_pos = 0;
    while (src + 1 < text_len && out_pos + 4 < out_size) {
        uint16_t unit = big_endian ? (uint16_t) ((text[src] << 8) | text[src + 1])
                                   : (uint16_t) ((text[src + 1] << 8) | text[src]);
        src += 2;
        if (unit == 0) break;

        uint32_t codepoint = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && src + 1 < text_len) {
            uint16_t unit2 = big_endian ? (uint16_t) ((text[src] << 8) | text[src + 1])
                                        : (uint16_t) ((text[src + 1] << 8) | text[src]);
            if (unit2 >= 0xDC00 && unit2 <= 0xDFFF) {
                codepoint = 0x10000 + (((uint32_t) unit - 0xD800) << 10) + (unit2 - 0xDC00);
                src += 2;
            }
        }

        if (codepoint < 0x80) {
            out[out_pos++] = (char) codepoint;
        } else if (codepoint < 0x800) {
            out[out_pos++] = (char) (0xC0 | (codepoint >> 6));
            out[out_pos++] = (char) (0x80 | (codepoint & 0x3F));
        } else if (codepoint < 0x10000) {
            out[out_pos++] = (char) (0xE0 | (codepoint >> 12));
            out[out_pos++] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
            out[out_pos++] = (char) (0x80 | (codepoint & 0x3F));
        } else {
            out[out_pos++] = (char) (0xF0 | (codepoint >> 18));
            out[out_pos++] = (char) (0x80 | ((codepoint >> 12) & 0x3F));
            out[out_pos++] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
            out[out_pos++] = (char) (0x80 | (codepoint & 0x3F));
        }
    }
    out[out_pos] = '\0';
}

/* Scans forward from `start` for the null terminator appropriate to an
 * ID3v2 text encoding byte (1-byte null for Latin-1/UTF-8, 2-byte null for
 * UTF-16/UTF-16BE) and returns the position right after it, or 0 if none
 * is found before `size` (0 is never a valid return otherwise, since
 * start is always >= 1 in every caller). */
static uint32_t id3v2_terminated_field_end(const uint8_t * data, uint32_t size, uint32_t start, uint8_t encoding) {
    bool wide_terminator = (encoding == 0x01 || encoding == 0x02);
    uint32_t pos = start;
    while (pos < size) {
        if (!wide_terminator && data[pos] == '\0') return pos + 1;
        if (wide_terminator && pos + 1 < size && data[pos] == '\0' && data[pos + 1] == '\0') return pos + 2;
        pos += wide_terminator ? 2 : 1;
    }
    return 0;
}

/* APIC frame body: 1 byte text encoding (applies to the description field
 * only -- MIME type is always Latin-1 regardless), null-terminated MIME
 * type string, 1 byte picture type, a description string terminated per
 * the encoding byte (1-byte null for Latin-1/UTF-8, 2-byte null for
 * UTF-16), then the raw picture bytes to the end of the frame. */
static void decode_id3v2_apic_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (out->picture_data != NULL || size < 2) return; /* keep the first picture found, same as FLAC */

    uint8_t encoding = data[0];
    uint32_t pos = 1;

    const uint8_t * mime_end = memchr(data + pos, '\0', size - pos);
    if (!mime_end) return;
    pos = (uint32_t) (mime_end - data) + 1;

    if (pos >= size) return;
    pos += 1; /* picture type byte */

    uint32_t desc_start = pos;
    pos = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (pos == 0 || pos <= desc_start || pos > size) return; /* malformed -- no terminator found */

    uint32_t picture_size = size - pos;
    if (picture_size == 0) return;

    out->picture_data = malloc(picture_size);
    if (out->picture_data) {
        memcpy(out->picture_data, data + pos, picture_size);
        out->picture_size = picture_size;
    }
}

/* TXXX frame body: 1 byte text encoding, a description string terminated
 * per the encoding byte, then a value string (encoded the same way) running
 * to the end of the frame with no terminator required. Taggers that write
 * ReplayGain into ID3v2 (rather than APEv2 or the binary RVA2 frame) use
 * this with descriptions "REPLAYGAIN_TRACK_GAIN"/"REPLAYGAIN_TRACK_PEAK". */
static void decode_id3v2_txxx_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (size < 2) return;

    uint8_t encoding = data[0];
    uint32_t desc_start = 1;
    uint32_t value_start = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (value_start == 0 || value_start > size) return;

    /* decode_id3v2_text_frame() expects data[0] to be the encoding byte,
     * so re-prefix each field into a small stack buffer to reuse it for
     * both the description and the value. */
    uint8_t desc_buf[64];
    uint32_t desc_len = value_start - desc_start;
    if (desc_len > sizeof(desc_buf) - 1) desc_len = sizeof(desc_buf) - 1;
    desc_buf[0] = encoding;
    memcpy(desc_buf + 1, data + desc_start, desc_len);

    char description[64];
    decode_id3v2_text_frame(desc_buf, desc_len + 1, description, sizeof(description));

    bool is_gain = strcasecmp(description, "REPLAYGAIN_TRACK_GAIN") == 0;
    bool is_peak = strcasecmp(description, "REPLAYGAIN_TRACK_PEAK") == 0;
    if (!is_gain && !is_peak) return;

    uint32_t value_len = size - value_start;
    uint8_t value_buf[40];
    uint32_t copy_len = value_len;
    if (copy_len > sizeof(value_buf) - 1) copy_len = sizeof(value_buf) - 1;
    value_buf[0] = encoding;
    memcpy(value_buf + 1, data + value_start, copy_len);

    char value_str[40];
    decode_id3v2_text_frame(value_buf, copy_len + 1, value_str, sizeof(value_str));

    if (is_gain) {
        out->replaygain_gain_db = strtod(value_str, NULL);
        out->has_replaygain = true;
    } else {
        out->replaygain_peak = strtod(value_str, NULL);
        out->has_replaygain_peak = true;
    }
}

/* v2.2 PIC frame body: 1 byte text encoding, a FIXED 3-character image
 * format code (not a null-terminated MIME string like APIC's -- e.g.
 * "JPG"/"PNG", always exactly 3 bytes, no terminator), 1 byte picture
 * type, a description string terminated per the encoding byte, then the
 * raw picture bytes to the end of the frame. */
static void decode_id3v2_pic_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (out->picture_data != NULL || size < 5) return; /* keep the first picture found, same as APIC */

    uint8_t encoding = data[0];
    uint32_t pos = 1 + 3; /* encoding byte + fixed 3-char image format code */
    pos += 1; /* picture type byte */
    if (pos >= size) return;

    uint32_t desc_start = pos;
    pos = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (pos == 0 || pos <= desc_start || pos > size) return; /* malformed -- no terminator found */

    uint32_t picture_size = size - pos;
    if (picture_size == 0) return;

    out->picture_data = malloc(picture_size);
    if (out->picture_data) {
        memcpy(out->picture_data, data + pos, picture_size);
        out->picture_size = picture_size;
    }
}

static bool read_id3v2(FILE * f, track_metadata_t * out) {
    uint8_t header[10];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) return false;
    if (memcmp(header, "ID3", 3) != 0) return false;

    uint8_t major_version = header[3];
    uint8_t flags = header[5];
    uint32_t tag_size = read_be32(&header[6], true); /* overall tag size is always synchsafe */

    uint8_t * tag_data = malloc(tag_size);
    if (!tag_data) return false;
    if (fread(tag_data, 1, tag_size, f) != tag_size) {
        free(tag_data);
        return false;
    }

    /* Real-device bug report: album art missing entirely from some
     * libraries, and corrupted/pixelated on larger images from others.
     * Root cause -- the tag header's own unsynchronization flag (bit 0x80
     * of the flags byte, header[5]) was never checked. When a tagger sets
     * it (an ID3v2.3+ mechanism that inserts a spurious 0x00 after every
     * 0xFF byte in the tag body, so a naive MP3 player scanning raw bytes
     * for a false sync signal (0xFF Ex) can't mistake tag content for the
     * start of an audio frame), every frame's payload needs those inserted
     * 0x00s stripped back out before it means anything. Plain text frames
     * (title/artist/...) rarely contain a raw 0xFF byte, so this went
     * unnoticed there -- but a JPEG bytestream is full of 0xFF marker
     * bytes (SOI/EOI/DHT/DQT/...), so an unsynchronized tag's embedded
     * APIC picture data was corrupted by scattered stray 0x00 bytes almost
     * every time, either failing to decode at all (in the "missing
     * entirely" library) or partially decoding with visible artifacts (the
     * "pixelated/corrupted" one) depending on where the corruption landed
     * relative to the JPEG's own structure. Fixed by de-unsynchronizing
     * the whole tag body in place, in one pass, before any frame is parsed
     * -- same standard fix every real ID3v2 reader applies, done once here
     * rather than per-frame since it only matters for the tag-level flag.
     * See further down for the rarer per-frame variant, now also handled. */
    if (flags & 0x80) {
        uint32_t write_pos = 0;
        for (uint32_t read_pos = 0; read_pos < tag_size; read_pos++) {
            uint8_t b = tag_data[read_pos];
            tag_data[write_pos++] = b;
            if (b == 0xFF && read_pos + 1 < tag_size && tag_data[read_pos + 1] == 0x00) {
                read_pos++; /* skip the inserted 0x00 */
            }
        }
        tag_size = write_pos;
    }

    uint32_t body_start = 0;

    /* Real-device bug report (persisting after the tag-level unsync fix
     * above): an ID3v2.3/2.4 tag can carry an optional extended header
     * (flags bit 0x40) between the 10-byte tag header and the first real
     * frame -- some taggers (certain foobar2000/Mp3tag configurations,
     * e.g. writing a CRC) include one. Left unskipped, its bytes get
     * misread as a bogus frame ID + frame size, which either desyncs
     * every frame boundary for the rest of the tag or trips the frame
     * loop's own bounds check and aborts it immediately -- silently
     * dropping every frame, title/artist/APIC alike. Sizes differ by spec
     * version: ID3v2.4's extended header size field includes itself (skip
     * exactly that many bytes); ID3v2.3's excludes it (skip 4 more, for
     * the size field itself). Doesn't apply to ID3v2.2 -- its flags byte
     * has no extended-header bit at all (only unsynchronisation and a
     * near-never-used whole-tag compression bit), so this is guarded to
     * major_version >= 3. */
    if (major_version >= 3 && (flags & 0x40) && tag_size >= 4) {
        uint32_t ext_size = read_be32(tag_data, major_version >= 4);
        uint32_t ext_total = (major_version >= 4) ? ext_size : (ext_size + 4);
        if (ext_total <= tag_size) body_start = ext_total;
    }

    bool found_any = false;

    if (major_version <= 2) {
        /* Real-device bug report: ID3v2.2 tags (written by older tools --
         * early iTunes, old EasyTAG/Winamp versions) use 3-character frame
         * IDs and 6-byte frame headers (3-char ID + 3-byte non-synchsafe
         * size), with no per-frame flags at all -- structurally different
         * from ID3v2.3/2.4's 10-byte headers the loop below assumes.
         * Running that parser against a v2.2 tag misreads every frame
         * boundary starting with the very first frame, typically finding
         * nothing at all (not just missing art -- title/artist too).
         * PIC (the v2.2 picture frame) also has a different body layout
         * than APIC -- see decode_id3v2_pic_frame()'s own comment. */
        uint32_t pos = body_start;
        while (pos + 6 <= tag_size) {
            if (tag_data[pos] == '\0') break;

            char frame_id[4];
            memcpy(frame_id, tag_data + pos, 3);
            frame_id[3] = '\0';
            uint32_t frame_size =
                ((uint32_t) tag_data[pos + 3] << 16) | ((uint32_t) tag_data[pos + 4] << 8) | tag_data[pos + 5];
            pos += 6;
            if (pos + frame_size > tag_size) break;

            if (strcmp(frame_id, "TT2") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->title, sizeof(out->title));
                out->has_title = out->title[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TP1") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->artist, sizeof(out->artist));
                out->has_artist = out->artist[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TAL") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->album, sizeof(out->album));
                out->has_album = out->album[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TP2") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->album_artist, sizeof(out->album_artist));
                out->has_album_artist = out->album_artist[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TCO") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->genre, sizeof(out->genre));
                out->has_genre = out->genre[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "PIC") == 0) {
                decode_id3v2_pic_frame(tag_data + pos, frame_size, out);
                found_any = true;
            }

            pos += frame_size;
        }

        free(tag_data);
        return found_any;
    }

    bool frame_size_synchsafe = (major_version >= 4);
    uint32_t pos = body_start;

    while (pos + 10 <= tag_size) {
        if (tag_data[pos] == '\0') break; /* start of padding */

        char frame_id[5];
        memcpy(frame_id, tag_data + pos, 4);
        frame_id[4] = '\0';
        uint32_t frame_size = read_be32(tag_data + pos + 4, frame_size_synchsafe);
        uint8_t frame_flags2 = tag_data[pos + 9];
        pos += 10;
        if (pos + frame_size > tag_size) break;
        uint32_t next_pos = pos + frame_size; /* captured now -- frame_size may still be adjusted below */

        /* Real-device bug report (persisting after the tag-level unsync fix
         * above): the two per-frame flag bytes were read but never
         * interpreted. ID3v2.4 adds a *per-frame* unsynchronisation bit
         * independent of the tag-level one already handled above -- a
         * tagger can leave the tag-level flag off and still set it only on
         * the APIC frame -- plus an optional grouping-identity byte and a
         * data-length-indicator field prepended to the frame body, and
         * compression/encryption bits this codebase has no decoder for.
         * Left unhandled, any of these silently feeds the wrong bytes (or
         * genuinely compressed/encrypted ones) into the APIC decoder as if
         * they were raw image data -- exactly the "still pixelated/
         * corrupted" symptom reported after the tag-level fix alone. Frame
         * flag byte layout differs between v2.3 and v2.4 (same two byte
         * positions, different bit meanings); v2.3 has no per-frame unsync
         * bit (added in v2.4) and no separate data-length-indicator bit (a
         * compressed v2.3 frame always carries that 4-byte prefix
         * unconditionally, per spec). Compressed/encrypted frames are
         * skipped entirely (not fed to any decoder) rather than risking
         * garbage output. */
        bool frame_compressed, frame_encrypted, frame_grouped, frame_unsync, frame_has_data_len;
        if (major_version >= 4) {
            frame_compressed = (frame_flags2 & 0x08) != 0;
            frame_encrypted = (frame_flags2 & 0x04) != 0;
            frame_grouped = (frame_flags2 & 0x40) != 0;
            frame_unsync = (frame_flags2 & 0x02) != 0;
            frame_has_data_len = (frame_flags2 & 0x01) != 0;
        } else {
            frame_compressed = (frame_flags2 & 0x80) != 0;
            frame_encrypted = (frame_flags2 & 0x40) != 0;
            frame_grouped = (frame_flags2 & 0x20) != 0;
            frame_unsync = false;
            frame_has_data_len = frame_compressed;
        }

        if (!frame_compressed && !frame_encrypted) {
            uint8_t * frame_data = tag_data + pos;
            uint32_t frame_data_size = frame_size;

            if (frame_grouped && frame_data_size >= 1) {
                frame_data += 1;
                frame_data_size -= 1;
            }
            if (frame_has_data_len && frame_data_size >= 4) {
                frame_data += 4;
                frame_data_size -= 4;
            }

            if (frame_unsync && frame_data_size > 0) {
                uint32_t write_pos = 0;
                for (uint32_t read_pos = 0; read_pos < frame_data_size; read_pos++) {
                    uint8_t b = frame_data[read_pos];
                    frame_data[write_pos++] = b;
                    if (b == 0xFF && read_pos + 1 < frame_data_size && frame_data[read_pos + 1] == 0x00) {
                        read_pos++;
                    }
                }
                frame_data_size = write_pos;
            }

            if (strcmp(frame_id, "TIT2") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->title, sizeof(out->title));
                out->has_title = out->title[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TPE1") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->artist, sizeof(out->artist));
                out->has_artist = out->artist[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TALB") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->album, sizeof(out->album));
                out->has_album = out->album[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TPE2") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->album_artist, sizeof(out->album_artist));
                out->has_album_artist = out->album_artist[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TCON") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->genre, sizeof(out->genre));
                out->has_genre = out->genre[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "APIC") == 0) {
                decode_id3v2_apic_frame(frame_data, frame_data_size, out);
                found_any = true;
            } else if (strcmp(frame_id, "TXXX") == 0) {
                decode_id3v2_txxx_frame(frame_data, frame_data_size, out);
                found_any = true;
            }
        }

        pos = next_pos;
    }

    free(tag_data);
    return found_any;
}

static void copy_id3v1_field(char * dst, size_t dst_size, const char * src, size_t src_len) {
    while (src_len > 0 && (src[src_len - 1] == ' ' || src[src_len - 1] == '\0')) src_len--;
    copy_bounded(dst, dst_size, src, src_len);
}

static bool read_id3v1(FILE * f, track_metadata_t * out) {
    if (fseek(f, -128, SEEK_END) != 0) return false;

    uint8_t tag[128];
    if (fread(tag, 1, sizeof(tag), f) != sizeof(tag)) return false;
    if (memcmp(tag, "TAG", 3) != 0) return false;

    copy_id3v1_field(out->title, sizeof(out->title), (const char *) tag + 3, 30);
    copy_id3v1_field(out->artist, sizeof(out->artist), (const char *) tag + 33, 30);
    copy_id3v1_field(out->album, sizeof(out->album), (const char *) tag + 63, 30);
    out->has_title = out->title[0] != '\0';
    out->has_artist = out->artist[0] != '\0';
    out->has_album = out->album[0] != '\0';
    return true;
}

static void read_mp3_metadata(const char * path, track_metadata_t * out) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    if (!read_id3v2(f, out)) {
        read_id3v1(f, out);
    }

    fclose(f);
}

/* Real-device bug report: album art (and title/artist/album) missing
 * entirely for raw AAC (.aac, ADTS/ADIF bitstream, as opposed to .m4a's
 * MP4 container) files -- metadata_read()'s dispatch below had no branch
 * for this extension at all, so every .aac file silently got a fully
 * zeroed track_metadata_t regardless of what tags it actually carried.
 * Raw AAC files are commonly tagged the exact same way MP3s are -- a
 * prepended ID3v2 tag (and/or a trailing ID3v1 one) sitting directly in
 * front of/behind the raw ADTS frames, which the tagging software doesn't
 * need to know or care is AAC rather than MP3 -- so this reuses
 * read_id3v2()/read_id3v1() verbatim rather than needing any new parser. */
static void read_aac_metadata(const char * path, track_metadata_t * out) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    if (!read_id3v2(f, out)) {
        read_id3v1(f, out);
    }

    fclose(f);
}

/* ---- M4A (ALAC or AAC container): moov/udta/meta/ilst iTunes-style atoms ----
 *
 * This is a separate, minimal box walker rather than a reuse of
 * mp4_demux.c's (audio.c's demuxer only tracks the moov/trak/.../stbl path
 * needed to locate compressed audio samples, has no reason to know about
 * udta/meta/ilst, and its helpers are file-local statics anyway). */

typedef struct {
    char type[5];
    uint64_t size;
    long header_size;
    long data_start;
} m4a_box_t;

static uint32_t m4a_read_u32be(const uint8_t * b) {
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | (uint32_t) b[3];
}

static bool m4a_read_box_header(FILE * f, m4a_box_t * out) {
    long start = ftell(f);
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;

    uint32_t size32 = m4a_read_u32be(hdr);
    memcpy(out->type, hdr + 4, 4);
    out->type[4] = '\0';

    if (size32 == 1) {
        uint8_t ext[8];
        if (fread(ext, 1, sizeof(ext), f) != sizeof(ext)) return false;
        uint64_t size64 = 0;
        for (int i = 0; i < 8; i++) size64 = (size64 << 8) | ext[i];
        out->size = size64;
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

static bool m4a_find_child(FILE * f, long container_start, uint64_t container_size, const char * type, m4a_box_t * out) {
    long end = container_start + (long) container_size;
    fseek(f, container_start, SEEK_SET);

    while (ftell(f) < end) {
        m4a_box_t box;
        if (!m4a_read_box_header(f, &box)) return false;
        if (strcmp(box.type, type) == 0) {
            *out = box;
            return true;
        }
        long next = (long) (box.data_start - box.header_size) + (long) box.size;
        if (fseek(f, next, SEEK_SET) != 0) return false;
    }
    return false;
}

/* Every ilst item (©nam/©ART/©alb/covr/...) wraps its actual value in a
 * nested "data" box: a FullBox-shaped header (type indicator(4) + locale(4)
 * -- always seen as zeroed/type 1 or 13/14 in practice, not otherwise
 * inspected here) followed immediately by the raw value bytes. */
static bool m4a_find_data_payload(FILE * f, m4a_box_t parent, long * out_offset, uint32_t * out_size) {
    m4a_box_t data_box;
    if (!m4a_find_child(f, parent.data_start, parent.size - (uint64_t) parent.header_size, "data", &data_box)) return false;
    if (data_box.size < (uint64_t) data_box.header_size + 8) return false;
    *out_offset = data_box.data_start + 8;
    *out_size = (uint32_t) (data_box.size - (uint64_t) data_box.header_size - 8);
    return true;
}

static void m4a_read_text_tag(FILE * f, m4a_box_t item, char * dst, size_t dst_size, bool * has_flag) {
    long payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size == 0) return;

    uint32_t n = (payload_size > dst_size - 1) ? (uint32_t) (dst_size - 1) : payload_size;
    fseek(f, payload_offset, SEEK_SET);
    if (fread(dst, 1, n, f) != n) return;
    dst[n] = '\0';
    *has_flag = true;
}

static void m4a_read_cover_art(FILE * f, m4a_box_t item, track_metadata_t * out) {
    if (out->picture_data != NULL) return; /* keep the first picture found, same as FLAC/MP3 */

    long payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size == 0) return;

    uint8_t * data = malloc(payload_size);
    if (!data) return;
    fseek(f, payload_offset, SEEK_SET);
    if (fread(data, 1, payload_size, f) == payload_size) {
        out->picture_data = data;
        out->picture_size = payload_size;
    } else {
        free(data);
    }
}

static void read_m4a_metadata(const char * path, track_metadata_t * out) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    m4a_box_t moov;
    bool found_moov = false;
    while (ftell(f) < file_size) {
        m4a_box_t box;
        if (!m4a_read_box_header(f, &box)) break;
        if (strcmp(box.type, "moov") == 0) { moov = box; found_moov = true; break; }
        long next = (long) (box.data_start - box.header_size) + (long) box.size;
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    if (!found_moov) { fclose(f); return; }

    m4a_box_t udta, meta, ilst;
    if (!m4a_find_child(f, moov.data_start, moov.size - (uint64_t) moov.header_size, "udta", &udta)) { fclose(f); return; }
    if (!m4a_find_child(f, udta.data_start, udta.size - (uint64_t) udta.header_size, "meta", &meta)) { fclose(f); return; }

    /* Unlike every other container walked here, top-level "meta" is itself a
     * FullBox -- its children start 4 bytes (version+flags) further in
     * than a plain container's would. */
    long meta_children_start = meta.data_start + 4;
    uint64_t meta_children_size = meta.size - (uint64_t) meta.header_size - 4;
    if (!m4a_find_child(f, meta_children_start, meta_children_size, "ilst", &ilst)) { fclose(f); return; }

    long ilst_end = ilst.data_start + (long) (ilst.size - (uint64_t) ilst.header_size);
    fseek(f, ilst.data_start, SEEK_SET);
    while (ftell(f) < ilst_end) {
        m4a_box_t item;
        if (!m4a_read_box_header(f, &item)) break;

        if (strcmp(item.type, "\xA9" "nam") == 0) {
            m4a_read_text_tag(f, item, out->title, sizeof(out->title), &out->has_title);
        } else if (strcmp(item.type, "\xA9" "ART") == 0) {
            m4a_read_text_tag(f, item, out->artist, sizeof(out->artist), &out->has_artist);
        } else if (strcmp(item.type, "\xA9" "alb") == 0) {
            m4a_read_text_tag(f, item, out->album, sizeof(out->album), &out->has_album);
        } else if (strcmp(item.type, "aART") == 0) {
            m4a_read_text_tag(f, item, out->album_artist, sizeof(out->album_artist), &out->has_album_artist);
        } else if (strcmp(item.type, "\xA9" "gen") == 0) {
            m4a_read_text_tag(f, item, out->genre, sizeof(out->genre), &out->has_genre);
        } else if (strcmp(item.type, "covr") == 0) {
            m4a_read_cover_art(f, item, out);
        }

        long next = (long) (item.data_start - item.header_size) + (long) item.size;
        if (fseek(f, next, SEEK_SET) != 0) break;
    }

    fclose(f);
}

/* ---- Opus: OpusTags (Ogg comment header, RFC 7845 5.2) ---- */

static void read_opus_metadata(const char * path, track_metadata_t * out) {
    ogg_demux_t * demux = ogg_demux_open(path);
    if (!demux) return;

    unsigned int count = ogg_demux_get_comment_count(demux);
    for (unsigned int i = 0; i < count; i++) {
        uint32_t comment_len;
        const char * comment = ogg_demux_get_comment(demux, i, &comment_len);
        if (!comment) continue;

        /* METADATA_BLOCK_PICTURE isn't a plain KEY=VALUE text field --
         * apply_vorbis_comment_field() would just fail its '='-split match
         * on the base64 payload harmlessly, but handling it explicitly
         * here (rather than teaching that shared helper about base64/
         * binary decoding) keeps it a pure text-field matcher. */
        static const char picture_key[] = "METADATA_BLOCK_PICTURE=";
        size_t picture_key_len = sizeof(picture_key) - 1;
        if (comment_len > picture_key_len && strncasecmp(comment, picture_key, picture_key_len) == 0) {
            const char * b64 = comment + picture_key_len;
            size_t b64_len = comment_len - picture_key_len;

            size_t decoded_len = 0;
            int size_err = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *) b64, b64_len);
            if ((size_err == 0 || size_err == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) && decoded_len > 0) {
                uint8_t * decoded = malloc(decoded_len);
                if (decoded) {
                    size_t actual_len = 0;
                    if (mbedtls_base64_decode(decoded, decoded_len, &actual_len, (const unsigned char *) b64, b64_len) == 0) {
                        parse_flac_picture_block(decoded, actual_len, out);
                    }
                    free(decoded);
                }
            }
            continue;
        }

        apply_vorbis_comment_field(out, comment, comment_len);
    }

    ogg_demux_close(demux);
}

void metadata_read(const char * path, track_metadata_t * out) {
    memset(out, 0, sizeof(*out));

    const char * ext = strrchr(path, '.');
    if (!ext) return;

    if (strcasecmp(ext, ".flac") == 0) {
        read_flac_metadata(path, out);
    } else if (strcasecmp(ext, ".mp3") == 0) {
        read_mp3_metadata(path, out);
    } else if (strcasecmp(ext, ".wav") == 0) {
        read_wav_metadata(path, out);
    } else if (strcasecmp(ext, ".aac") == 0) {
        read_aac_metadata(path, out);
    } else if (strcasecmp(ext, ".m4a") == 0) {
        read_m4a_metadata(path, out);
    } else if (strcasecmp(ext, ".opus") == 0) {
        read_opus_metadata(path, out);
    }
}

/* Real-device incident: a handful of FLAC files with a malformed leading
 * ID3v2 tag (confirmed via a standalone host-side reproduction: dr_flac's
 * ID3-skip landed at the wrong offset and went on to parse garbage bytes
 * as fake metadata block headers) made a full library rescan grind the
 * whole device to a halt on this single-core, 56MB-RAM target, even though
 * the same files didn't visibly hang on a fast host CPU -- consistent with
 * a garbage block-length value blowing up into a pathologically slow (not
 * strictly infinite, just far too slow to matter) parse loop rather than a
 * hang in the traditional sense. gui.c's own scan already had a thread-
 * based watchdog for a stuck file, but that can only ABANDON a stuck
 * worker thread, never actually stop it (pthread_cancel against arbitrary
 * vendored decoder code mid-flight isn't safe) -- several such files in a
 * row each left one more runaway thread permanently competing for the
 * single core, which reads as "frozen" even though no single file was
 * truly stuck forever.
 *
 * This runs the real parse in a short-lived child process instead of the
 * calling thread, exactly like subprocess.c's own SIGKILL-on-timeout
 * pattern (see subprocess_run_timeout()'s doc comment) -- unlike a leaked
 * thread, the OS fully reclaims a killed process's CPU and memory
 * immediately, and a hard crash inside the decoder (a plausible cause
 * given garbage data is being parsed as if it were valid) is contained to
 * that one child instead of taking down the whole app. picture_data is
 * always dropped (freed in the child, never sent back) -- getting a
 * variable-length buffer across a fork boundary isn't worth the
 * complexity for the one caller (a bulk library scan) that needs this,
 * which already discards the picture right after this call anyway. */
void metadata_read_isolated(const char * path, track_metadata_t * out, int timeout_ms) {
    memset(out, 0, sizeof(*out));

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        metadata_read(path, out); /* pipe() failing is exotic enough to fall back to the plain, unprotected read rather than reporting every file unreadable */
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        metadata_read(path, out);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        track_metadata_t result;
        metadata_read(path, &result);
        free(result.picture_data);
        result.picture_data = NULL;
        result.picture_size = 0;
        size_t written = 0;
        while (written < sizeof(result)) {
            ssize_t n = write(pipefd[1], (const char *) &result + written, sizeof(result) - written);
            if (n <= 0) break;
            written += (size_t) n;
        }
        _exit(0);
    }

    close(pipefd[1]);

    size_t total = 0;
    bool ok = true;
    while (total < sizeof(*out)) {
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) { ok = false; break; }
        ssize_t n = read(pipefd[0], (char *) out + total, sizeof(*out) - total);
        if (n <= 0) { ok = false; break; }
        total += (size_t) n;
    }
    close(pipefd[0]);
    if (!ok) {
        memset(out, 0, sizeof(*out));
        kill(pid, SIGKILL);
    }

    /* Bounded reap, same as subprocess_run_timeout()'s own final wait --
     * even after SIGKILL, a child genuinely stuck in an uninterruptible
     * kernel read stays unreapable until that I/O naturally unblocks, so
     * this can't be a plain blocking waitpid() either. */
    for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}
