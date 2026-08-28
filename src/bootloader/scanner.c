#include "scanner.h"
#include "subprocess.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUILD_STAMP_LEN 16 /* "YYYY-MM-DD_HH:MM" -- see the Makefile's BUILD_STAMP_DEFINE */

/* True if a real filesystem is mounted at SD_MOUNT_POINT (its st_dev
 * differs from its parent's), as opposed to just an empty directory
 * sitting there -- same check as src/main.c's own sd_mount_point_mounted(),
 * against the fully-resolved path (see scanner.h's own comment on why). */
static bool sd_mount_point_mounted(void) {
    struct stat parent_st, mnt_st;
    if (stat("/usr/data/mnt", &parent_st) != 0) return false;
    if (stat(SD_MOUNT_POINT, &mnt_st) != 0) return false;
    return parent_st.st_dev != mnt_st.st_dev;
}

/* Same three-filesystem-in-turn attempt as src/main.c's own
 * try_mount_sd_device_node(), against the same mount options -- ntfs-3g
 * over vfat/exfat is not something this bootloader has any independent
 * basis for; it's copied because the real player's own comment there
 * explains a real, already-debugged reason (no usable in-kernel ntfs path
 * on this kernel/BusyBox combination) that still applies here. */
static void try_mount_sd_device_node(const char * device_node) {
    char * vfat_argv[] = { (char *) "mount", (char *) "-t", (char *) "vfat", (char *) "-o",
                            (char *) "rw,relatime,fmask=0022,dmask=0022,codepage=936,iocharset=utf8,shortname=mixed",
                            (char *) device_node, (char *) SD_MOUNT_POINT, NULL };
    subprocess_run(vfat_argv, NULL, 0);
    if (sd_mount_point_mounted()) return;

    char * exfat_argv[] = { (char *) "mount", (char *) "-t", (char *) "exfat", (char *) "-o",
                             (char *) "rw,relatime", (char *) device_node, (char *) SD_MOUNT_POINT, NULL };
    subprocess_run(exfat_argv, NULL, 0);
    if (sd_mount_point_mounted()) return;

    char * ntfs_argv[] = { (char *) "/usr/bin/ntfs-3g", (char *) "-o",
                            (char *) "rw,relatime,big_writes,umask=0022",
                            (char *) device_node, (char *) SD_MOUNT_POINT, NULL };
    subprocess_run(ntfs_argv, NULL, 0);
}

/* Wait up to 5s (50 x 100ms) for the SD block node. This deliberately
 * exceeds src/main.c's standalone-player fallback settle: field reports
 * from multiple devices showed that the original shared 2.5s bound could
 * expire before mdev exposed the card, making the bootloader silently take
 * the no-alternate path and auto-boot Open Player even though hiby_player
 * was present. The player's own splash minimum is correspondingly shorter
 * now, so this larger bootloader discovery window does not simply stack an
 * additional full splash delay onto the normal SD-present boot.
 *
 * This wait is necessary, not just cautious -- "a software-triggered reboot
 * can start this S92 process before the SD block node has reappeared" -- and
 * mdev creates /dev/mmcblk0* directly off the mmc host's own card-detect
 * uevent (confirmed in gui_library.c's own comment on hotplug), so there
 * is no faster or more direct "card present" signal available than the
 * node itself eventually existing. This bootloader now occupies that same
 * S92 position (see scanner.h's own doc comment), including after the
 * very same class of software-triggered reboot this bootloader's own
 * run_player_supervised() performs after an abnormal player exit -- a single
 * one-shot node check here would intermittently miss a genuinely-present
 * SD alternate/update on exactly that path, silently booting internal
 * instead. This does mean a boot with truly no SD card ever inserted can
 * take up to 5s before falling through to the instant-boot
 * path below. The early boot background is already visible throughout this
 * bounded wait, without an extra status string (see main.c). */
#define SD_SETTLE_MAX_ATTEMPTS 50
#define SD_SETTLE_DELAY_MS 100

/* Mirrors src/main.c's own mount_sd_card_if_needed() (mount point
 * creation, already-mounted short-circuit, partition-node-then-whole-disk
 * fallback for a partition-less "superfloppy" card) -- see scanner.h's own
 * doc comment for why a bootloader needs its own copy of this at all --
 * wrapped in the settle retry above SD_SETTLE_MAX_ATTEMPTS documents. */
static void mount_sd_card_if_needed(void) {
    mkdir("/usr/data/mnt", 0755);
    mkdir(SD_MOUNT_POINT, 0755);

    for (int attempt = 0; attempt < SD_SETTLE_MAX_ATTEMPTS; attempt++) {
        if (sd_mount_point_mounted()) return;

        /* Gate each node on its own existence first -- attempting a mount
         * against a node that doesn't exist yet is pure wasted subprocess
         * spawns (vfat+exfat+ntfs each), and this loop already retries on
         * its own schedule regardless. */
        if (access(SD_DEVICE_NODE_PARTITION, F_OK) == 0) {
            try_mount_sd_device_node(SD_DEVICE_NODE_PARTITION);
            if (sd_mount_point_mounted()) return;
        }
        if (access(SD_DEVICE_NODE_WHOLE_DISK, F_OK) == 0) {
            try_mount_sd_device_node(SD_DEVICE_NODE_WHOLE_DISK);
            if (sd_mount_point_mounted()) return;
        }

        /* Neither node exists yet, or one exists but genuinely failed to
         * mount (corrupt filesystem, etc.) -- either way, keep retrying
         * rather than giving up on the very first miss; a node that will
         * never mount just harmlessly rides out the same bounded 5s as
         * one that's still settling. */
        usleep(SD_SETTLE_DELAY_MS * 1000);
    }
}

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

/* True only if both binaries have a discoverable BUILD_STAMP and sd_path's
 * is lexically greater than internal_path's -- "YYYY-MM-DD_HH:MM" sorts
 * correctly as plain text, no date parsing needed. Any missing/unreadable
 * stamp on either side is treated as "not newer", never guessed -- an SD
 * binary that isn't even a build of this app (no BUILD_STAMP at all) must
 * never be auto-adopted just because it happens to be named
 * open_hiby_player. */
static bool sd_build_is_newer(const char * internal_path, const char * sd_path) {
    char internal_stamp[BUILD_STAMP_LEN + 1];
    char sd_stamp[BUILD_STAMP_LEN + 1];
    if (!extract_build_stamp(internal_path, internal_stamp, sizeof(internal_stamp))) return false;
    if (!extract_build_stamp(sd_path, sd_stamp, sizeof(sd_stamp))) return false;
    return strcmp(sd_stamp, internal_stamp) > 0;
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

    if (out->sd_update_present) {
        out->sd_update_is_newer = sd_build_is_newer(INTERNAL_PLAYER_PATH, SD_UPDATE_PLAYER_PATH);
    }

    load_preferences(&out->default_entry, &out->timeout_seconds);
    loaded_timeout_seconds = out->timeout_seconds;
    /* Persisted choice is no longer reachable (SD removed, file deleted
     * since) -- fall back rather than defaulting a countdown to an entry
     * that can't actually be booted. */
    if (out->default_entry == BOOT_ENTRY_SD_STOCK && !out->sd_stock_present) {
        out->default_entry = BOOT_ENTRY_INTERNAL;
    } else if (out->default_entry == BOOT_ENTRY_SD_UPDATE && !out->sd_update_present) {
        out->default_entry = BOOT_ENTRY_INTERNAL;
    }

    /* An SD update build being present at all takes default priority over
     * whatever was separately persisted from an earlier boot -- dropping a
     * new open_hiby_player build on the card is itself a strong, deliberate
     * signal of intent to run it, stronger than a preference recorded
     * before that file ever existed. Only reached when it isn't ALSO
     * newer (see main()'s own decision tree) -- that case already
     * auto-boots it directly with no menu, so this is specifically the
     * "present, offered as a menu entry" path highlighting it as the
     * default rather than requiring it to be manually picked every time. */
    if (out->sd_update_present) {
        out->default_entry = BOOT_ENTRY_SD_UPDATE;
    }
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
