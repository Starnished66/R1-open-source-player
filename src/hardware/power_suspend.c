#include "power_suspend.h"
#include "bluetooth_control.h"
#include "debug_log.h"
#include "subprocess.h"
#include "wifi_control.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Diagnostic-only, for the double-press-to-wake investigation: independent
 * of gui.c's lv_tick_get() (that clock isn't accessible from here, and
 * shouldn't need to be -- CLOCK_MONOTONIC is directly comparable against
 * hw_buttons.c's own monotonic_ms() helper since both read the same clock),
 * lets the two threads' log lines be lined up on one shared timeline. */
static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void write_sysfs(const char * path, const char * value) {
    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s", value);
    fclose(f);
}

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
    write_sysfs("/sys/power/state", "mem"); /* blocks here until the device wakes back up */
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
