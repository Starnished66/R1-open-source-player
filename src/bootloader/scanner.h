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
 * present on the same card at once -- each is its own independent
 * BOOT_ENTRY_* when present:
 *  - "hiby_player": a backed-up copy of the ORIGINAL stock firmware's
 *    player binary -- BOOT_ENTRY_SD_STOCK, always a manual menu entry
 *    when present.
 *  - "open_hiby_player": a build of THIS SAME app the user dropped on the
 *    SD card, e.g. ahead of a full firmware reflash -- BOOT_ENTRY_SD_UPDATE.
 *    When its embedded BUILD_STAMP compares strictly newer than the
 *    installed one, scanner_scan() also sets sd_update_is_newer, and
 *    main() auto-boots it directly only when the Stock alternate is absent
 *    (see that flag's own doc comment). If hiby_player is present, the menu
 *    is always shown and the newer Open Player remains its default entry.
 *    It is STILL a normal, present, selectable menu entry the rest of the
 *    time (same age or not comparable). When it
 *    compares strictly OLDER than the installed internal build, it is
 *    still present and selectable, but scanner_scan() no longer defaults
 *    to it -- the internal build having since overtaken it defaults the
 *    menu/countdown back to BOOT_ENTRY_INTERNAL instead (see
 *    default_entry's own doc comment). */
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
#define BOOT_ENTRY_SD_UPDATE 2
#define BOOT_BUILD_STAMP_LEN 16 /* "YYYY-MM-DD_HH:MM" */

typedef struct {
    /* True if SD_STOCK_PLAYER_PATH exists and is executable -- when false,
     * BOOT_ENTRY_SD_STOCK is not a valid choice and must not appear as a
     * menu entry. */
    bool sd_stock_present;

    /* True if SD_UPDATE_PLAYER_PATH exists and is executable -- when
     * false, BOOT_ENTRY_SD_UPDATE is not a valid choice and must not
     * appear as a menu entry. Independent of sd_update_is_newer below:
     * this is "does it exist at all", not "should it be auto-adopted". */
    bool sd_update_present;

    /* True if sd_update_present is also true AND its embedded BUILD_STAMP
     * compares strictly newer than INTERNAL_PLAYER_PATH's own -- see
     * scanner_scan()'s doc comment. The caller may boot it directly only
     * when sd_stock_present is false. Stock's presence always forces the
     * menu; this flag then keeps the newer SD Open Player selected by
     * default. */
    bool sd_update_is_newer;

    /* The inverse comparison, kept separate so an equal or unreadable
     * stamp is not guessed to be older. */
    bool sd_update_is_older;

    /* True only when both embedded stamps were readable. This separates an
     * exact match (Internal wins) from an unknown comparison (SD keeps its
     * established drop-in priority). */
    bool sd_update_build_comparable;

    /* Empty when the corresponding executable has no readable embedded
     * BUILD_STAMP. The menu displays the complete stamp on its own line. */
    char internal_build_stamp[BOOT_BUILD_STAMP_LEN + 1];
    char sd_update_build_stamp[BOOT_BUILD_STAMP_LEN + 1];

    /* The newest comparable Open Player: SD only when its build is newer;
     * an exact match gives Internal priority. A non-comparable SD build
     * retains SD-drop priority. Stock is never the automatic selection. This
     * is both the initially-highlighted menu entry and what a countdown
     * timeout confirms. */
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
 * populates out with the full boot decision: whether the SD "stock"
 * alternate exists (and therefore forces the menu), whether a newer SD
 * "update" build may be auto-adopted when Stock is absent, and the
 * computed default entry plus persisted timeout for the countdown. Never
 * fails outright -- no SD
 * card present at all, a missing/corrupt preference file, or an unreadable
 * candidate binary all degrade to "internal player, no menu, default
 * timeout" rather than blocking boot. Safe to call even if the real player
 * later mounts the same card again itself -- both check "already mounted"
 * first. */
void scanner_scan(scan_result_t * out);

/* Discards clean page-cache pages populated while extracting the SD Open
 * Player's build stamp. Call only when handing off to Stock; an Open Player
 * launch benefits from retaining those already-read executable pages. */
void scanner_drop_sd_update_cache(void);

/* Persists `entry` (a BOOT_ENTRY_* value) as the new default for the next
 * boot's countdown/highlighted entry -- called once boot_choice_path (see
 * main.c) is actually about to be handed off to, whether that was reached
 * by explicit user selection or an unattended timeout. Best-effort: a
 * failure to write is logged, never fatal (the device still boots either
 * way; only next boot's remembered default is affected). */
void scanner_save_last_boot(int entry);

#endif /* BOOTLOADER_SCANNER_H */
