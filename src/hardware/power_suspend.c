#include "power_suspend.h"
#include "bluetooth_control.h"
#include "debug_log.h"
#include "subprocess.h"
#include "wifi_control.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Diagnostic-only, for the double-press-to-wake investigation: independent
 * of gui.c's lv_tick_get() (that clock isn't accessible from here, and
 * shouldn't need to be -- CLOCK_MONOTONIC is directly comparable against
 * hw_buttons.c's own monotonic_ms() helper since both read the same clock),
 * lets the two threads' log lines be lined up on one shared timeline. */
#ifdef TEST_BUILD_TAG
static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

static void write_sysfs(const char * path, const char * value) {
    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s", value);
    fclose(f);
}

/* Same write, but reports success/failure instead of silently no-opping --
 * used only for /sys/power/state below. write_sysfs() above swallows a
 * fopen() failure entirely (wrong permissions, "mem" rejected by this
 * kernel's own /sys/power/state, anything already holding the transition
 * open); for the fb0 blank writes that's harmless cosmetically, but for the
 * actual suspend request it would make a suspend attempt that never
 * happened at all look identical, from update_timer_cb()'s side, to a
 * normal quick wake -- idle_shutdown_attempted resets to false either way
 * and the next 500ms tick just tries again, forever, while the fb blank/
 * unblank pair and this whole function still run every cycle. That is a
 * second, purely-software way to produce "considerable idle battery drain,
 * device isn't suspending" with no wakeup IRQ involved at all -- distinct
 * from the pm_wakeup_irq path below, and telling the two apart on a real
 * device needs to know whether this write is even succeeding. Same fopen/
 * fprintf/fclose cost as write_sysfs() either way (the DBG_LOG calls below
 * compile to nothing outside TEST_BUILD_TAG and, per debug_log.h, don't
 * even evaluate their arguments then), so this doesn't need its own
 * TEST_BUILD_TAG gate the way log_suspend_diagnostics() below does. */
static bool write_sysfs_checked(const char * path, const char * value) {
    FILE * f = fopen(path, "w");
    if (!f) {
        DBG_LOG("power_suspend: fopen(%s) failed: %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fprintf(f, "%s", value) >= 0;
    if (!ok) DBG_LOG("power_suspend: write to %s failed: %s\n", path, strerror(errno));
    if (fclose(f) != 0 && ok) {
        DBG_LOG("power_suspend: fclose(%s) failed: %s\n", path, strerror(errno));
        ok = false;
    }
    return ok;
}

#ifdef TEST_BUILD_TAG
/* CLOCK_MONOTONIC does not advance across Linux suspend-to-RAM (same fact
 * battery.c's own battery_clock_id() is built around, confirmed kernel/
 * POSIX behavior, not device-specific) -- an actual 8-hour suspend would
 * report as a few milliseconds "slept" if this reused monotonic_ms() above,
 * which is deliberately CLOCK_MONOTONIC for a DIFFERENT reason (lining up
 * with hw_buttons.c's own monotonic_ms() for the double-press-to-wake
 * investigation, where only ordering of events while the CPU is awake
 * matters, never a span across the sleep itself). CLOCK_BOOTTIME is
 * identical except it DOES include suspended time; probed once and cached
 * rather than assumed, falling back to CLOCK_MONOTONIC if it's ever
 * unavailable, matching battery_clock_id()'s exact pattern. */
static clockid_t suspend_duration_clock_id(void) {
    static clockid_t cached = CLOCK_MONOTONIC;
    static bool probed = false;
    if (!probed) {
        struct timespec ts;
        cached = (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) ? CLOCK_BOOTTIME : CLOCK_MONOTONIC;
        probed = true;
    }
    return cached;
}

static uint32_t suspend_duration_ms_now(void) {
    struct timespec ts;
    clock_gettime(suspend_duration_clock_id(), &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Diagnostic-only, for the "battery drain at idle" investigation: real-user
 * bug report is that idle battery drain is considerable and the device is
 * "either not suspending to RAM properly or something is waking it up
 * constantly." Nothing in this file (or gui.c's caller, see its own comment
 * on power_suspend_now() legitimately returning "without a real user wake,
 * e.g. a spurious IRQ") ever masks Linux wakeup-source IRQs before writing
 * "mem" to /sys/power/state -- so any IRQ line the kernel/DTS already marks
 * wakeup-capable (touch controller, PMIC/charger IRQ, etc., not just the
 * power button) can resume the device on its own. That can't be fixed here
 * without real-device data: guessing which specific wakeup source to
 * disable risks silently breaking the one that's supposed to work (e.g. the
 * physical power button) on hardware this can't be tested against from a
 * host build. This only measures it. pm_wakeup_irq (Linux kernel ABI,
 * Documentation/ABI/testing/sysfs-power: readable after a resume, holds the
 * IRQ number responsible for the last wakeup, or is empty/ENODATA if the
 * kernel can't attribute one) turns a repro from "screen came back on" into
 * a concrete IRQ number a real-device log (TEST_BUILD_TAG build, over ADB)
 * can be grepped for -- and paired with the actual time-asleep figure below,
 * distinguishes "kept re-waking every few seconds" (a wakeup-source problem)
 * from "stayed asleep the whole time but still drained" (a different
 * problem: this SoC's "mem" state itself not cutting standby draw much,
 * independent of any wakeup event). Read-only: cannot itself change suspend
 * or wake behavior, so there's no regression risk in adding it. The whole
 * function -- not just its DBG_LOG call -- is gated on TEST_BUILD_TAG:
 * unlike write_sysfs_checked() above, this does real extra work every
 * resume (a second sysfs open/read) that a production build has no reader
 * for and shouldn't pay for. */
static void log_suspend_diagnostics(uint32_t slept_ms, bool suspend_write_ok) {
    static unsigned int suspend_cycle_count = 0;
    suspend_cycle_count++;
    char wakeup_irq[32] = "";
    FILE * f = fopen("/sys/power/pm_wakeup_irq", "r");
    if (f) {
        if (!fgets(wakeup_irq, sizeof(wakeup_irq), f)) wakeup_irq[0] = '\0';
        fclose(f);
    }
    size_t irq_len = strlen(wakeup_irq);
    if (irq_len > 0 && wakeup_irq[irq_len - 1] == '\n') wakeup_irq[irq_len - 1] = '\0';
    DBG_LOG("power_suspend: cycle #%u slept %ums, write_ok=%d, pm_wakeup_irq=%s\n", suspend_cycle_count, slept_ms,
            suspend_write_ok, wakeup_irq[0] ? wakeup_irq : "(unavailable)");
}
#endif /* TEST_BUILD_TAG */

typedef struct {
    bool wifi_was_on;
    bool bt_was_on;
} radio_restore_args_t;

/* Real-device bug report: waking from suspend with Bluetooth on took up to
 * ~10s for the screen to come back -- root cause, this ran inline on the
 * caller's thread (the main GUI thread, since power_suspend_now() is always
 * called directly from update_timer_cb per its own header comment), and
 * bt_control_init_chip() is documented (bluetooth_control.h) to block
 * ~10-13s for chip firmware reflashing, "always call this off the UI
 * thread" -- which this violated. The whole rest of the UI, including the
 * code that flips the backlight back on right after power_suspend_now()
 * returns, was frozen behind it. Restoring the radios in their own detached
 * thread instead lets the caller return (and the screen come back) the
 * instant the kernel itself resumes, while BT/WiFi quietly catch up in the
 * background -- same "fire and forget" shape as hw_buttons_init(). */
static void * radio_restore_thread_func(void * arg) {
    radio_restore_args_t * args = (radio_restore_args_t *) arg;
    if (args->bt_was_on) {
        bt_control_init_chip();
        bt_control_enable();
    }
    if (args->wifi_was_on) wifi_control_enable();
    free(args);
    return NULL;
}

void power_suspend_now(void) {
    bool wifi_was_on = wifi_control_is_enabled();
    bool bt_was_on = bt_control_is_powered();

    if (wifi_was_on) wifi_control_disable();

    /* Real-device bug report: suspending with an actively-connected
     * Bluetooth output reboots the device, the same watchdog-driven
     * mechanism suspected for the BT-disconnect-freeze bug (see
     * bt_media_player.c/bluetooth_control.c's own concurrency-fix
     * comments). bt_suspend (below) is the raw radio teardown --
     * bluetooth_control.c's own bt_control_disable() doc comment already
     * confirms it "fully tears down the chip's UART firmware link (kills
     * brcm_patchram_plus/hciattach, rfkill block)" -- fine for a genuinely
     * idle radio, but yanking that transport out from under a live,
     * actively-streaming ACL/SCO link (rather than a clean BlueZ-level
     * disconnect) is a plausible way to wedge the kernel driver hard
     * enough to trip the same watchdog. bt_control_disable() first goes
     * through BlueZ's own D-Bus adapter power-off, which cleanly
     * disconnects any connected device as part of normal adapter
     * shutdown, before the raw UART teardown below ever runs. */
    if (bt_was_on) bt_control_disable();

    char * bt_suspend_argv[] = { (char *) "/usr/bin/bt_suspend", NULL };
    subprocess_run(bt_suspend_argv, NULL, 0);

    write_sysfs("/sys/class/graphics/fb0/blank", "4"); /* FB_BLANK_POWERDOWN */

    DBG_LOG("power_suspend: entering mem sleep at t=%u\n", monotonic_ms());
#ifdef TEST_BUILD_TAG
    uint32_t sleep_start_ms = suspend_duration_ms_now();
#endif
    bool suspend_write_ok = write_sysfs_checked("/sys/power/state", "mem"); /* blocks here until the device wakes back up, if it actually took */
#ifdef TEST_BUILD_TAG
    log_suspend_diagnostics(suspend_duration_ms_now() - sleep_start_ms, suspend_write_ok);
#else
    (void) suspend_write_ok;
#endif
    DBG_LOG("power_suspend: returned from mem sleep at t=%u\n", monotonic_ms());

    write_sysfs("/sys/class/graphics/fb0/blank", "0"); /* FB_BLANK_UNBLANK */

    if (bt_was_on || wifi_was_on) {
        radio_restore_args_t * args = malloc(sizeof(*args));
        args->wifi_was_on = wifi_was_on;
        args->bt_was_on = bt_was_on;
        pthread_t restore_thread;
        pthread_create(&restore_thread, NULL, radio_restore_thread_func, args);
        pthread_detach(restore_thread);
    }
}
