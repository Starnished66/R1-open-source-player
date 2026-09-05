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
