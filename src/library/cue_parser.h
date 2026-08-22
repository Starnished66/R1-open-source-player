#ifndef CUE_PARSER_H
#define CUE_PARSER_H

#include <stdbool.h>

/* One TRACK entry from a .cue sheet -- title/performer default to empty
 * strings (not every track, or every sheet, sets both), start_seconds is
 * always set (falls back to 0.0 for the first track if the sheet somehow
 * omits INDEX 01, matching how a real player would still want SOME start
 * point rather than dropping the track). */
typedef struct {
    int number;
    char title[128];
    char performer[128];
    double start_seconds;
    bool has_index_01;
} cue_track_t;

/* A parsed .cue sheet -- single-FILE sheets only (the overwhelming common
 * case: one big lossless rip of a whole album/disc, split into tracks by
 * INDEX time rather than separate files). A sheet with more than one FILE
 * command is rejected outright (cue_parse_file() returns false) rather than
 * silently mis-attributing tracks from the second FILE onward to the first
 * -- see cue_parser.c's own comment on why this is the right call for now. */
typedef struct {
    char audio_path[600]; /* resolved: the .cue's own directory + FILE's own filename */
    cue_track_t * tracks; /* caller-owned, sorted by INDEX 01 time ascending -- see cue_parser.c */
    int track_count;
} cue_sheet_t;

/* Parses cue_path (already resolved, real path). On success, out->audio_path
 * points at the referenced audio file (existence NOT verified here -- see
 * cue_parser.c's own comment); out->tracks is a freshly malloc'd array the
 * caller owns, validated in strictly ascending start-time order, always at least 1 entry when true is
 * returned. Returns false (out left zeroed) on a missing/unreadable file, a
 * sheet with no FILE command, no TRACK AUDIO entries, or more than one FILE
 * command (see cue_sheet_t's own comment). */
bool cue_parse_file(const char * cue_path, cue_sheet_t * out);

/* Frees out->tracks and zeroes the struct -- safe on an already-zeroed or
 * partially-parsed (cue_parse_file() returned false) cue_sheet_t. */
void cue_sheet_free(cue_sheet_t * out);

#endif /* CUE_PARSER_H */
