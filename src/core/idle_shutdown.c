#include "idle_shutdown.h"
#include "subprocess.h"

void idle_shutdown_now(void) {
    char * argv[] = { "/sbin/poweroff", NULL };
    subprocess_run(argv, NULL, 0);
}
