#include "lvgl/lvgl.h"
#include "audio.h"
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef HOST_BUILD
  #include "src/drivers/sdl/lv_sdl_window.h"
  #include "src/drivers/sdl/lv_sdl_mouse.h"
  #include "src/drivers/sdl/lv_sdl_keyboard.h"
#else
  #include "src/drivers/display/fb/lv_linux_fbdev.h"
  #include "src/drivers/evdev/lv_evdev.h"
  #include "input_device_utils.h"
  #include "hw_buttons.h"
  #include "firmware_update.h"
  #include "subprocess.h"
  #include "bt_media_player.h"
  #include <fcntl.h>
  #include <sys/ioctl.h>
  #include <sys/stat.h>
  #include <linux/fb.h>
#endif

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 800

/* Custom tick interface for LVGL timing (replaces older thread-based ticks) */
static uint32_t custom_tick_get(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

/* GUI initialization declaration */
extern void gui_init(uint32_t screen_width, uint32_t screen_height);

#ifndef HOST_BUILD
/* On a genuine cold boot, S92_03_start_music_player is the LAST init.d
 * script to run, so /dev/fb0 should already exist by then -- but the device
 * node existing doesn't guarantee the underlying driver is done settling;
 * open()/ioctl() on it can still transiently fail for a short window right
 * after S11jpeg_display_shell hands off. lv_linux_fbdev_set_file() doesn't
 * report failure back to its caller and silently leaves the framebuffer
 * unusable in that case, which then crashes on the very first render flush
 * -- and since hiby_player.sh runs us in the foreground and unconditionally
 * reboots once we exit, that crash becomes an invisible boot loop (red LED
 * repeatedly restarting, never reaching the UI). This was never caught by
 * interactive testing, since launching the binary by hand (adb, well after
 * the system had been fully booted for minutes) never hits the race. Poll
 * with a real open()+ioctl() readiness check instead of trusting the device
 * node's mere existence. */
static bool wait_for_fbdev_ready(const char * path, int max_attempts, int delay_ms) {
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            struct fb_var_screeninfo vinfo;
            bool ok = (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0);
            close(fd);
            if (ok) return true;
        }
        usleep(delay_ms * 1000);
    }
    return false;
}

/* Real-device incident: the SD card is never mounted at boot by anything in
 * the OS layer -- confirmed by reading every relevant piece of config on a
 * real device: /etc/mdev.conf has no rule matching mmcblk* (only external
 * USB mass-storage sd[a-z][0-9] insertion is handled there), /etc/fstab has
 * no entry for it at all, and none of the /etc/init.d/S* scripts touch it
 * either (S21mount_ubifs only mounts the separate internal UBIFS
 * partition). This app's very first genuine cold boot after fixing the
 * D-Bus-cleanup boot hang (see bluetooth_control.h's comment above
 * bt_control_is_powered()) came up with /data/mnt/sd_0 completely
 * unmounted -- MUSIC_ROOT_DIR (gui.c) is just an empty mountpoint until
 * something mounts the real card there, breaking both
 * firmware_update_check_boot_combo() (scans that path for a *.upt) and
 * metadata_db.c's stock-db warm-start import (reads
 * .../sd_0/usrlocal_media.db) if it runs first.
 *
 * The mount point directory itself doesn't exist either -- confirmed live:
 * /data/mnt/ was completely empty (no "sd_0" entry at all) on that same
 * cold boot, and a manual `mount` attempt failed with "No such file or
 * directory" until this ran `mkdir -p` first. Something else on the system
 * does appear to retry mounting the card reactively (confirmed via dmesg:
 * a successful FAT-fs mount landed at kernel uptime ~914s, many minutes
 * after boot, using different default options than specified here -- not
 * triggered by anything this function did) but silently keeps failing for
 * however long the target directory doesn't exist, since mount(8) never
 * creates missing mount points itself. Whatever that other mechanism is,
 * creating the directory ourselves, immediately at boot, is what actually
 * unblocks it (and our own explicit mount attempt right after covers the
 * case where nothing else would ever get around to it). Options match what
 * a working device's own `mount` output showed earlier this session: vfat,
 * fmask/dmask 0022, codepage 936 (CJK filename support), utf8 iocharset,
 * shortname=mixed. Best-effort via the same busybox `mount`
 * mass_storage_inserting.sh already uses elsewhere on this firmware for the
 * exact same reason (shelling out, not a raw syscall, to match this
 * codebase's own established convention) -- if the card's genuinely not
 * inserted, or something already mounted it, this just fails harmlessly and
 * startup proceeds with whatever's actually there.
 *
 * Real-device bug report: a card that shows up in the OS as detected but
 * never populates the library. The official HiBy R1 user manual explicitly
 * lists FAT32, exFAT *and* NTFS as supported card filesystems, but this
 * function only ever attempted -t vfat -- any exFAT/NTFS card would just
 * silently fail to mount here forever, exactly matching that report. There
 * is no mount.exfat/ntfs-3g/exfatprogs/ntfsprogs anywhere in this firmware's
 * rootfs (confirmed by searching it), but the same is true of vfat itself
 * (no vfat.ko/fat.ko either -- see module_driver/'s own contents), so FAT
 * support is compiled directly into this vendor's kernel image rather than
 * loaded as a module; exFAT/NTFS support, if the manual's claim is accurate,
 * is almost certainly built in the same way, needing nothing more than
 * picking the right -t. There's no reliable way to ask busybox's mount to
 * auto-detect the type here, so this just tries each supported type in turn
 * and stops at the first one that actually results in a mounted
 * filesystem. exFAT/NTFS use deliberately minimal options (just rw,relatime)
 * rather than reusing vfat's own tuned option set above -- fmask/dmask/
 * codepage/shortname are vfat-specific, and this exact kernel's exFAT/NTFS
 * driver (whatever it actually is under the hood) has never been inspected
 * directly, so guessing at option names it might reject risks turning a
 * mountable card into an unmountable one. */
static bool sd_mount_point_mounted(void) {
    struct stat parent_st, mnt_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat("/data/mnt/sd_0", &mnt_st) != 0) return false;
    return parent_st.st_dev != mnt_st.st_dev;
}

/* Tries all three supported filesystem types against one device node, same
 * "try each in turn, stop at the first that actually mounts" approach as
 * mount_sd_card_if_needed() itself -- factored out so that function can
 * call it twice: once for the partition node, once (see its own comment)
 * as a fallback for the whole-disk node. */
static void try_mount_sd_device_node(const char * device_node) {
    char * vfat_argv[] = { (char *) "mount", (char *) "-t", (char *) "vfat", (char *) "-o",
                            (char *) "rw,relatime,fmask=0022,dmask=0022,codepage=936,iocharset=utf8,shortname=mixed",
                            (char *) device_node, (char *) "/data/mnt/sd_0", NULL };
    subprocess_run(vfat_argv, NULL, 0);
    if (sd_mount_point_mounted()) return;

    char * exfat_argv[] = { (char *) "mount", (char *) "-t", (char *) "exfat", (char *) "-o",
                             (char *) "rw,relatime", (char *) device_node, (char *) "/data/mnt/sd_0", NULL };
    subprocess_run(exfat_argv, NULL, 0);
    if (sd_mount_point_mounted()) return;

    char * ntfs_argv[] = { (char *) "mount", (char *) "-t", (char *) "ntfs", (char *) "-o",
                            (char *) "rw,relatime", (char *) device_node, (char *) "/data/mnt/sd_0", NULL };
    subprocess_run(ntfs_argv, NULL, 0);
}

void mount_sd_card_if_needed(void) {
    mkdir("/data/mnt/sd_0", 0755);
    if (sd_mount_point_mounted()) return;

    try_mount_sd_device_node("/dev/mmcblk0p1");
    if (sd_mount_point_mounted()) return;

    /* Real-device bug report: a card that mounts fine on a PC but never
     * populates the library on this device. /dev/mmcblk0p1 (the first
     * partition) only ever gets a device node from the kernel if the card
     * actually has a partition table -- a card formatted as a "superfloppy"
     * (a filesystem written straight to the raw disk, no MBR/partition
     * table at all, which some SD formatting tools produce by default,
     * especially for smaller cards) never gets a p1 node, so every attempt
     * above was guaranteed to fail no matter which -t was tried. Falling
     * back to the whole-disk node here covers that case; gui.c's own
     * poll_sd_card_hotplug()/sd_card_base_device_present() already knew
     * about this scenario (see its own comment) but had no mount attempt to
     * fall back to before concluding the card needed reformatting -- this
     * is that missing attempt, tried before ever reaching that destructive
     * path. Harmless no-op for a normally-partitioned card: mount just
     * fails (whole-disk-as-filesystem on a partitioned card isn't a valid
     * filesystem), same "best effort, check the result" pattern as every
     * other attempt in this function. */
    try_mount_sd_device_node("/dev/mmcblk0");
}

/* Diagnostic instrumentation for the still-unresolved cold-boot failure:
 * this app has never been confirmed to reach the main loop on a genuine
 * flash+cold-boot cycle (only ever launched manually, well after the system
 * had already settled), and hiby_player.sh runs us in the foreground with
 * an unconditional `reboot` once we exit -- so a crash or hang anywhere in
 * startup is otherwise completely invisible, just a boot loop with no
 * diagnostic trail. This writes to /usr/data -- UBIFS, mounted early by
 * S21mount_ubifs well before we run, and not touched by a normal OS-only
 * firmware revert -- so whatever step this dies on is still readable via
 * adb after reverting to stock. Cold boot itself is now confirmed working
 * (see bluetooth_control.h's comment above bt_control_is_powered() for the
 * root cause and fix) -- kept around a while longer since it's still useful
 * for chasing any other startup-path issues that turn up, rather than
 * ripping it out immediately. Not static: gui_init() (gui.c) also calls
 * this directly, via its own extern declaration, since the hang this was
 * built to diagnose had moved further into startup than main.c alone could
 * see.
 *
 * Real-device incident: confirmed via this very log on a genuine cold boot
 * that the FIRST call (main entered) always succeeds, but even the SECOND
 * call ever made in the process's lifetime sometimes never completes --
 * with no other code at all between them (see main()'s own two calls back
 * to back). The original implementation did a full fopen(append)+fprintf+
 * fflush+fsync+fclose cycle on every single call, repeating directory/inode
 * lookup work each time; kept open across the whole process instead
 * (opened lazily on first use, reused after), so every call after the
 * first is just a raw write()+fdatasync() -- smaller surface for whatever
 * is contending for UBIFS/flash I/O at this exact moment in a genuine cold
 * boot (other init.d-started services still settling) to actually hang on,
 * though this doesn't fully rule out the boot logging itself being at risk
 * on this specific device -- if checkpoints are still missing after this
 * change, the app's own startup work, not the logging, is what's stuck. */
static int boot_debug_fd = -1;

void boot_checkpoint(const char * step) {
    if (boot_debug_fd < 0) {
        /* O_TRUNC, not O_APPEND -- this log's only purpose is diagnosing
         * *this* boot's startup sequence (see the block comment above), so
         * appending forever across the device's entire lifetime just grows
         * a file nobody reads past the most recent boot, for no benefit --
         * real, avoidable wear on this device's limited flash storage.
         * Opened once per process lifetime either way (see above), so this
         * naturally resets on every fresh boot without needing its own
         * rotation/truncation logic elsewhere. */
        boot_debug_fd = open("/usr/data/boot_debug.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (boot_debug_fd < 0) return;
    }
    char buf[160];
    int len = snprintf(buf, sizeof(buf), "[%u ms] %s\n", (unsigned) custom_tick_get(), step);
    if (len <= 0) return;
    ssize_t unused_result = write(boot_debug_fd, buf, (size_t) len);
    (void) unused_result;
    fdatasync(boot_debug_fd);
}
#endif

int main(void) {
    /* Real-device bug report: a Bluetooth disconnect during playback froze
     * the device and triggered a reboot, sometimes leaving Bluetooth
     * unrecoverable short of a long power-button press. Root cause: this
     * process never installed a SIGPIPE handler anywhere (confirmed by
     * grepping the whole codebase), so its default disposition -- immediate
     * process termination, no unwind, no cleanup -- was still in effect.
     * audio_output.c's Bluetooth output path (audio_output_write())
     * write()s decoded audio into a pipe feeding an `aplay -D bluealsa`
     * child; BlueZ tearing down the A2DP transport on a real disconnect
     * kills that child, and the very next write() into the now-reader-less
     * pipe raises SIGPIPE. On the real device, hiby_player.sh (the stock
     * launcher this binary replaces in place -- see README.md's own
     * real-device testing notes) runs `sleep 1; reboot` the instant
     * hiby_player exits for any reason, so a SIGPIPE kill here reads
     * exactly like "device reboot" -- and because the process dies
     * mid-write rather than exiting cleanly, whatever BT/UART transaction
     * was in flight at that exact instant can be left wedged badly enough
     * that even bt_resume can't recover it without a hard power cycle,
     * matching the "Bluetooth not recoverable" half of the report too.
     * Ignoring SIGPIPE process-wide is the standard fix for this class of
     * bug: write()/send() calls into a broken pipe/socket then just return
     * -1/EPIPE instead of killing the process -- audio_output_write()
     * already handles exactly that return value correctly (breaks its
     * retry loop, lets the next audio_output_ensure() reopen), so this
     * lets that existing, already-correct fallback path actually run
     * instead of being pre-empted by a fatal signal first. Must run before
     * anything else below gets a chance to open a subprocess pipe. */
    signal(SIGPIPE, SIG_IGN);

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("Starting open_hiby_player...\n");

#ifndef HOST_BUILD
    boot_checkpoint("main entered");

    /* Real-device incident: an earlier library audit of the read-only
     * squashfs rootfs (documented in the user's own repack staging tree,
     * not this repo) removed /usr/lib/libldacdec.so as "unused" -- wrong:
     * /usr/bin/bluealsa links it unconditionally (DT_NEEDED), regardless of
     * which A2DP codec actually gets negotiated at runtime, so bluealsa
     * failed to even start ("error while loading shared libraries"), which
     * silently broke every Bluetooth audio profile (A2DP, AVRCP) with no
     * error surfaced anywhere in this app -- confirmed live via bluetoothd's
     * own debug log showing `a2dp-sink profile connect failed: Protocol not
     * available`, several layers removed from the actual cause. / (the
     * squashfs image) can't be written to at runtime to restore the file in
     * place, so this instead adds /usr/data (the writable ubifs partition)
     * as a fallback library search path -- a copy of the restored .so lives
     * there for now. Harmless even once the rootfs image itself is properly
     * fixed on the next reflash: ld.so just never finds anything to load
     * from here at that point, this line becomes a no-op. */
    setenv("LD_LIBRARY_PATH", "/usr/data/lib", 1);

    /* Must run before firmware_update_check_boot_combo() and gui_init() --
     * both read from the SD card, which starts unmounted on a genuine cold
     * boot (see mount_sd_card_if_needed()'s own comment for why). */
    mount_sd_card_if_needed();
    boot_checkpoint("mount_sd_card_if_needed done");

    /* No startup-time Bluetooth/D-Bus cleanup call here -- see the comment
     * above bt_control_is_powered() in bluetooth_control.h for why: real-
     * device testing (three rounds of fixing real bugs in a bounded version
     * of it, none of which changed where or whether boot succeeded, then
     * confirmed live that just not calling it at all fixes cold boot
     * completely) showed doing that work at this exact point in boot -- not
     * just being slow at it -- reliably hung this app before it ever
     * reached the main loop. */

    /* Must run before any display/GUI setup -- if Power+Volume Up are held
     * (the deliberate recovery-boot gesture) and a *.upt update file is on
     * the SD card, this reboots straight into the recovery partition and
     * never returns, so the normal UI should never even flash on screen
     * first. See firmware_update.h for why no confirmation prompt is needed
     * here (a boot-time hold is already an unambiguous, deliberate act). */
    firmware_update_check_boot_combo();
    boot_checkpoint("firmware_update_check_boot_combo done");
#endif

    /* 1. Initialize LVGL core */
    lv_init();

    /* Register the custom tick source */
    lv_tick_set_cb(custom_tick_get);
#ifndef HOST_BUILD
    boot_checkpoint("lv_init done");
#endif

#ifdef HOST_BUILD
    printf("Initializing Host Build (SDL2 Simulation at %dx%d)...\n", SCREEN_WIDTH, SCREEN_HEIGHT);

    /* Create SDL2 window and display */
    lv_display_t * disp = lv_sdl_window_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!disp) {
        fprintf(stderr, "Error: Failed to create SDL2 window\n");
        return 1;
    }

    /* Register Mouse as Pointer device (maps mouse click -> touch) */
    lv_indev_t * mouse = lv_sdl_mouse_create();
    if (mouse) {
        lv_indev_set_display(mouse, disp);
    }

    /* Register Keyboard as Keypad device */
    lv_indev_t * kbd = lv_sdl_keyboard_create();
    if (kbd) {
        lv_indev_set_display(kbd, disp);
    }
#else
    printf("Initializing Target Build (Linux Framebuffer and EVDEV touch)...\n");

    /* Create Framebuffer display */
    lv_display_t * disp = lv_linux_fbdev_create();
    if (!disp) {
        fprintf(stderr, "Error: Failed to create Linux framebuffer display\n");
        boot_checkpoint("lv_linux_fbdev_create FAILED, returning 1");
        return 1;
    }
    boot_checkpoint("lv_linux_fbdev_create done");
    if (!wait_for_fbdev_ready("/dev/fb0", 50, 100)) {
        fprintf(stderr, "Warning: /dev/fb0 not ready after 5s of retries, proceeding anyway...\n");
    }
    boot_checkpoint("wait_for_fbdev_ready done");
    lv_linux_fbdev_set_file(disp, "/dev/fb0");
    boot_checkpoint("lv_linux_fbdev_set_file done");

    /* Create Touch Input device via evdev. Auto-detect the touch controller
     * by name first (works on the R1's Hynitron "hyn_ts"); fall back to
     * guessing event0/event1 for variants where that lookup doesn't match. */
    lv_indev_t * touch = NULL;
    char touch_path[64];
    if (find_input_device_by_name("hyn_ts", touch_path, sizeof(touch_path))) {
        printf("Detected touch controller at %s\n", touch_path);
        touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    }
    boot_checkpoint("touch auto-detect attempt done");
    if (!touch) {
        fprintf(stderr, "Warning: Touch controller not auto-detected. Guessing /dev/input/event0...\n");
        snprintf(touch_path, sizeof(touch_path), "/dev/input/event0");
        touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    }
    if (!touch) {
        fprintf(stderr, "Warning: Failed to open /dev/input/event0. Trying event1...\n");
        snprintf(touch_path, sizeof(touch_path), "/dev/input/event1");
        touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_path);
    }
    boot_checkpoint("touch setup done");

    if (touch) {
        lv_indev_set_display(touch, disp);
        printf("Touch screen input driver successfully registered.\n");
    } else {
        fprintf(stderr, "Warning: No touch input device found.\n");
    }

    /* Physical volume/skip/play-pause buttons: a separate poller thread,
     * since they're media-key shortcuts (act regardless of what's focused),
     * not keypad navigation for the UI. */
    hw_buttons_init();
    boot_checkpoint("hw_buttons_init done");
#endif

    /* 2. Initialize audio playback and the application GUI */
    audio_init();
#ifndef HOST_BUILD
    boot_checkpoint("audio_init done");

    /* AVRCP transport-button support (see bt_media_player.c's own
     * top-of-file comment) -- connects to the system bus and starts its
     * own dispatch thread now, for the app's whole lifetime, same as
     * audio_init() just above. Doesn't register with BlueZ yet (that only
     * happens once Bluetooth output is actually in use, see
     * bt_media_player_set_active() in gui.c's poll_refresh_bt_icon()), so
     * it's safe to call unconditionally at startup regardless of whether
     * Bluetooth is even on. */
    bt_media_player_init();
    boot_checkpoint("bt_media_player_init done");
#endif
    gui_init(SCREEN_WIDTH, SCREEN_HEIGHT);
#ifndef HOST_BUILD
    boot_checkpoint("gui_init done");
#endif

    /* 3. Main event loop */
    printf("Entering main event loop...\n");
#ifndef HOST_BUILD
    boot_checkpoint("entering main loop");
#endif
    while(1) {
        uint32_t time_till_next = lv_timer_handler();
        usleep(time_till_next * 1000); /* Convert milliseconds to microseconds */
    }

    return 0;
}
