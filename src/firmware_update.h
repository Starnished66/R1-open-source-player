#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

#include <stdbool.h>
#include <stddef.h>

/* Firmware update via the R1 Pro's own stock recovery mechanism -- confirmed
 * by reading the closed-source hiby_player binary's strings (references a
 * "%s/sd_0/%s.upt" path pattern and calls "/usr/bin/bootmode.sh Recovery")
 * and by inspecting /proc/mtd: kernel2/rootfs2 are a separate, smaller
 * partition pair (rootfs2 is roughly half rootfs's size), not a live A/B
 * twin of the running system -- they're a dedicated recovery image whose
 * whole job is to find a *.upt file (an ISO9660 image containing a chunked
 * kernel+rootfs payload, the exact same directory shape /etc/ota_bin's
 * network OTA scripts expect, just packaged as a mountable ISO instead of a
 * plain directory) on the SD card and flash it into the main kernel/rootfs
 * slot, then switch back and reboot again. Because that recovery partition
 * is untouched by this project's own repack, no custom flashing logic is
 * needed here at all -- only the same trigger the stock player uses. */

/* Scans the SD card root (not recursive -- matches the stock hiby_player's
 * own fixed "sd_0" + ".upt" lookup, not a user-organized folder) for the
 * first *.upt file. Writes its full path into out_path. Returns false if none
 * found. */
bool firmware_update_scan(char * out_path, size_t out_size);

/* Flips the boot flag to the recovery partition (kernel2/rootfs2) via
 * /usr/bin/bootmode.sh Recovery, then reboots. Does not return on success.
 * The recovery image handles the rest (finding and flashing the *.upt file,
 * switching the flag back to the main slot, rebooting again) entirely on
 * its own, exactly as it does for the stock firmware's own "Firmware
 * Update" menu item. */
void firmware_update_enter_recovery(void);

/* Checks whether Power + Volume Up are BOTH currently held down (via
 * EVIOCGKEY, which reads the device's live key-state bitmap rather than
 * waiting for a fresh press event -- by the time this runs, several seconds
 * into boot, the user is expected to already be holding both, so a normal
 * evdev press event would never arrive) and a *.upt file is present. If so,
 * calls firmware_update_enter_recovery() immediately with no confirmation
 * prompt -- this is the deliberate hold-at-boot recovery gesture, the same
 * kind of unprompted combo other devices use for the same purpose -- and
 * does not return. Meant to be called once, very early in main(), before
 * any display/GUI setup, so a held combo never even flashes the normal UI
 * on screen first. No-op (returns normally) if the combo isn't held or no
 * update file is present. */
void firmware_update_check_boot_combo(void);

#endif /* FIRMWARE_UPDATE_H */
