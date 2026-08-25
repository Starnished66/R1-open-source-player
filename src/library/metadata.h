#ifndef METADATA_H
#define METADATA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    bool has_title;
    bool has_artist;
    bool has_album;

    /* Album artist -- distinct from the track artist for compilations/
     * various-artists albums (FLAC VORBIS_COMMENT ALBUMARTIST, MP3 ID3v2
     * TPE2, M4A "aART" atom). Left false rather than defaulting to the
     * track artist here -- callers that want "fall back to track artist
     * when there's no explicit album artist" (the common, useful behavior
     * for the non-compilation case) can do that themselves by checking
     * has_artist afterward, same as any other has_* fallback in this
     * struct. */
    char album_artist[128];
    bool has_album_artist;

    /* Genre (FLAC VORBIS_COMMENT GENRE, MP3 ID3v2 TCON, M4A "\xA9gen" atom).
     * Only the plain-text form is handled -- legacy ID3v1-style numeric
     * genre codes some older MP3 taggers still write into TCON as e.g.
     * "(17)" or "(17)Rock" are not resolved against the ID3v1 genre table,
     * so a file tagged that way will show the literal "(17)" rather than
     * "Rock". */
    char genre[128];
    bool has_genre;

    /* Album sequencing tags.  Values are the leading positive integer from
     * common forms such as "3", "03/12", ID3 TRCK/TPOS, Vorbis
     * TRACKNUMBER/DISCNUMBER, and MP4 trkn/disk atoms. */
    int track_number;
    int disc_number;
    bool has_track_number;
    bool has_disc_number;

    /* Embedded cover art (FLAC METADATA_BLOCK_PICTURE, MP3 ID3v2 APIC),
     * still-encoded (JPEG or PNG) bytes -- NULL if the file has none or
     * isn't a supported container. malloc'd by metadata_read(); caller
     * takes ownership and must free() it (and only after it's done being
     * displayed -- LVGL's JPEG decoder reads lazily from this buffer via
     * an in-memory "file", it doesn't copy it up front like the PNG one does). */
    uint8_t * picture_data;
    uint32_t picture_size;

    /* Embedded lyrics text (MP3/AAC/AIFF/WAV/DSF/DFF ID3v2 USLT, FLAC/Opus
     * VORBIS_COMMENT LYRICS/UNSYNCEDLYRICS, M4A "\xA9lyr" atom) -- raw text
     * exactly as embedded, NUL-terminated, UTF-8. May or may not contain
     * [mm:ss.xx]-style LRC timestamps; callers should try lyrics_parse_buffer()
     * (lyrics.h) first and fall back to displaying it as a plain unsynced
     * block if that yields no lines, same as an external .lrc sidecar's own
     * content would need. NULL if the file has none or isn't a supported
     * container. malloc'd by metadata_read(); caller takes ownership and
     * must free() it -- same ownership contract as picture_data above. */
    char * lyrics;

    /* ReplayGain track gain/peak -- FLAC VORBIS_COMMENT REPLAYGAIN_TRACK_GAIN
     * / _PEAK, or MP3 ID3v2 TXXX frames with those same description names
     * (the RVA2 binary frame some taggers write instead isn't parsed).
     * Peak is used to avoid clipping: if applying the gain as specified
     * would push the loudest sample past full scale, the gain is capped
     * instead. has_replaygain_peak may be false even when has_replaygain is
     * true (not every tagger writes peak) -- treat peak as 1.0 (no extra
     * headroom needed) in that case. */
    bool has_replaygain;
    double replaygain_gain_db;
    bool has_replaygain_peak;
    double replaygain_peak;

    /* ReplayGain ALBUM gain/peak -- same tag families as track gain/peak
     * above, just the ALBUM_ variant (VORBIS_COMMENT REPLAYGAIN_ALBUM_GAIN/
     * _PEAK, ID3v2 TXXX "REPLAYGAIN_ALBUM_GAIN"/"REPLAYGAIN_ALBUM_PEAK").
     * Lets Settings -> Playback -> ReplayGain's "Per Album" mode preserve
     * intentional relative loudness differences between tracks on the same
     * album, rather than normalizing every track to the same perceived
     * loudness like Per Track mode does. Not every tagger writes album-
     * level tags (most single/track-at-a-time rips only ever get track
     * tags) -- has_replaygain_album false in that case, callers should fall
     * back to track gain (see gui.c's own resolve_replaygain()). */
    bool has_replaygain_album;
    double replaygain_album_gain_db;
    bool has_replaygain_album_peak;
    double replaygain_album_peak;
} track_metadata_t;

/* Reads title/artist/album/album_artist/genre tags, embedded cover art,
 * embedded lyrics, and ReplayGain track/album gain (if present) from a FLAC
 * (VORBIS_COMMENT + METADATA_BLOCK_PICTURE), MP3 (ID3v2 APIC/USLT/TXXX/TPE2/
 * TCON, falling back to ID3v1 which has none of those), M4A (moov/udta/meta/
 * ilst atoms), or WAV (RIFF LIST/INFO, title/artist/album only, plus an ID3v2
 * USLT fallback for lyrics) file. Fields that aren't present or can't be
 * parsed are left as empty/NULL with the corresponding has_* flag (or a NULL
 * check, for the picture/lyrics) false -- callers should fall back to the
 * filename, a placeholder image, or no gain adjustment in that case. */
void metadata_read(const char * path, track_metadata_t * out);

/* Library-scan tag read. MP3/AAC (our own bounded ID3 parser) run
 * in-process. Formats that feed vendored/unbounded decoders (FLAC and
 * the other container walkers) still run in a short-lived child,
 * SIGKILLed if they don't finish within timeout_ms. Reaping is nonblocking:
 * a child stuck in uninterruptible SD I/O is remembered and retried by a
 * later scan call rather than allowing waitpid() to freeze the scanner --
 * see metadata.c. On timeout or child crash, *out is the same as
 * metadata_read()'s "couldn't read tags" case: zeroed, every has_* flag
 * false. picture_data/picture_size/lyrics always come back NULL/0. */
void metadata_read_isolated(const char * path, track_metadata_t * out, int timeout_ms);

#endif /* METADATA_H */
