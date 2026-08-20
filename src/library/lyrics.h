#ifndef LYRICS_H
#define LYRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Cap an accepted .lrc file at this many bytes -- rejects outright rather
 * than truncating a file this large, since something that big is more
 * likely a wrong-file mixup than a real lyric sheet. */
#define LYRICS_MAX_FILE_BYTES (512u * 1024u)

/* Bounded parse output -- the whole doc is held in memory at once (there's
 * no paging/DB layer here, unlike the music library), so both caps keep a
 * pathological file from producing an unbounded allocation. */
#define LYRICS_MAX_LINES 4000
#define LYRICS_MAX_LINE_BYTES 512

/* Clamp any parsed or offset-shifted timestamp to this many milliseconds
 * (36 hours) -- generous enough for any real track, small enough to catch
 * a corrupt/garbage timestamp before it becomes a huge, mostly-empty
 * virtualized scroll range. */
#define LYRICS_MAX_TIME_MS (36u * 3600u * 1000u)

typedef struct {
    uint32_t time_ms;
    char * text; /* owned, UTF-8, NUL-terminated */
} lyrics_line_t;

typedef struct {
    lyrics_line_t * lines; /* owned, sorted ascending by time_ms */
    int count;
} lyrics_doc_t;

/* Parses LRC-format text already resident in memory -- no file I/O here,
 * see lyrics_load_sidecar() below for that. `buf` need not be
 * NUL-terminated; `len` bounds it. Recognizes [mm:ss.xx]/[mm:ss.xxx]
 * (multiple leading timestamps on one line each produce their own entry
 * sharing that line's text), a single [offset:+/-N] tag applied as a bias
 * to every timestamp in the file then clamped to >= 0, and skips metadata
 * tags ([ti:...], [ar:...], [al:...], [by:...], etc. -- 2-3 letters then a
 * colon) without emitting them as lines. Strips a leading UTF-8 BOM.
 * Truncates a line's text at the first invalid UTF-8 byte rather than
 * rejecting the whole file over one bad line. Returns false (leaving *out
 * unset) only if `buf`/`len` describe nothing parseable at all (e.g. over
 * LYRICS_MAX_FILE_BYTES, or zero usable lines) -- caller owns *out on
 * success and must lyrics_doc_free() it. */
bool lyrics_parse_buffer(const char * buf, size_t len, lyrics_doc_t * out);

void lyrics_doc_free(lyrics_doc_t * doc);

/* Binary search for the last line whose time_ms <= position_seconds*1000.
 * Returns -1 if position_seconds is before the first line (or doc is
 * empty) -- caller treats that as "no active line yet" rather than an
 * error. */
int lyrics_find_line_at(const lyrics_doc_t * doc, double position_seconds);

/* Resolves the same-basename .lrc sidecar path for a local audio file path
 * (swap the extension for ".lrc" in the same directory), reads it (capped
 * at LYRICS_MAX_FILE_BYTES) and parses it in one step. Safe to call from a
 * background thread -- this is the only file-I/O entry point in this
 * module. Returns false if no sidecar exists, it's unreadable, or it
 * parses to zero usable lines. */
bool lyrics_load_sidecar(const char * audio_track_path, lyrics_doc_t * out);

#endif /* LYRICS_H */
