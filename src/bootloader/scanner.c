#include "scanner.h"
#include "sd_ready.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUILD_STAMP_LEN BOOT_BUILD_STAMP_LEN

/* mount_sd_card_if_needed() itself now lives in sd_ready_real.c, backed by
 * sd_ready.c's adaptive wait_for_sd_ready() state machine instead of a flat
 * fixed-attempt loop -- see sd_ready.c's own top comment for the algorithm
 * (the asynchronous MMC-detect / node-publish / mount race this exists to
 * ride out) and sd_ready_real.c for this build's actual timing policy. Kept
 * as a real cross-TU call (sd_ready.h) rather than folded back in here so
 * the readiness state machine itself stays testable (sd_ready_test.c)
 * without dragging in this file's own build-stamp-scanning/preference-file
 * logic, which has nothing to do with SD readiness. */

#define DEFAULT_TIMEOUT_SECONDS 3
#define MIN_TIMEOUT_SECONDS 1
#define MAX_TIMEOUT_SECONDS 30

static bool path_is_executable(const char * path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* True if the 16 bytes at p, followed by a NUL, exactly match the
 * "YYYY-MM-DD_HH:MM" shape the Makefile's `date +%Y-%m-%d_%H:%M` always
 * produces. Checking for the trailing NUL (not just the digit/separator
 * shape) matters: this is scanning raw file bytes, not parsed text, and a
 * string literal in an ELF's rodata is NUL-terminated -- requiring it
 * rules out a coincidental digit run inside unrelated binary data that
 * merely happens to start with the right shape. */
static bool looks_like_build_stamp(const unsigned char * p) {
    for (int i = 0; i < 4; i++) if (!isdigit(p[i])) return false;
    if (p[4] != '-') return false;
    for (int i = 5; i < 7; i++) if (!isdigit(p[i])) return false;
    if (p[7] != '-') return false;
    for (int i = 8; i < 10; i++) if (!isdigit(p[i])) return false;
    if (p[10] != '_') return false;
    for (int i = 11; i < 13; i++) if (!isdigit(p[i])) return false;
    if (p[13] != ':') return false;
    for (int i = 14; i < 16; i++) if (!isdigit(p[i])) return false;
    return p[16] == '\0';
}

/* Scans the WHOLE file in bounded chunks (never loads a ~20MB player
 * binary fully into memory just to find a handful of 16-byte strings --
 * this runs before anything else has established how much RAM is actually
 * free) and keeps the LEXICALLY MAXIMUM substring matching
 * looks_like_build_stamp() -- deliberately not the first one found. `make
 * target` is incremental: BUILD_STAMP is evaluated once per invocation and
 * baked into every .o compiled THAT run, but an unchanged .c file's .o
 * from an EARLIER run (with an OLDER embedded stamp) is reused as-is and
 * linked in alongside it -- confirmed empirically, not hypothetically: a
 * real built binary from this same project was found (via `strings`) to
 * contain two different stamps a minute apart. Which one a byte-scan
 * happens to hit FIRST depends on link order/section placement, not on
 * which is actually newer, so taking the first match is not just
 * theoretically wrong but demonstrated wrong. Taking the max is not a
 * perfect "true full link time" (a build system change -- one always-
 * freshly-generated, uniquely-marked version unit -- would be the fully
 * correct fix) but it is far more robust than trusting scan order: the
 * newest recompiled unit's stamp reliably wins on both sides of the
 * comparison this feeds. Returns false (not true-with-empty-string) if
 * the file has no such string at all, e.g. it isn't a build of this app --
 * callers must treat that as "unknown version", never as "oldest possible
 * version". */
static bool extract_build_stamp(const char * path, char * out, size_t out_size) {
    if (out_size <= BUILD_STAMP_LEN) return false;
    FILE * f = fopen(path, "rb");
    if (!f) return false;

    unsigned char buf[65536 + BUILD_STAMP_LEN + 1];
    char best[BUILD_STAMP_LEN + 1];
    size_t carry = 0;
    bool found = false;

    for (;;) {
        size_t n = fread(buf + carry, 1, sizeof(buf) - carry - 1, f);
        size_t total = carry + n;
        if (total < (size_t) BUILD_STAMP_LEN + 1) break; /* not enough left for a full match + trailing NUL */
        /* buf[total] is a SENTINEL this function wrote, never a byte the
         * file actually contains at that position -- a candidate starting
         * at i == total - BUILD_STAMP_LEN would read p[16] as that
         * sentinel instead of the real next file byte (which, mid-file,
         * hasn't been read yet; it arrives in the NEXT chunk's fread()).
         * That made looks_like_build_stamp() see a false NUL terminator
         * at every chunk boundary regardless of the real byte there,
         * capable of manufacturing a bogus "maximum" stamp right at a
         * boundary. Stopping one earlier (scan_end = total - (LEN+1))
         * guarantees p[16] is always a byte this function actually read.
         * This does not cost a genuine match sitting exactly at true EOF:
         * when the file's real last byte IS the stamp's terminating NUL,
         * that byte is already counted in `total` (fread() actually read
         * it), so scan_end still includes that candidate's start index --
         * p[16] resolves to buf[total-1], the real last byte, never to
         * the buf[total] sentinel. */
        buf[total] = '\0';

        size_t scan_end = total - (BUILD_STAMP_LEN + 1);
        for (size_t i = 0; i <= scan_end; i++) {
            if (!looks_like_build_stamp(buf + i)) continue;
            if (!found || memcmp(buf + i, best, BUILD_STAMP_LEN) > 0) {
                memcpy(best, buf + i, BUILD_STAMP_LEN);
                found = true;
            }
        }
        if (n == 0) break;

        /* Carry the tail into the next chunk so a match straddling this
         * boundary is still caught next iteration. */
        carry = (total > (size_t) BUILD_STAMP_LEN) ? (size_t) BUILD_STAMP_LEN : total;
        memmove(buf, buf + total - carry, carry);
    }

    if (found) {
        best[BUILD_STAMP_LEN] = '\0';
        memcpy(out, best, sizeof(best));
    }

    fclose(f);
    return found;
}

/* >0 if sd_path's BUILD_STAMP is lexically greater ("newer") than
 * internal_path's, <0 if lexically less ("older"), 0 if they match exactly
 * or either side's stamp couldn't be found -- "YYYY-MM-DD_HH:MM" sorts
 * correctly as plain text, no date parsing needed. A missing/unreadable
 * stamp on either side folds into 0 ("not comparable"), never guessed as
 * either direction -- an SD binary that isn't even a build of this app (no
 * BUILD_STAMP at all) must never be treated as newer OR older just because
 * it happens to be named open_hiby_player. */
static int compare_build_stamps(const char * internal_stamp, const char * sd_stamp) {
    if (!internal_stamp[0] || !sd_stamp[0]) return 0;
    int cmp = strcmp(sd_stamp, internal_stamp);
    return cmp > 0 ? 1 : (cmp < 0 ? -1 : 0);
}

/* Round-tripped by scanner_save_last_boot() so persisting a new default
 * entry never clobbers a separately hand-edited timeout_seconds back to
 * the default -- set once by scanner_scan() (the only place a boot
 * preference file is ever read), read once by scanner_save_last_boot(). */
static int loaded_timeout_seconds = DEFAULT_TIMEOUT_SECONDS;

static void load_preferences(int * out_default_entry, int * out_timeout_seconds) {
    *out_default_entry = BOOT_ENTRY_INTERNAL;
    *out_timeout_seconds = DEFAULT_TIMEOUT_SECONDS;

    FILE * f = fopen(BOOT_PREF_PATH, "r");
    if (!f) return; /* first boot, or SD/partition not present yet -- defaults above stand */

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int value;
        if (sscanf(line, "default_entry=%d", &value) == 1) {
            if (value == BOOT_ENTRY_INTERNAL || value == BOOT_ENTRY_SD_STOCK || value == BOOT_ENTRY_SD_UPDATE)
                *out_default_entry = value;
        } else if (sscanf(line, "timeout_seconds=%d", &value) == 1) {
            if (value >= MIN_TIMEOUT_SECONDS && value <= MAX_TIMEOUT_SECONDS) *out_timeout_seconds = value;
        }
    }
    fclose(f);
}

void scanner_scan(scan_result_t * out) {
    memset(out, 0, sizeof(*out));

    mount_sd_card_if_needed();

    out->sd_stock_present = path_is_executable(SD_STOCK_PLAYER_PATH);
    out->sd_update_present = path_is_executable(SD_UPDATE_PLAYER_PATH);

    extract_build_stamp(INTERNAL_PLAYER_PATH, out->internal_build_stamp,
                        sizeof(out->internal_build_stamp));
    if (out->sd_update_present) {
        extract_build_stamp(SD_UPDATE_PLAYER_PATH, out->sd_update_build_stamp,
                            sizeof(out->sd_update_build_stamp));
    }

    int sd_build_cmp = 0;
    if (out->sd_update_present) {
        sd_build_cmp = compare_build_stamps(out->internal_build_stamp,
                                            out->sd_update_build_stamp);
        out->sd_update_is_newer = sd_build_cmp > 0;
        out->sd_update_is_older = sd_build_cmp < 0;
    }

    load_preferences(&out->default_entry, &out->timeout_seconds);
    loaded_timeout_seconds = out->timeout_seconds;
    /* The unattended/default choice is always the newest comparable Open
     * Player, independent of an older persisted menu choice. Dropping an
     * equal or non-comparable SD build remains an explicit signal to use
     * that copy, preserving the established SD priority. Stock is still a
     * selectable entry whenever present, but never the automatic one. */
    out->default_entry = out->sd_update_present && sd_build_cmp >= 0
                       ? BOOT_ENTRY_SD_UPDATE : BOOT_ENTRY_INTERNAL;
}

void scanner_save_last_boot(int entry) {
    /* Same tmp-file-then-rename pattern this project already uses for
     * on-disk state it cares about not corrupting on a mid-write power
     * loss (see albumart.c's albumart_store_rgb565()) -- a boot
     * preference file is small and rare enough to write that the extra
     * few syscalls here cost nothing. */
    char tmp[sizeof(BOOT_PREF_PATH) + 16];
    snprintf(tmp, sizeof(tmp), "%s.tmp", BOOT_PREF_PATH);

    FILE * f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "scanner: failed to open %s for writing boot preference\n", tmp);
        return;
    }
    fprintf(f, "default_entry=%d\n", entry);
    fprintf(f, "timeout_seconds=%d\n", loaded_timeout_seconds);
    bool ok = fflush(f) == 0 && fclose(f) == 0;
    if (ok) ok = rename(tmp, BOOT_PREF_PATH) == 0;
    if (!ok) {
        fprintf(stderr, "scanner: failed to persist boot preference\n");
        unlink(tmp);
    }
}
