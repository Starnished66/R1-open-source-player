#include "cue_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Extracts a "quoted string" argument (the TITLE/PERFORMER/FILE convention)
 * from `line`, starting the scan at `line`'s own first non-space character
 * after the command keyword has already been skipped by the caller. Some
 * real-world tools write an unquoted argument for a name with no spaces --
 * tolerated here too: falls back to reading up to the next whitespace/EOL
 * if no opening quote is found. Bounded-copies into out (out_size includes
 * the NUL). */
static void extract_arg(const char * line, char * out, size_t out_size) {
    out[0] = '\0';
    while (*line == ' ' || *line == '\t') line++;

    if (*line == '"') {
        line++;
        const char * end = strchr(line, '"');
        size_t len = end ? (size_t) (end - line) : strlen(line);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, line, len);
        out[len] = '\0';
        return;
    }

    size_t len = 0;
    while (line[len] && line[len] != '\r' && line[len] != '\n' && !isspace((unsigned char) line[len])) len++;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, line, len);
    out[len] = '\0';
}

/* mm:ss:ff (CD frame time -- 75 frames/sec, the Red Book standard every
 * real .cue sheet's INDEX lines use) -> seconds. Returns false (out
 * untouched) if the three fields don't parse as plain integers, so a
 * malformed INDEX line is skipped rather than silently producing 0:00:00 as
 * if it were a genuine track start. */
static bool parse_cue_time(const char * s, double * out_seconds) {
    if (!s || !out_seconds) return false;
    unsigned long fields[3] = {0, 0, 0};
    for (int field = 0; field < 3; field++) {
        if (!isdigit((unsigned char) *s)) return false;
        int digits = 0;
        while (isdigit((unsigned char) *s)) {
            if (++digits > 7 || fields[field] > 10000000UL) return false;
            fields[field] = fields[field] * 10UL + (unsigned long) (*s - '0');
            s++;
        }
        if (field < 2) {
            if (*s++ != ':') return false;
        }
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s != '\0' || fields[1] > 59 || fields[2] > 74) return false;
    *out_seconds = (double) fields[0] * 60.0 + (double) fields[1] + (double) fields[2] / 75.0;
    return true;
}

/* Case-insensitive "does line start with keyword, followed by a word
 * boundary (space/tab/EOL)" check -- real .cue sheets are written in
 * ALL CAPS by convention but not every tool honors that strictly. Returns
 * a pointer just past the keyword (and any following whitespace) on match,
 * NULL otherwise. */
static const char * match_keyword(const char * line, const char * keyword) {
    size_t len = strlen(keyword);
    if (strncasecmp(line, keyword, len) != 0) return NULL;
    char next = line[len];
    if (next != '\0' && next != ' ' && next != '\t' && next != '\r' && next != '\n') return NULL;
    const char * rest = line + len;
    while (*rest == ' ' || *rest == '\t') rest++;
    return rest;
}

static bool append_track(cue_track_t ** tracks, int * count, int * capacity, int number) {
    if (*count == *capacity) {
        int new_capacity = *capacity == 0 ? 8 : *capacity * 2;
        cue_track_t * grown = realloc(*tracks, sizeof(cue_track_t) * (size_t) new_capacity);
        if (!grown) return false;
        *tracks = grown;
        *capacity = new_capacity;
    }
    cue_track_t * t = &(*tracks)[*count];
    memset(t, 0, sizeof(*t));
    t->number = number;
    (*count)++;
    return true;
}

bool cue_parse_file(const char * cue_path, cue_sheet_t * out) {
    memset(out, 0, sizeof(*out));

    FILE * f = fopen(cue_path, "r");
    if (!f) return false;

    char file_name[512] = "";
    int file_command_count = 0;

    cue_track_t * tracks = NULL;
    int track_count = 0;
    int track_capacity = 0;
    bool have_current_track = false;
    bool current_track_is_audio = false;
    bool parse_failed = false;

    char line[1024];
    bool first_line = true;
    while (fgets(line, sizeof(line), f)) {
        char * p = line;
        /* Strip a leading UTF-8 BOM -- real-world .cue files (especially
         * from Windows-based ripping tools) commonly have one; left in
         * place, it would corrupt the very first command's own keyword
         * match. Only checked on the first line actually read -- line[] is
         * one reused stack buffer across every iteration of this loop, so
         * checking this unconditionally on every line would risk matching
         * stale leftover bytes from a PREVIOUS, longer line still sitting
         * past a later short line's own NUL terminator (memory-safe either
         * way, fgets() always NUL-terminates within the buffer, but a false
         * match there would wrongly eat that line's own first 3 real
         * characters). */
        if (first_line && (unsigned char) p[0] == 0xEF && (unsigned char) p[1] == 0xBB && (unsigned char) p[2] == 0xBF) {
            p += 3;
        }
        first_line = false;
        while (*p == ' ' || *p == '\t') p++;

        const char * arg;
        if ((arg = match_keyword(p, "FILE")) != NULL) {
            file_command_count++;
            extract_arg(arg, file_name, sizeof(file_name));
        } else if ((arg = match_keyword(p, "TRACK")) != NULL) {
            int number = atoi(arg);
            const char * type = arg;
            while (*type && !isspace((unsigned char) *type)) type++;
            while (*type == ' ' || *type == '\t') type++;
            /* Only AUDIO tracks -- a mixed-mode disc's own DATA track(s)
             * (a hidden data session some CD releases have) aren't
             * something this app can play, and would just be silently
             * unseekable/silent audio if treated as one. */
            current_track_is_audio = strncasecmp(type, "AUDIO", 5) == 0;
            if (current_track_is_audio) {
                have_current_track = append_track(&tracks, &track_count, &track_capacity, number);
                if (!have_current_track) {
                    parse_failed = true;
                    break;
                }
            } else {
                have_current_track = false;
            }
        } else if (have_current_track && current_track_is_audio && (arg = match_keyword(p, "TITLE")) != NULL) {
            extract_arg(arg, tracks[track_count - 1].title, sizeof(tracks[track_count - 1].title));
        } else if (have_current_track && current_track_is_audio && (arg = match_keyword(p, "PERFORMER")) != NULL) {
            extract_arg(arg, tracks[track_count - 1].performer, sizeof(tracks[track_count - 1].performer));
        } else if (have_current_track && current_track_is_audio && (arg = match_keyword(p, "INDEX")) != NULL) {
            /* INDEX 01 is the real track start every player seeks to;
             * INDEX 00 (if present) is the pre-gap, which precedes it and
             * isn't where a listener tapping this track expects playback
             * to begin -- skipped in favor of 01. Some sheets have only
             * 01 with no 00 at all, which this handles the same way
             * (the only INDEX line present is 01). */
            int index_number = atoi(arg);
            if (index_number == 1) {
                const char * time_str = arg;
                while (*time_str && !isspace((unsigned char) *time_str)) time_str++;
                while (*time_str == ' ' || *time_str == '\t') time_str++;
                double start_seconds;
                if (!parse_cue_time(time_str, &start_seconds)) {
                    parse_failed = true;
                    break;
                }
                tracks[track_count - 1].start_seconds = start_seconds;
                tracks[track_count - 1].has_index_01 = true;
            }
        }
    }
    fclose(f);

    /* Multiple FILE commands (a sheet spanning several physical audio
     * files) is real but comparatively rare -- correctly attributing each
     * track to the right one of several files needs tracking which FILE
     * section each track fell under, not just the single audio_path this
     * struct currently has room for. Rejected outright rather than
     * silently mis-attributing every track after the first FILE to the
     * wrong file, which would seek into the wrong song entirely. */
    if (!parse_failed) {
        for (int i = 0; i < track_count; i++) {
            if (!tracks[i].has_index_01 || (i > 0 && tracks[i].start_seconds <= tracks[i - 1].start_seconds)) {
                parse_failed = true;
                break;
            }
        }
    }
    if (parse_failed || file_command_count != 1 || track_count == 0 || file_name[0] == '\0') {
        free(tracks);
        return false;
    }

    /* Resolve relative to the .cue file's OWN directory, not the process's
     * current working directory -- matches playlist_files.c's own relative-
     * path convention for the exact same reason (the .cue and its audio
     * file are expected to sit side by side on the SD card, wherever that
     * happens to be mounted). */
    const char * last_slash = strrchr(cue_path, '/');
    if (last_slash) {
        size_t dir_len = (size_t) (last_slash - cue_path) + 1;
        if (dir_len >= sizeof(out->audio_path)) dir_len = sizeof(out->audio_path) - 1;
        memcpy(out->audio_path, cue_path, dir_len);
        snprintf(out->audio_path + dir_len, sizeof(out->audio_path) - dir_len, "%s", file_name);
    } else {
        snprintf(out->audio_path, sizeof(out->audio_path), "%s", file_name);
    }

    out->tracks = tracks;
    out->track_count = track_count;
    return true;
}

void cue_sheet_free(cue_sheet_t * out) {
    if (!out) return;
    free(out->tracks);
    memset(out, 0, sizeof(*out));
}
