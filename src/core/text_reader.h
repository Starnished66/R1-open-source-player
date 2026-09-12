#ifndef TEXT_READER_H
#define TEXT_READER_H

#include <stdbool.h>

/* Recursively scans root for .txt files, sorted alphabetically by full
 * path -- same contract as file_browser_scan_all_songs() (see its own
 * comment) for plain text files instead of audio, kept as a separate
 * standalone scanner rather than generalizing file_browser.c's internals
 * (which are tightly coupled to playable-audio extensions and playlist
 * building, a different shape of problem than "list some files, open one
 * to read"). Caller owns *out_paths (free each entry, then the array).
 * Returns false if root can't be read or has no .txt files anywhere
 * under it. */
bool text_reader_scan_txt_files(const char * root, char *** out_paths, int * out_count);

/* Reads the whole file into a heap buffer, NUL-terminated, up to
 * TEXT_READER_MAX_BYTES -- large enough for any reasonable plain-text
 * book/document, small enough to stay a "basic" reader (no pagination or
 * streaming) without a pathological multi-MB file stalling the UI thread
 * or blowing up memory on this device. Returns NULL if the file can't be
 * opened; *out_truncated is set to true if the real file was larger than
 * the cap. Caller owns the returned buffer (free()). */
#define TEXT_READER_MAX_BYTES (512 * 1024)
char * text_reader_load(const char * path, bool * out_truncated);

#endif /* TEXT_READER_H */
