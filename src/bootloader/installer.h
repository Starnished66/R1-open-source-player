#ifndef BOOTLOADER_INSTALLER_H
#define BOOTLOADER_INSTALLER_H

#include <stdbool.h>

#include "scanner.h"

/* The durable, internal-storage copy that an SD update binary gets installed
 * into -- see installer.c's own top comment for why this app must never run
 * straight off the SD card (SD_UPDATE_PLAYER_PATH, scanner.h) and only ever
 * runs from here or from the read-only squashfs INTERNAL_PLAYER_PATH. The
 * legacy path below is retained so an older installed build cannot override
 * a newer packaged build during the .compas transition. */
#define INSTALLED_PLAYER_PATH "/usr/data/compas_player"
#define LEGACY_INSTALLED_PLAYER_PATH "/usr/data/open_hiby_player"

/* If scan->sd_update_present, copies the selected update path (new .compas
 * preferred, legacy path accepted) to
 * INSTALLED_PLAYER_PATH via a validated, fsync'd temp-file-then-rename
 * sequence, then deletes the SD copy -- only once the install is fully
 * durable. A no-op when scan->sd_update_present is false (the ordinary case
 * on every boot with no pending update). Draws a plain "updating" frame via
 * fb_draw.h when fb_ready, since the copy/validate steps can take a
 * noticeable moment and the SD card must not be removed mid-copy.
 *
 * Never fails the boot: on any error (unreadable SD file, no space, a copy
 * that comes up short or corrupt, a failed rename) this logs to stderr and
 * returns having changed nothing durable -- the SD update file is left in
 * place for another attempt next boot, and whatever was previously
 * installed (or the squashfs fallback, on a device that has never installed
 * anything) remains exactly as bootable as it was before this call. */
void installer_run(const scan_result_t * scan, bool fb_ready);

/* Selects between arbitrary packaged/installed player paths using the same
 * policy as installer_internal_player_path(). Exposed for host regression
 * tests; returned pointers are exactly one of the two input pointers. */
const char * installer_select_internal_player(const char * packaged, const char * installed);

/* The player to boot as "the internal copy." Normally preserves the prior
 * INSTALLED_PLAYER_PATH-first behavior, but removes that override and selects
 * INTERNAL_PLAYER_PATH when both have trustworthy build stamps and the
 * packaged player is strictly newer. Call after installer_run() so a
 * just-completed install is reflected on the same boot. */
const char * installer_internal_player_path(void);

#endif /* BOOTLOADER_INSTALLER_H */
