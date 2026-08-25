#ifndef AUDIO_HELPERS_H
#define AUDIO_HELPERS_H

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Upper bound on missing end-of-track frames due to container padding,
 * gapless encoder delay, or imprecise duration metadata (e.g. VBR MP3/AAC).
 * 8192 frames is ~0.18s at 44.1kHz. Any termination with more than 8192
 * unread frames is clearly premature and must be retried. */
#define MAX_EOF_TOLERANCE_FRAMES 8192ULL

/* Returns a safe, bounded sanitized filename/leaf for diagnostic logs.
 * Strips directory prefixes, remote URLs, HTTP credentials, query parameters,
 * and tokens so logs never leak sensitive info or private metadata. */
static inline const char * safe_path_tail(const char * path) {
    if (!path) return "(null)";
    const char * query = strchr(path, '?');
    const char * hash = strchr(path, '#');
    const char * end = path + strlen(path);
    if (query && query < end) end = query;
    if (hash && hash < end) end = hash;

    const char * start = path;
    const char * scheme = strstr(path, "://");
    if (scheme && scheme + 3 < end) {
        /* URL: check for a path component after the host/authority */
        const char * path_slash = NULL;
        for (const char * p = scheme + 3; p < end; p++) {
            if (*p == '/') path_slash = p;
        }
        if (path_slash) {
            /* Leaf path component (e.g. /rest/stream.flac -> stream.flac) */
            start = path_slash + 1;
        } else {
            /* Authority-only URL (e.g. http://user:pass@example.com) -> strip user:pass */
            const char * at = NULL;
            for (const char * p = scheme + 3; p < end; p++) {
                if (*p == '@') { at = p; break; }
            }
            start = at ? at + 1 : scheme + 3;
        }
    } else {
        /* Local path: find last '/' */
        const char * slash = NULL;
        for (const char * p = path; p < end; p++) {
            if (*p == '/') slash = p;
        }
        if (slash) start = slash + 1;
    }

    if (start >= end) return "(endpoint)";

    static __thread char buf[64];
    size_t len = (size_t) (end - start);
    if (len > 48) {
        start = end - 48;
        len = 48;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

/* Returns true if a zero-frame decoder read is clearly premature (i.e. not
 * natural EOF) for a finite local file. Live streams (total_frames == 0)
 * are excluded -- they never truly "end", so a zero read just means no data
 * yet. */
static inline bool is_premature_eof(uint64_t frames_played_local, uint64_t total_frames,
                                    bool is_stream) {
    if (is_stream || total_frames == 0) return false;
    if (frames_played_local >= total_frames) return false;
    uint64_t remaining = total_frames - frames_played_local;
    return remaining > MAX_EOF_TOLERANCE_FRAMES;
}

#endif /* AUDIO_HELPERS_H */
