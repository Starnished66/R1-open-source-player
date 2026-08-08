#include "airplay_control.h"
#include "subprocess.h"

void airplay_control_start(const char * device_name) {
    char * argv[] = { (char *) "shairport", (char *) "-a", (char *) device_name, (char *) "-o", (char *) "ot",
                      (char *) "-M", (char *) "/tmp", (char *) "-b", (char *) "160", NULL };
    subprocess_spawn_daemon(argv);
}

void airplay_control_stop(void) {
    char * argv[] = { (char *) "killall", (char *) "shairport", NULL };
    subprocess_run(argv, NULL, 0);
}
