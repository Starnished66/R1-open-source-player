#include "power_suspend.h"
#include "bluetooth_control.h"
#include "debug_log.h"
#include "subprocess.h"
#include "wifi_control.h"

#include <stdio.h>
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

void power_suspend_now(void) {
    bool wifi_was_on = wifi_control_is_enabled();
    bool bt_was_on = bt_control_is_powered();

    if (wifi_was_on) wifi_control_disable();

    char * bt_suspend_argv[] = { (char *) "/usr/bin/bt_suspend", NULL };
    subprocess_run(bt_suspend_argv, NULL, 0);

    write_sysfs("/sys/class/graphics/fb0/blank", "4"); /* FB_BLANK_POWERDOWN */

    DBG_LOG("power_suspend: entering mem sleep at t=%u\n", monotonic_ms());
    write_sysfs("/sys/power/state", "mem"); /* blocks here until the device wakes back up */
    DBG_LOG("power_suspend: returned from mem sleep at t=%u\n", monotonic_ms());

    write_sysfs("/sys/class/graphics/fb0/blank", "0"); /* FB_BLANK_UNBLANK */

    if (bt_was_on) {
        bt_control_init_chip();
        bt_control_enable();
    }
    if (wifi_was_on) wifi_control_enable();
}
