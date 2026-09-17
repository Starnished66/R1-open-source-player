#include "idle_shutdown.h"

#ifndef HOST_BUILD
#include <unistd.h>
#include <sys/reboot.h>
#endif

void idle_shutdown_now(void) {
#ifndef HOST_BUILD
    extern void gui_player_queue_flush(void);
    gui_player_queue_flush();
    /* Must not go through subprocess_run(): that helper waits 15s then
     * SIGKILLs the child. /sbin/poweroff (busybox, talks to init or waits
     * on other processes) routinely outlives that budget, so the 3-2-1
     * countdown reached zero, the child was killed, and the device stayed
     * on -- only a hardware power-button hold actually cut power. */
    sync();
    execl("/sbin/poweroff", "poweroff", (char *) NULL);
    reboot(RB_POWER_OFF);
    for (;;) pause();
#endif
}

void idle_shutdown_reboot_now(void) {
#ifndef HOST_BUILD
    extern void gui_player_queue_flush(void);
    gui_player_queue_flush();
    /* Unlike idle_shutdown_now() above, this must NOT execl() an external
     * /sbin/reboot: open_hiby_bootloader's run_player_supervised()
     * (src/bootloader/main.c) treats ANY clean (status 0) exit of this
     * exact supervised PID as "player exited cleanly -- power off",
     * regardless of which command replaced this process's image.
     * /sbin/reboot typically exits 0 once it hands off to init, well
     * before the actual kernel restart completes, so the bootloader's
     * poweroff-on-clean-exit races ahead of and wins over the real reboot
     * -- confirmed as the cause of a real "reboot just shuts down instead"
     * report (also fixed in settings.c's factory reset, which used the
     * same pattern). Calling reboot(2) directly, same as main.c's own
     * reboot_device() already does for every other abnormal-exit reboot,
     * restarts the kernel immediately from within this process instead of
     * handing off through an external command whose exit status the
     * supervisor can misread. */
    sync();
    reboot(RB_AUTOBOOT);
    for (;;) pause();
#endif
}
