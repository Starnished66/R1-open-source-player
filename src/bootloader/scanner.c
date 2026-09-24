#define _POSIX_C_SOURCE 200809L

#include "scanner.h"
#include "sd_ready.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUILD_STAMP_LEN BOOT_BUILD_STAMP_LEN

/* mount_sd_card_if_needed() is implemented in sd_ready_real.c, backed by
 * the wait_for_sd_ready() state machine. */

bool scanner_path_is_executable(const char * path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* Returns true if the 16 bytes at p and the trailing NUL match "YYYY-MM-DD_HH:MM". */
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
    if (p[16] != '\0') return false;
    int year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + p[3] - '0';
    int month = (p[5] - '0') * 10 + p[6] - '0';
    int day = (p[8] - '0') * 10 + p[9] - '0';
    int hour = (p[11] - '0') * 10 + p[12] - '0';
    int minute = (p[14] - '0') * 10 + p[15] - '0';
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (year == 0 || month < 1 || month > 12 || hour > 23 || minute > 59) return false;
    int max_day = days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) max_day++;
    return day >= 1 && day <= max_day;
}

/* Scans an executable binary in chunks for build stamps matching looks_like_build_stamp()
 * and selects the lexicographically maximum stamp found. */
bool scanner_read_build_stamp(const char * path, char * out, size_t out_size) {
    if (!path || !out || out_size <= BUILD_STAMP_LEN) return false;
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
        buf[total] = '\0';

        /* Stop scanning at total - (BUILD_STAMP_LEN + 1) to ensure the trailing NUL byte
         * is part of the bytes read from the file. */
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

    /* A partial read cannot establish which embedded stamp is newest. */
    bool read_ok = !ferror(f);
    if (fclose(f) != 0) read_ok = false;
    if (found && read_ok) {
        best[BUILD_STAMP_LEN] = '\0';
        memcpy(out, best, sizeof(best));
    }

    return found && read_ok;
}

void scanner_scan(scan_result_t * out) {
    memset(out, 0, sizeof(*out));

    mount_sd_card_if_needed();

    out->sd_update_path = scanner_path_is_executable(SD_UPDATE_PLAYER_PATH) ? SD_UPDATE_PLAYER_PATH :
                          (scanner_path_is_executable(LEGACY_SD_UPDATE_PLAYER_PATH) ?
                               LEGACY_SD_UPDATE_PLAYER_PATH : NULL);
    out->sd_update_present = out->sd_update_path != NULL;

}
