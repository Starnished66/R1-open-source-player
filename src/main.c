#include "lvgl/lvgl.h"
#include "audio.h"
#include "backlight.h"
#include "metadata.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <execinfo.h>

#include "gui.h"

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
  #include <fcntl.h>
  #include <sys/ioctl.h>
  #include <sys/stat.h>
  #include <linux/fb.h>
#endif

#include "board_config.h"

#define SCREEN_WIDTH BOARD_SCREEN_WIDTH
#define SCREEN_HEIGHT BOARD_SCREEN_HEIGHT

static volatile sig_atomic_t running = 1;

/* Custom tick interface for LVGL timing (replaces older thread-based ticks) */
static uint32_t custom_tick_get(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

#ifdef UI_PERF_TRACE
static uint64_t perf_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}
#endif

#ifdef HOST_BUILD
/* gui.h isn't included in the HOST_BUILD branch above (it pulls in
 * target-only headers transitively), so this is the only declaration
 * gui_init() gets there. */
extern void gui_init(uint32_t screen_width, uint32_t screen_height);
#endif

#ifndef HOST_BUILD
#include <ucontext.h>

/* Temporary investigation instrumentation for the "enabling/disabling
 * plugins triggers a reboot" report -- a live dmesg capture already
 * confirmed a real SIGSEGV inside musl's pthread_join() (invalid read from
 * a near-NULL offset, 0x18 into its internal thread struct), and gui_
 * reload.c/gui_library.c's own reload_diag()/library_teardown_diag() calls
 * already narrowed it to always landing right after gui_library_init()
 * finishes and before gui_network_init() starts -- but neither says WHICH
 * of this codebase's 70+ pthread_join() call sites, or whose thread
 * handle, is actually at fault. This handler writes pc/ra/sp and
 * pthread_join()'s own first two arguments (a0 = the bad handle itself,
 * a1 = its retval-out pointer) into the exact same reload_diag.log every
 * other diagnostic in this investigation already appends to, plus a raw
 * scan of the stack above sp for words that land inside this binary's own
 * static text range -- a poor man's backtrace, since -O3 doesn't keep
 * frame pointers here for a real one. Re-raises with the default handler
 * restored afterward so the bootloader's existing crash-reboot behavior
 * is unchanged. */
static void crash_diag_handler(int sig, siginfo_t * info, void * ucontext_v) {
    int fd = open("/data/mnt/sd_0/reload_diag.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ucontext_t * uc = (ucontext_t *) ucontext_v;
        unsigned long pc = (unsigned long) uc->uc_mcontext.pc;
        unsigned long ra = (unsigned long) uc->uc_mcontext.gregs[31];
        unsigned long sp = (unsigned long) uc->uc_mcontext.gregs[29];
        unsigned long a0 = (unsigned long) uc->uc_mcontext.gregs[4];
        unsigned long a1 = (unsigned long) uc->uc_mcontext.gregs[5];
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
            "[pid=%ld] *** CRASH sig=%d fault_addr=%p pc=%08lx ra=%08lx sp=%08lx a0=%08lx a1=%08lx ***\n",
            (long) getpid(), sig, info ? info->si_addr : NULL, pc, ra, sp, a0, a1);
        if (len > 0) { ssize_t r = write(fd, buf, (size_t) len); (void) r; }
        for (unsigned long addr = sp; addr < sp + 2048 && addr >= sp; addr += 4) {
            unsigned long word = *(unsigned long *) addr;
            if (word >= 0x400000UL && word < 0x9d0000UL) {
                len = snprintf(buf, sizeof(buf), "[pid=%ld]   stack[+%04lx] = %08lx\n",
                                (long) getpid(), addr - sp, word);
                if (len > 0) { ssize_t r = write(fd, buf, (size_t) len); (void) r; }
            }
        }
        fsync(fd);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Polls /dev/fb0 with a real open()+ioctl() check rather than trusting the
 * device node's mere existence -- the node can appear before the underlying
 * driver has finished settling, and using it too early crashes on the first
 * render flush. */
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

/* True if a real filesystem is mounted at /data/mnt/sd_0 (its st_dev differs
 * from its parent's), as opposed to just an empty directory sitting there. */
static bool sd_mount_point_mounted(void) {
    struct stat parent_st, mnt_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat("/data/mnt/sd_0", &mnt_st) != 0) return false;
    return parent_st.st_dev != mnt_st.st_dev;
}

/* Tries each supported filesystem type (vfat, exfat, NTFS) against one
 * device node in turn, stopping at the first that actually mounts.
 *
 * NTFS is intentionally mounted through the ntfs-3g binary included in the
 * firmware image.  This kernel/BusyBox combination has no usable in-kernel
 * `ntfs` mount path (and no mount.ntfs helper), so `mount -t ntfs` never
 * reached the driver that is actually shipped on the device. */
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

    char * ntfs_argv[] = { (char *) "/usr/bin/ntfs-3g", (char *) "-o",
                            (char *) "rw,relatime,big_writes,umask=0022",
                            (char *) device_node, (char *) "/data/mnt/sd_0", NULL };
    subprocess_run(ntfs_argv, NULL, 0);
}

/* Ensures the SD card is mounted at /data/mnt/sd_0. Creates the mount point
 * first (best-effort -- /data/mnt may not exist yet on a fresh boot), then
 * tries the card's partition node, falling back to the whole-disk node for
 * a partition-less ("superfloppy") card. */
void mount_sd_card_if_needed(void) {
    mkdir("/data/mnt", 0755);
    mkdir("/data/mnt/sd_0", 0755);
    if (sd_mount_point_mounted()) return;

    try_mount_sd_device_node("/dev/mmcblk0p1");
    if (sd_mount_point_mounted()) return;

    try_mount_sd_device_node("/dev/mmcblk0");
}

/* A software-triggered reboot can start this S92 process before the SD
 * block node has reappeared. The one early mount attempt above then misses
 * it, and plugin_manager_init() later sees no .plugins directory; unlike the
 * library, plugin-backed screens are built only once and cannot recover on
 * the later hotplug poll. Called immediately after painting the splash, so
 * this bounded wait overlaps the player's own splash and remains useful for
 * standalone test launches that bypass the bootloader.
 * Avoid running mount helpers until a real node exists. */
static void settle_sd_mount_during_splash(void) {
    const int attempts = 25;
    for (int i = 0; i < attempts; i++) {
        if (sd_mount_point_mounted()) return;
        if (access("/dev/mmcblk0p1", F_OK) == 0 || access("/dev/mmcblk0", F_OK) == 0) {
            mount_sd_card_if_needed();
            if (sd_mount_point_mounted()) return;
        }
        usleep(100000);
    }
}

/* Lightweight boot-time diagnostic log at /usr/data/boot_debug.log, readable
 * via adb even after a crash/reboot loop (hiby_player.sh reboots
 * unconditionally on any exit). Not static: gui_init() (gui.c) also calls
 * this directly via its own extern declaration. */
static int boot_debug_fd = -1;

void boot_checkpoint(const char * step) {
    if (boot_debug_fd < 0) {
        /* O_TRUNC, not O_APPEND -- only this boot's sequence matters, and
         * opened once per process lifetime so this naturally resets fresh
         * on every boot. */
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

// handler for signals that indicate crashes like SIGSEGV or SIGABRT
void crash_handler(int sig) {
    void *buffer[128];
    int size;

    size = backtrace(buffer, 128);

    // Write directly to stderr (safe in signal handlers)
    write(STDERR_FILENO, "Crashed! Backtrace:\n", 20);
    backtrace_symbols_fd(buffer, size, STDERR_FILENO);
    exit(1);
}

// handler for SIGINT
static void sigint_handler(int sig) {
	(void)sig;
	running = 0;

	// TODO: add more cleanup (like turning off the screen)
}

int main(int argc, char ** argv) {
    /* Ignore SIGPIPE process-wide: a Bluetooth disconnect during playback
     * kills the `aplay -D bluealsa` child audio_output.c writes into, and
     * without this the next write() would kill the whole process instead of
     * just returning EPIPE (which audio_output_write() already handles
     * correctly). Must run before anything else opens a subprocess pipe. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGINT, sigint_handler);  // interrupt handling (ctrl-c in terminal)

#ifndef HOST_BUILD
    struct sigaction crash_sa;
    memset(&crash_sa, 0, sizeof(crash_sa));
    crash_sa.sa_sigaction = crash_diag_handler;
    crash_sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &crash_sa, NULL);
    sigaction(SIGBUS, &crash_sa, NULL);
#endif

    if (argc == 4 && strcmp(argv[1], "--metadata-artwork-helper") == 0) {
        char * end = NULL;
        errno = 0;
        long fd = strtol(argv[2], &end, 10);
        if (errno != 0 || !end || *end != '\0' || fd < 0 || fd > INT_MAX) return 1;
        return metadata_artwork_helper_run(argv[3], (int) fd);
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("Starting open_hiby_player...\n");

#ifndef HOST_BUILD
    boot_checkpoint("main entered");

    /* Fallback library search path on the writable partition, for restoring
     * a shared library the read-only squashfs rootfs is missing without
     * needing a reflash (e.g. libldacdec.so, required by bluealsa). */
    setenv("LD_LIBRARY_PATH", "/usr/data/lib", 1);

    /* Must run before firmware_update_check_boot_combo() and gui_init() --
     * both read from the SD card. */
    mount_sd_card_if_needed();
    boot_checkpoint("mount_sd_card_if_needed done");

    /* Deliberately no startup-time Bluetooth/D-Bus cleanup call here -- see
     * bluetooth_control.h's comment above bt_control_is_powered(): doing
     * that work at this point in boot reliably hangs the app before it
     * reaches the main loop. */

    /* Must run before any display/GUI setup -- if Power+Volume Up are held
     * (the recovery-boot gesture) and a *.upt update file is on the SD
     * card, this reboots straight into recovery and never returns. */
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

    /* As early as this process can paint anything -- see gui_show_boot_
     * splash()'s own comment (gui.c) for why this exists and how gui_init()
     * further down uses the tick recorded here. */
    gui_show_boot_splash();
    boot_checkpoint("gui_show_boot_splash done");

    settle_sd_mount_during_splash();
    boot_checkpoint("settle_sd_mount_during_splash done");

    /* Create Touch Input device via evdev. Auto-detect the touch controller
     * by name first (works on the R1's Hynitron "hyn_ts"); fall back to
     * guessing event0/event1 for variants where that lookup doesn't match. */
    lv_indev_t * touch = NULL;
    char touch_path[64];
    if (find_input_device_by_name("hyn_ts", touch_path, sizeof(touch_path)) || find_input_device_by_name("goodix-ts", touch_path, sizeof(touch_path))) {
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
    /* LVGL normally calls its registered tick callback thousands of times
     * per second. On this old 32-bit kernel, musl implements each call as a
     * failing clock_gettime64 syscall followed by a legacy clock_gettime
     * fallback. Switch LVGL to its in-memory tick counter after setup and
     * advance it once per handler iteration from one real clock read. Timer
     * semantics remain identical: the tick is stable while one handler pass
     * runs, just like the conventional interrupt-driven lv_tick_inc model. */
    /* Preserve LVGL's exact clock domain across the handoff. Using a second
     * custom_tick_get() value as the internal-clock seed looked equivalent,
     * but real-device tracing showed the display's last_activity_time and
     * the newly seeded clock separated by ~3.88 billion ms (wraparound),
     * making screen timeout fire on the first runtime timer callback. */
    uint32_t lvgl_handoff_tick = lv_tick_get();
    uint32_t last_real_tick = custom_tick_get();
    lv_tick_inc(lvgl_handoff_tick);
    lv_tick_set_cb(NULL);
#ifndef HOST_BUILD
    /* Must be after lv_tick_set_cb(NULL): this baseline must belong to the
     * runtime clock that update_timer_cb will read, not the boot clock. */
    gui_reset_interactive_timeout_baseline();
#endif
#ifdef UI_PERF_TRACE
    uint32_t perf_report_tick = last_real_tick;
    uint64_t perf_handler_total_us = 0;
    uint64_t perf_handler_max_us = 0;
    unsigned perf_handler_calls = 0;
    unsigned perf_handler_over_16ms = 0;
#endif
    while(running) {
        uint32_t real_tick = custom_tick_get();
        lv_tick_inc(real_tick - last_real_tick);
        last_real_tick = real_tick;
#ifdef UI_PERF_TRACE
        uint64_t perf_handler_start_us = perf_now_us();
#endif
        uint32_t time_till_next = lv_timer_handler();
#ifdef UI_PERF_TRACE
        uint64_t perf_handler_us = perf_now_us() - perf_handler_start_us;
        perf_handler_total_us += perf_handler_us;
        if (perf_handler_us > perf_handler_max_us) perf_handler_max_us = perf_handler_us;
        perf_handler_calls++;
        if (perf_handler_us >= 16000) perf_handler_over_16ms++;
        if ((uint32_t) (real_tick - perf_report_tick) >= 1000) {
            printf("PERF main handler calls=%u avg_us=%llu max_us=%llu over16=%u next_ms=%u screen_on=%d\n",
                   perf_handler_calls,
                   (unsigned long long) (perf_handler_calls ? perf_handler_total_us / perf_handler_calls : 0),
                   (unsigned long long) perf_handler_max_us, perf_handler_over_16ms, time_till_next,
                   backlight_screen_is_on());
            perf_report_tick = real_tick;
            perf_handler_total_us = 0;
            perf_handler_max_us = 0;
            perf_handler_calls = 0;
            perf_handler_over_16ms = 0;
        }
#endif
        /* Real-device bug report: a screen (compact_list's own background-
         * fetch poll timer, screen_builders.c) went permanently blank after
         * scrolling -- confirmed via direct tracing that its fetch actually
         * completed, but the timer polling for that completion never fired
         * again. Root cause: lv_timer_handler() returns LV_NO_TIMER_READY
         * (0xFFFFFFFF, lv_timer.h) when no *currently unpaused* timer has
         * work due -- true here for a brief window whenever every other
         * active timer happens to be paused/idle at the same moment as this
         * one (e.g. right after a fetch completes and its own poll timer
         * re-pauses itself). `time_till_next * 1000` then silently
         * overflows this uint32_t, producing a ~71-minute usleep() instead
         * of the intended "sleep only until the next timer needs it" --
         * indistinguishable from a permanent freeze in normal testing.
         * Capping the sleep bounds the worst case to this constant instead,
         * while still being long enough that a genuinely idle app burns
         * negligible CPU -- and covers any future timer hitting the same
         * window, not just this one. */
        /* Screen-off CPU/battery optimization: while the backlight is off,
         * gui.c's own update_timer_cb() (still running every 500ms -- see
         * its own comment on why that keeps going regardless) is the only
         * timer that still needs prompt service; LVGL's display-refresh
         * timer is paused for the same duration (see gui.c's screen_just_
         * woke/screen_on_now handling), so there's nothing display-related
         * this loop needs to wake up quickly for. Raising the idle cap to
         * match that 500ms cadence while off (kept at 100ms while on, for
         * touch/animation responsiveness) is what actually cuts main-loop
         * wakeups -- backlight_screen_is_on() is the same shared source of
         * truth gui.c itself uses, so this can never disagree with the
         * paused/resumed state of the refresh timer. */
        uint32_t idle_cap_ms = backlight_screen_is_on() ? 100 : 500;
        if (time_till_next > idle_cap_ms) time_till_next = idle_cap_ms;
        usleep(time_till_next * 1000); /* Convert milliseconds to microseconds */
    }

    gui_deinit();
    return 0;
}
