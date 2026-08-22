#include "hostname_apply.h"
#include "debug_log.h"

#include <stdio.h>
#include <string.h>

#ifndef HOST_BUILD
#include "subprocess.h"
#include <unistd.h>

#define HOSTNAME_OVERRIDE_FILE "/usr/data/hostname_override.txt"
#define STOCK_HOSTNAME_PATH "/usr/resource/hostname"
#define STOCK_BT_NAME_PATH "/usr/resource/bt_name"

static bool bind_mount_over(const char * source, const char * target) {
    char * argv[] = { "/bin/mount", "-o", "bind", (char *) source, (char *) target, NULL };
    return subprocess_run(argv, NULL, 0);
}
#endif

bool hostname_apply(const char * hostname) {
    if (!hostname || hostname[0] == '\0') return true;

#ifdef HOST_BUILD
    return true; /* no real /usr/resource or /usr/data on a dev machine -- see this file's own header comment */
#else
    FILE * f = fopen(HOSTNAME_OVERRIDE_FILE, "w");
    if (!f) {
        DBG_LOG("hostname_apply: can't open %s for writing\n", HOSTNAME_OVERRIDE_FILE);
        return false;
    }
    fputs(hostname, f);
    fclose(f);

    bool ok_hostname = bind_mount_over(HOSTNAME_OVERRIDE_FILE, STOCK_HOSTNAME_PATH);
    bool ok_bt_name = bind_mount_over(HOSTNAME_OVERRIDE_FILE, STOCK_BT_NAME_PATH);
    /* Best-effort, non-fatal to the app either way -- worst case the stock
     * name stays in effect for whichever of the two this couldn't override,
     * same tolerant style as timezone_apply(). */
    sethostname(hostname, strlen(hostname));

    DBG_LOG("hostname_apply: '%s' -> %s (%s), %s (%s)\n", hostname, STOCK_HOSTNAME_PATH,
            ok_hostname ? "ok" : "failed", STOCK_BT_NAME_PATH, ok_bt_name ? "ok" : "failed");
    return ok_hostname && ok_bt_name;
#endif
}
