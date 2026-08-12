#include "import_web.h"
#include "subprocess.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

#define SRC_WEB "/usr/share/web"
#define DATA_PATH "/usr/data/share"
#define DST_WEB DATA_PATH "/web"

static bool dir_exists(const char * path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void import_web_start(void) {
    char * mkdir_argv[] = { (char *) "mkdir", (char *) "-p", (char *) DATA_PATH, NULL };
    subprocess_run(mkdir_argv, NULL, 0);

    if (!dir_exists(DST_WEB)) {
        char * cp_argv[] = { (char *) "cp", (char *) "-r", (char *) "-p", (char *) SRC_WEB, (char *) DATA_PATH,
                             NULL };
        subprocess_run(cp_argv, NULL, 0);
    }

    char * cp_html_argv[] = { (char *) "cp", (char *) "-p", (char *) SRC_WEB "/pages/index-EN.html",
                              (char *) DST_WEB "/index.html", NULL };
    subprocess_run(cp_html_argv, NULL, 0);
    char * cp_js_argv[] = { (char *) "cp", (char *) "-p", (char *) SRC_WEB "/pages/index-EN.js",
                            (char *) DST_WEB "/js/index.js", NULL };
    subprocess_run(cp_js_argv, NULL, 0);

    char * udp_argv[] = { (char *) "udp_server", (char *) "-p", (char *) "4399", NULL };
    subprocess_spawn_daemon(udp_argv);

    char * thttpd_argv[] = { (char *) "thttpd", (char *) "-C", (char *) "/etc/thttpd.conf", NULL };
    subprocess_spawn_daemon(thttpd_argv);
}

static const char * const IMPORT_WEB_PROCESS_NAMES[] = {
    "udp_server", "thttpd", "create.cgi", "delete.cgi", "download.cgi", "list.cgi", "move.cgi", "upload.cgi",
};
#define IMPORT_WEB_PROCESS_COUNT (sizeof(IMPORT_WEB_PROCESS_NAMES) / sizeof(IMPORT_WEB_PROCESS_NAMES[0]))

static bool process_running(const char * name) {
    int exit_code = -1;
    char * argv[] = { (char *) "pgrep", (char *) "-x", (char *) name, NULL };
    subprocess_run_checked(argv, NULL, 0, 1000, &exit_code);
    return exit_code == 0; /* pgrep: 0 = at least one match, 1 = none */
}

/* Real-device crash: entering and leaving this screen a few times in a row
 * eventually crashed the whole player. Root cause: busybox's killall here
 * only sends SIGTERM and returns as soon as the signal is delivered -- it
 * has no -w/wait flag (confirmed via `strings` on the on-device busybox;
 * GNU killall's -w doesn't exist in this busybox build) -- so this used to
 * return with no guarantee any target had actually exited yet. thttpd's own
 * CGI children (create/delete/download/list/move/upload.cgi) are ordinary
 * forked processes under no obligation to honor SIGTERM promptly, or at
 * all if caught mid-syscall. import_web_start() then unconditionally
 * respawns udp_server/thttpd on the very next screen entry with no check
 * that the prior instances are gone -- on this device's 64MB of total RAM,
 * a few cycles' worth of duplicate/leaked daemon or CGI processes stacking
 * up is enough to trigger an OOM-kill of something, possibly this app
 * itself. Fixed two ways: SIGKILL instead of the default SIGTERM (can't be
 * caught, ignored, or delayed by whatever the target is doing), and an
 * actual bounded wait verifying every target is gone via pgrep before
 * returning, so the next import_web_start() never races a fresh spawn
 * against a not-yet-dead prior one. */
void import_web_stop(void) {
    for (size_t i = 0; i < IMPORT_WEB_PROCESS_COUNT; i++) {
        char * argv[] = { (char *) "killall", (char *) "-9", (char *) IMPORT_WEB_PROCESS_NAMES[i], NULL };
        subprocess_run(argv, NULL, 0);
    }

    /* SIGKILL can't be blocked, but a just-killed process still takes the
     * kernel a moment to actually reap off the runqueue -- polled rather
     * than assumed instant. Bounded so a genuinely wedged process (D-state
     * on a stuck filesystem, say) can't hang screen navigation forever. */
    for (int waited_ms = 0; waited_ms < 500; waited_ms += 50) {
        bool any_alive = false;
        for (size_t i = 0; i < IMPORT_WEB_PROCESS_COUNT; i++) {
            if (process_running(IMPORT_WEB_PROCESS_NAMES[i])) {
                any_alive = true;
                break;
            }
        }
        if (!any_alive) break;
        usleep(50000);
    }
}
