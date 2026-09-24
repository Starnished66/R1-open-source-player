#ifndef BOOTLOADER_SCANNER_H
#define BOOTLOADER_SCANNER_H

#include <stdbool.h>
#include <stddef.h>

/* Always-present internal player -- if this itself is somehow missing/not
 * executable, that is a fatal condition for the whole device, not
 * something scanner.c tries to recover from. */
#define INTERNAL_PLAYER_PATH "/usr/bin/compas_player"

/* User-provided Compás updates are kept under this app's own hidden config
 * directory on the SD card so they don't clutter a user's music folders.
 * A compas_player build dropped here triggers installer_run() to copy it
 * into INSTALLED_PLAYER_PATH (installer.h), on the writable partition,
 * before the internal player is launched. It is never executed directly
 * from the SD card; see installer.c's own top comment for why. */
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
#define SD_ALT_DIR "/usr/data/mnt/sd_0/.compas"
#define LEGACY_SD_ALT_DIR "/usr/data/mnt/sd_0/.open_hiby_player"

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
#define SD_UPDATE_PLAYER_PATH SD_ALT_DIR "/compas_player"
#define LEGACY_SD_UPDATE_PLAYER_PATH LEGACY_SD_ALT_DIR "/open_hiby_player"

#define BOOT_BUILD_STAMP_LEN 16 /* "YYYY-MM-DD_HH:MM" */

typedef struct {
    /* True if SD_UPDATE_PLAYER_PATH exists and is executable. Not itself a
     * boot destination (see this file's own doc comment on SD_ALT_DIR) --
     * main() passes this straight to installer_run(), which is what
     * actually acts on it. */
    bool sd_update_present;

    /* The executable selected for a pending update. New .compas storage is
     * preferred; the legacy path remains a one-release migration fallback. */
    const char *sd_update_path;

} scan_result_t;

/* Mounts the SD card (if not already mounted) exactly like src/main.c's
 * own mount_sd_card_if_needed() does -- vfat, then exfat, then ntfs-3g,
 * against SD_DEVICE_NODE_PARTITION then SD_DEVICE_NODE_WHOLE_DISK -- then
 * populates out with whether an SD Compás update is present for
 * installer_run() to act on. Never fails outright -- no SD card present or
 * an unreadable update binary simply leaves the internal player to boot.
 * Safe to call even if the real player later mounts the same card again
 * itself -- both check "already mounted" first. */
void scanner_scan(scan_result_t * out);

/* Reads INTERNAL_PLAYER_PATH's or INSTALLED_PLAYER_PATH's embedded
 * BUILD_STAMP (see this function's own doc comment in scanner.c for exactly
 * what counts as one and why the lexically-maximum match across the whole
 * file is what's returned) into out, NUL-terminated. Returns false (out
 * left untouched) if path has no such string at all -- callers must treat
 * that as "unknown version", never as "oldest possible version". */
bool scanner_read_build_stamp(const char * path, char * out, size_t out_size);

/* Returns true if path exists, is a regular file, and has execute permission. */
bool scanner_path_is_executable(const char * path);

#endif /* BOOTLOADER_SCANNER_H */
