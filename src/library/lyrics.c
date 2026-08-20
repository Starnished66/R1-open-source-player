#include "lyrics.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Longest valid UTF-8 prefix of s[0..len) -- used to truncate a line's text
 * at a safe boundary (never split a multi-byte codepoint) instead of
 * rejecting the whole line over one bad byte, either from real mojibake or
 * from LYRICS_MAX_LINE_BYTES cutting a line off mid-codepoint. Rejects
 * overlong/malformed sequences too; this is a defensive truncation bound,
 * not a strict conformance validator. */
static size_t utf8_valid_prefix_len(const unsigned char * s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        size_t seq_len;
        if (c < 0x80) seq_len = 1;
        else if ((c & 0xE0) == 0xC0) seq_len = 2;
        else if ((c & 0xF0) == 0xE0) seq_len = 3;
        else if ((c & 0xF8) == 0xF0) seq_len = 4;
        else return i;
        if (i + seq_len > len) return i;
        for (size_t j = 1; j < seq_len; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return i;
        }
        i += seq_len;
    }
    return i;
}

/* Caps the digit run consumed to 8 -- a malformed file can hand this an
 * arbitrarily long run of digit characters (nothing upstream bounds how
 * many consecutive digits form "minutes"/"seconds" before this is called),
 * and accumulating more than a handful into a plain `int` is signed-integer-
 * overflow undefined behavior in C. 8 digits (max 99999999) stays well
 * under INT_MAX and is already far beyond any real timestamp -- callers
 * clamp the final millisecond value against LYRICS_MAX_TIME_MS separately,
 * but that clamp runs after this accumulation, too late to prevent the
 * overflow itself. */
static int atoi_n(const char * s, size_t n) {
    if (n > 8) n = 8;
    int v = 0;
    for (size_t i = 0; i < n; i++) v = v * 10 + (s[i] - '0');
    return v;
}

/* Parses a leading [mm:ss.xx] / [mm:ss.xxx] timestamp from s[0..len).
 * *consumed is set to how many bytes were used -- caller requires this to
 * equal `len` exactly (the whole bracket content) for it to count as a
 * timestamp tag rather than falling through to the metadata-tag check. */
static bool parse_timestamp_ms(const char * s, size_t len, size_t * consumed, uint32_t * out_ms) {
    size_t i = 0;
    size_t min_start = i;
    while (i < len && isdigit((unsigned char) s[i])) i++;
    if (i == min_start) return false;
    int minutes = atoi_n(s + min_start, i - min_start);

    if (i >= len || s[i] != ':') return false;
    i++;

    size_t sec_start = i;
    while (i < len && isdigit((unsigned char) s[i])) i++;
    if (i == sec_start) return false;
    int seconds = atoi_n(s + sec_start, i - sec_start);

    int frac_ms = 0;
    if (i < len && s[i] == '.') {
        i++;
        size_t frac_start = i;
        while (i < len && isdigit((unsigned char) s[i])) i++;
        size_t frac_len = i - frac_start;
        if (frac_len == 0) return false;
        char digits[3] = { '0', '0', '0' };
        for (size_t k = 0; k < 3 && k < frac_len; k++) digits[k] = s[frac_start + k];
        frac_ms = atoi_n(digits, 3);
    }

    /* atoi_n()'s 8-digit cap keeps `minutes`/`seconds` themselves from
     * overflowing signed accumulation, but a capped value this large
     * (up to 99999999) still overflows a uint32_t once multiplied by
     * 60000/1000 below -- well-defined wraparound, not UB, but it can
     * alias a malformed timestamp into an apparently-small, seemingly
     * valid value instead of reliably becoming the LYRICS_MAX_TIME_MS
     * clamp the caller (lyrics_parse_buffer()) applies afterward. Summing
     * in a wider type first and clamping before narrowing closes that:
     * the largest possible sum here (~6*10^12) is nowhere near
     * UINT64_MAX, so this addition itself can never wrap. */
    uint64_t total_ms = (uint64_t) minutes * 60000u + (uint64_t) seconds * 1000u + (uint64_t) frac_ms;
    if (total_ms > LYRICS_MAX_TIME_MS) total_ms = LYRICS_MAX_TIME_MS;
    *out_ms = (uint32_t) total_ms;
    *consumed = i;
    return true;
}

typedef struct {
    lyrics_line_t * arr;
    int count;
    int cap;
} line_builder_t;

static void line_builder_push(line_builder_t * b, uint32_t time_ms, const char * text, size_t text_len) {
    if (b->count >= LYRICS_MAX_LINES) return;
    if (b->count == b->cap) {
        int new_cap = b->cap ? b->cap * 2 : 64;
        if (new_cap > LYRICS_MAX_LINES) new_cap = LYRICS_MAX_LINES;
        lyrics_line_t * n = realloc(b->arr, (size_t) new_cap * sizeof(lyrics_line_t));
        if (!n) return;
        b->arr = n;
        b->cap = new_cap;
    }
    size_t clipped = text_len > LYRICS_MAX_LINE_BYTES ? LYRICS_MAX_LINE_BYTES : text_len;
    clipped = utf8_valid_prefix_len((const unsigned char *) text, clipped);
    char * copy = malloc(clipped + 1);
    if (!copy) return;
    memcpy(copy, text, clipped);
    copy[clipped] = '\0';
    b->arr[b->count].time_ms = time_ms;
    b->arr[b->count].text = copy;
    b->count++;
}

/* Strips leading [mm:ss.xx] timestamp tags (collecting each into
 * pending_times -- a line can carry more than one), a file-scope
 * [offset:+/-N] tag (written into *offset_ms, applied to every line only
 * after the whole file has been scanned), and metadata tags ([ti:...] and
 * the like -- 1-6 letters then ':', skipped without becoming a line).
 * Whatever remains after the last recognized leading bracket is the line's
 * text. A line with no timestamp tag is dropped entirely -- this first
 * release is sync-only, unsynced lines have nothing to attach to. */
static void process_line(const char * line, size_t len, line_builder_t * b, long * offset_ms) {
    size_t i = 0;
    uint32_t pending_times[8];
    int pending_count = 0;

    while (i < len && line[i] == '[') {
        size_t close = i + 1;
        while (close < len && line[close] != ']') close++;
        if (close >= len) break;

        size_t inner_start = i + 1;
        size_t inner_len = close - inner_start;
        const char * inner = line + inner_start;

        size_t consumed;
        uint32_t ts_ms;
        if (inner_len > 0 && isdigit((unsigned char) inner[0]) &&
            parse_timestamp_ms(inner, inner_len, &consumed, &ts_ms) && consumed == inner_len) {
            if (pending_count < 8) pending_times[pending_count++] = ts_ms;
            i = close + 1;
            continue;
        }

        if (inner_len > 7 && strncasecmp(inner, "offset:", 7) == 0) {
            size_t p = 7;
            int sign = 1;
            if (p < inner_len && (inner[p] == '+' || inner[p] == '-')) {
                if (inner[p] == '-') sign = -1;
                p++;
            }
            size_t digit_start = p;
            long val = 0;
            /* Same overflow reasoning as atoi_n()'s own comment -- stop
             * accumulating past 8 digits (this target's `long` is 32-bit,
             * same overflow risk as `int`), but keep advancing `p` so a
             * longer, still-malformed run is still recognized as digits
             * (and correctly consumed/skipped) rather than falling through
             * and being misread as literal text. */
            while (p < inner_len && isdigit((unsigned char) inner[p])) {
                if (p - digit_start < 8) val = val * 10 + (inner[p] - '0');
                p++;
            }
            if (p > digit_start) *offset_ms = sign * val;
            i = close + 1;
            continue;
        }

        size_t p = 0;
        while (p < inner_len && p < 6 && isalpha((unsigned char) inner[p])) p++;
        if (p > 0 && p < inner_len && inner[p] == ':') {
            i = close + 1;
            continue;
        }

        break; /* unrecognized bracket -- leave it as literal text below */
    }

    if (pending_count == 0) return;

    const char * text = line + i;
    size_t text_len = len - i;
    while (text_len > 0 && (*text == ' ' || *text == '\t')) { text++; text_len--; }

    for (int k = 0; k < pending_count; k++) line_builder_push(b, pending_times[k], text, text_len);
}

static int compare_lines(const void * a, const void * b) {
    const lyrics_line_t * la = a;
    const lyrics_line_t * lb = b;
    if (la->time_ms < lb->time_ms) return -1;
    if (la->time_ms > lb->time_ms) return 1;
    return 0;
}

bool lyrics_parse_buffer(const char * buf, size_t len, lyrics_doc_t * out) {
    if (!buf || !out || len == 0 || len > LYRICS_MAX_FILE_BYTES) return false;

    size_t pos = 0;
    if (len >= 3 && (unsigned char) buf[0] == 0xEF && (unsigned char) buf[1] == 0xBB && (unsigned char) buf[2] == 0xBF) pos = 3;

    line_builder_t b = { 0 };
    long offset_ms = 0;

    while (pos < len && b.count < LYRICS_MAX_LINES) {
        size_t line_start = pos;
        while (pos < len && buf[pos] != '\n') pos++;
        size_t line_end = pos;
        if (pos < len) pos++;

        size_t seg_len = line_end - line_start;
        const char * seg = buf + line_start;
        if (seg_len > 0 && seg[seg_len - 1] == '\r') seg_len--;
        if (seg_len == 0) continue;

        process_line(seg, seg_len, &b, &offset_ms);
    }

    if (b.count == 0) {
        free(b.arr);
        return false;
    }

    for (int i = 0; i < b.count; i++) {
        long t = (long) b.arr[i].time_ms + offset_ms;
        if (t < 0) t = 0;
        if ((unsigned long) t > LYRICS_MAX_TIME_MS) t = LYRICS_MAX_TIME_MS;
        b.arr[i].time_ms = (uint32_t) t;
    }

    qsort(b.arr, (size_t) b.count, sizeof(lyrics_line_t), compare_lines);

    out->lines = b.arr;
    out->count = b.count;
    return true;
}

void lyrics_doc_free(lyrics_doc_t * doc) {
    if (!doc || !doc->lines) return;
    for (int i = 0; i < doc->count; i++) free(doc->lines[i].text);
    free(doc->lines);
    doc->lines = NULL;
    doc->count = 0;
}

int lyrics_find_line_at(const lyrics_doc_t * doc, double position_seconds) {
    if (!doc || !doc->lines || doc->count == 0) return -1;
    if (position_seconds < 0) position_seconds = 0;
    uint32_t pos_ms = (uint32_t) (position_seconds * 1000.0);

    int lo = 0, hi = doc->count - 1, result = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (doc->lines[mid].time_ms <= pos_ms) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

bool lyrics_load_sidecar(const char * audio_track_path, lyrics_doc_t * out) {
    if (!audio_track_path || !out) return false;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", audio_track_path);
    char * dot = strrchr(path, '.');
    char * slash = strrchr(path, '/');
    if (dot && (!slash || dot > slash)) *dot = '\0';
    size_t base_len = strlen(path);
    if (base_len + 4 >= sizeof(path)) return false;
    memcpy(path + base_len, ".lrc", 5);

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t) st.st_size > LYRICS_MAX_FILE_BYTES) return false;

    FILE * f = fopen(path, "rb");
    if (!f) return false;
    char * buf = malloc((size_t) st.st_size);
    bool read_ok = buf && fread(buf, 1, (size_t) st.st_size, f) == (size_t) st.st_size;
    fclose(f);
    if (!read_ok) { free(buf); return false; }

    bool ok = lyrics_parse_buffer(buf, (size_t) st.st_size, out);
    free(buf);
    return ok;
}
