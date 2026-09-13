#ifndef BOOTLOADER_SCANNER_H
#define BOOTLOADER_SCANNER_H

#include <stdbool.h>
#include <stddef.h>

/* Always-present internal player -- if this itself is somehow missing/not
 * executable, that is a fatal condition for the whole device, not
 * something scanner.c tries to recover from. */
#define INTERNAL_PLAYER_PATH "/usr/bin/open_hiby_player"

/* Rockbox-style user-provided alternates, kept under this app's own hidden
 * config directory on the SD card so they don't clutter a user's music
 * folders. Two distinct names, two distinct purposes, and BOTH can be
 * present on the same card at once:
 *  - "hiby_player": a backed-up copy of the ORIGINAL stock firmware's
 *    player binary -- BOOT_ENTRY_SD_STOCK, always a manual menu entry
 *    when present. Still executed directly from the SD card -- a
 *    completely separate concern from open_hiby_player below, and not
 *    something this update mechanism touches.
 *  - "open_hiby_player": a build of THIS SAME app the user dropped on the
 *    SD card, e.g. ahead of a full firmware reflash. NEVER executed
 *    directly from here -- see installer.c's own top comment for why.
 *    Its mere presence (scan_result_t's own sd_update_present) instead
 *    triggers installer_run() to copy it into INSTALLED_PLAYER_PATH
 *    (installer.h), on the writable partition, before any boot decision is
 *    made; it is not a selectable boot destination in its own right. */
/* This app's own runtime code mostly refers to this same location as
 * "/data/mnt/sd_0" ("/data" is a symlink to "usr/data", confirmed
 * on-device -- see settings.h's own comment on Factory Reset's "mnt"
 * carve-out). That symlink is already resolvable this early in boot --
 * proven by src/main.c's own mount_sd_card_if_needed() using it
 * successfully at the very same S92 init position a bootloader would also
 * run at -- so this isn't worked around here for timing reasons. Using
 * the fully-resolved, non-symlinked path is just as correct either way
 * (same underlying mount either name reaches it through) and reads more
 * obviously self-contained in a component that, unlike the rest of this
 * app, has no other file to point back to for context. */
#define SD_ALT_DIR "/usr/data/mnt/sd_0/.open_hiby_player"

/* Nothing before this app's own main() mounts the SD card -- confirmed via
 * src/main.c's mount_sd_card_if_needed(), which the real player calls
 * itself, right after its own boot_checkpoint(), specifically because nothing
 * earlier in the boot sequence has done it yet. A bootloader inserted before
 * that player ever runs is, by construction, "before" that point too: it
 * must mount the card itself before scanning SD_ALT_DIR, or a normal boot
 * will never see an alternate player that is genuinely present. See
 * scanner_scan()'s own doc comment. */
#define SD_DEVICE_NODE_PARTITION "/dev/mmcblk0p1"
#define SD_DEVICE_NODE_WHOLE_DISK "/dev/mmcblk0"
#define SD_MOUNT_POINT "/usr/data/mnt/sd_0"
#define SD_STOCK_PLAYER_PATH SD_ALT_DIR "/hiby_player"
#define SD_UPDATE_PLAYER_PATH SD_ALT_DIR "/open_hiby_player"

/* Same /usr/data root and naming convention as settings.c's own
 * SETTINGS_FILE_PATH ("/usr/data/open_hiby_player_settings.txt") -- the
 * one writable partition, never the read-only squashfs rootfs. Plain
 * "key=value" lines, not real INI (no sections) -- one small integer per
 * line does not need a real parser. */
#define BOOT_PREF_PATH "/usr/data/open_hiby_bootloader_preference.txt"

#define BOOT_ENTRY_INTERNAL 0
#define BOOT_ENTRY_SD_STOCK 1
#define BOOT_BUILD_STAMP_LEN 16 /* "YYYY-MM-DD_HH:MM" */

typedef struct {
    /* True if SD_STOCK_PLAYER_PATH exists and is executable -- when false,
     * BOOT_ENTRY_SD_STOCK is not a valid choice and must not appear as a
     * menu entry. */
    bool sd_stock_present;

    /* True if SD_UPDATE_PLAYER_PATH exists and is executable. Not itself a
     * boot destination (see this file's own doc comment on SD_ALT_DIR) --
     * main() passes this straight to installer_run(), which is what
     * actually acts on it. */
    bool sd_update_present;

    /* Empty when INTERNAL_PLAYER_PATH has no readable embedded BUILD_STAMP.
     * The menu displays the complete stamp on the Internal card's own line;
     * main() overwrites this with the currently-installed copy's own stamp
     * (scanner_read_build_stamp()) when installer_internal_player_path()
     * resolves to INSTALLED_PLAYER_PATH instead. */
    char internal_build_stamp[BOOT_BUILD_STAMP_LEN + 1];

    /* Always BOOT_ENTRY_INTERNAL -- Stock is never the automatic selection
     * (a user must actively pick it every time; see scanner_scan()'s own
     * doc comment). This is both the initially-highlighted menu entry and
     * what an unattended countdown timeout confirms. */
    int default_entry;

    /* Seconds before an unattended timeout confirms default_entry.
     * Clamped to a sane range by scanner_scan() itself (see its .c file)
     * so a corrupt or hand-edited preference file can't produce an
     * effectively-infinite or effectively-zero countdown. */
    int timeout_seconds;
} scan_result_t;

/* Mounts the SD card (if not already mounted) exactly like src/main.c's
 * own mount_sd_card_if_needed() does -- vfat, then exfat, then ntfs-3g,
 * against SD_DEVICE_NODE_PARTITION then SD_DEVICE_NODE_WHOLE_DISK -- then
 * populates out with whether the SD "stock" alternate exists (and therefore
 * forces the menu), whether an SD "update" build is present (for
 * installer_run() to act on), the hardcoded default entry (BOOT_ENTRY_INTERNAL),
 * and the persisted timeout for the countdown loaded from preferences.
 * Never fails outright -- no SD card present at all, a missing/corrupt preference
 * file, or an unreadable candidate binary all degrade to "internal player, no
 * menu, default timeout" rather than blocking boot. Safe to call even if the
 * real player later mounts the same card again itself -- both check "already
 * mounted" first. */
void scanner_scan(scan_result_t * out);

/* Reads INTERNAL_PLAYER_PATH's or INSTALLED_PLAYER_PATH's embedded
 * BUILD_STAMP (see this function's own doc comment in scanner.c for exactly
 * what counts as one and why the lexically-maximum match across the whole
 * file is what's returned) into out, NUL-terminated. Returns false (out
 * left untouched) if path has no such string at all -- callers must treat
 * that as "unknown version", never as "oldest possible version". */
bool scanner_read_build_stamp(const char * path, char * out, size_t out_size);

/* Discards clean page-cache pages populated while checksumming/copying the
 * SD Open Player during installer_run(). Call only when handing off to
 * Stock; an Open Player launch benefits from retaining those already-read
 * pages. */
void scanner_drop_sd_update_cache(void);

/* Returns true if path exists, is a regular file, and has execute permission. */
bool scanner_path_is_executable(const char * path);

#endif /* BOOTLOADER_SCANNER_H */
