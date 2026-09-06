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

/* Monotonic timestamp in milliseconds. Uses CLOCK_MONOTONIC to align with
 * hw_buttons timestamps on the same timeline. */
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

/* Writes a sysfs value and reports success/failure.
 * Used for /sys/power/state to detect if the write itself failed. */
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
/* CLOCK_MONOTONIC does not advance across Linux suspend-to-RAM.
 * CLOCK_BOOTTIME is used to measure elapsed time across suspend. */
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

/* Logs suspend duration and wakeup IRQ diagnostics for test builds. */
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

/* Restores Bluetooth and Wi-Fi in a detached thread after resume so that
 * the caller and display unblank are not blocked by radio re-initialization. */
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

    /* Cleanly disable Bluetooth through D-Bus before invoking raw bt_suspend
     * to ensure active links are gracefully disconnected. */
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
