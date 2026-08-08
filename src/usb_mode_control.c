#include "usb_mode_control.h"
#include "subprocess.h"
#include "usb_dac_bridge.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SETTLE_RETRY_LIMIT 20 /* ~1s at 50ms between checks */

/* Storage and DAC both live under this same configfs gadget name (confirmed
 * by reading usb_dev_mass_storage.sh and uac_device_config.sh directly off
 * the device) -- ADB uses a separate "adb_demo" directory. */
#define ANDROID0_DIR "/sys/kernel/config/usb_gadget/android0"
#define ANDROID0_UDC_PATH ANDROID0_DIR "/UDC"
#define ADB_DEMO_DIR "/sys/kernel/config/usb_gadget/adb_demo"
#define ADB_DEMO_UDC_PATH ADB_DEMO_DIR "/UDC"

/* Matches subprocess_run()'s own default -- not reused directly since
 * SUBPROCESS_TIMEOUT_MS is private to subprocess.c and there's no public
 * "run with the default timeout, but checked" entry point. */
#define SCRIPT_TIMEOUT_MS 15000

static bool path_exists(const char * path) {
    return access(path, F_OK) == 0;
}

/* A gadget's UDC sysfs file holds the bound UDC's name (e.g.
 * "13500000.otg_new") once `echo $UDC > UDC` has actually taken, empty
 * otherwise -- the one place in configfs that says "the kernel accepted
 * this gadget's descriptors and it's live", as opposed to "the script that
 * was supposed to set it up merely ran to completion". */
static bool udc_is_bound(const char * udc_path) {
    FILE * f = fopen(udc_path, "r");
    if (!f) return false;
    char buf[64] = { 0 };
    bool read_ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!read_ok) return false;

    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' ')) buf[--len] = '\0';
    return len > 0;
}

/* Polls udc_is_bound() for up to ~1s instead of checking exactly once --
 * `echo $UDC > UDC` returning to the shell script doesn't necessarily mean
 * the kernel has fully finished processing the bind by the time this
 * process's subprocess_run_checked() call returns and this gets checked;
 * a real device switch showed a start script exit 0 with a function
 * correctly linked but UDC reading back empty on the first immediate
 * check, reported as a spurious "can't switch" failure. */
static bool udc_becomes_bound(const char * udc_path) {
    for (int i = 0; i < SETTLE_RETRY_LIMIT; i++) {
        if (udc_is_bound(udc_path)) return true;
        usleep(50000);
    }
    return false;
}

/* True if some function directory under <gadget_dir>/functions starts with
 * `name_prefix` (e.g. "uac_sa." or "mass_storage.") -- checking for a
 * SPECIFIC expected function, not just "is anything bound", is what catches
 * the case a real device hit: android0's UDC bound but to a mass_storage.N
 * function left over from a different mode, while this app believed (and
 * told the user) DAC had succeeded. */
static bool function_linked(const char * gadget_dir, const char * name_prefix) {
    char path[256];
    snprintf(path, sizeof(path), "%s/functions", gadget_dir);
    DIR * dir = opendir(path);
    if (!dir) return false;

    bool found = false;
    size_t prefix_len = strlen(name_prefix);
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, name_prefix, prefix_len) == 0) { found = true; break; }
    }
    closedir(dir);
    return found;
}

/* Best-effort, generic teardown of a configfs gadget directory -- doesn't
 * assume any particular function name is inside, unlike the stock stop
 * scripts (uac_device_config.sh's uac_stop(), usb_dev_mass_storage.sh's
 * storage_stop()), which each hardcode exactly one function name and
 * silently fail to finish (their targeted rm/rmdir just no-op on a path
 * that doesn't exist, and the final rmdir on the gadget dir itself then
 * fails since it's non-empty) if a MISMATCHED function from a different
 * mode's incomplete previous teardown is what's actually there. Confirmed
 * on a real device: this is exactly how android0 ended up permanently
 * stuck with a stale mass_storage.0 that DAC's own stop script couldn't
 * remove, silently corrupting every subsequent switch attempt. Called
 * after the stock stop scripts as a guarantee, not a replacement for
 * them -- they still do the real process-killing side effects (killall
 * adbd/adbserver.sh) this doesn't attempt to duplicate. */
static void force_clean_gadget_dir(const char * gadget_dir) {
    char path[300];

    snprintf(path, sizeof(path), "%s/UDC", gadget_dir);
    FILE * udc_f = fopen(path, "w");
    if (udc_f) {
        fputs("\n", udc_f);
        fclose(udc_f);
    }

    snprintf(path, sizeof(path), "%s/functions", gadget_dir);
    DIR * dir = opendir(path);
    if (dir) {
        struct dirent * entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char link_path[300];
            snprintf(link_path, sizeof(link_path), "%s/configs/c.1/%s", gadget_dir, entry->d_name);
            unlink(link_path);
            char func_path[300];
            snprintf(func_path, sizeof(func_path), "%s/functions/%s", gadget_dir, entry->d_name);
            rmdir(func_path);
        }
        closedir(dir);
    }

    snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409", gadget_dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/strings/0x409", gadget_dir);
    rmdir(path);
    rmdir(gadget_dir);
}

bool usb_mode_control_apply(usb_mode_t mode) {
    fprintf(stderr, "usb_mode_control: switching to mode=%d\n", (int) mode);
    usb_dac_bridge_stop(); /* no-op unless we're actually leaving DAC mode */

    /* Deliberately does NOT call uac_device_config.sh's own "stop" --
     * confirmed on a real device that uac_stop()'s very first line (`echo 0
     * > /sys/class/usb_gadget/android0/soft_disconnect`, a stale reference
     * to the legacy sysfs class path rather than the configfs one this
     * script otherwise uses throughout) blocks indefinitely on this
     * device/kernel. Every call ate the full subprocess timeout and never
     * reached its actual cleanup code, which is how android0 ended up
     * permanently stuck with stale leftover functions poisoning every
     * later switch. force_clean_gadget_dir() below (which only touches the
     * safe configfs paths, confirmed working directly against this exact
     * leftover state) handles android0 teardown unconditionally instead,
     * for both DAC and Storage. */

    char * adb_stop_argv[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "stop", NULL };
    subprocess_run(adb_stop_argv, NULL, 0);

    /* Same /dev/mmcblk0 argument adbon/adboff themselves pass -- storage's
     * own stop script matches against this exact path to find which
     * mass_storage.N function to tear down. */
    char * storage_stop_argv[] = { (char *) "/usr/bin/usb_dev_mass_storage.sh", (char *) "stop", (char *) "/dev/mmcblk0", NULL };
    subprocess_run(storage_stop_argv, NULL, 0);

    /* Guarantee a clean slate regardless of whether the stock stop scripts
     * actually finished -- see force_clean_gadget_dir()'s own comment. */
    if (path_exists(ANDROID0_DIR)) {
        fprintf(stderr, "usb_mode_control: android0 survived the stop scripts, force-cleaning\n");
        force_clean_gadget_dir(ANDROID0_DIR);
    }
    if (path_exists(ADB_DEMO_DIR)) {
        fprintf(stderr, "usb_mode_control: adb_demo survived the stop scripts, force-cleaning\n");
        force_clean_gadget_dir(ADB_DEMO_DIR);
    }

    int exit_code = -1;
    switch (mode) {
        case USB_MODE_STORAGE: {
            char * argv[] = { (char *) "/usr/bin/usb_dev_mass_storage.sh", (char *) "start", (char *) "/dev/mmcblk0", NULL };
            subprocess_run_checked(argv, NULL, 0, SCRIPT_TIMEOUT_MS, &exit_code);
            bool ok = exit_code == 0 && udc_becomes_bound(ANDROID0_UDC_PATH) && function_linked(ANDROID0_DIR, "mass_storage.");
            fprintf(stderr, "usb_mode_control: Storage start exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
            return ok;
        }
        case USB_MODE_ADB: {
            char * argv[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "start", NULL };
            subprocess_run_checked(argv, NULL, 0, SCRIPT_TIMEOUT_MS, &exit_code);
            bool ok = exit_code == 0 && udc_becomes_bound(ADB_DEMO_UDC_PATH) && function_linked(ADB_DEMO_DIR, "ffs.");
            fprintf(stderr, "usb_mode_control: ADB start exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
            return ok;
        }
        case USB_MODE_DAC: {
            /* No -v/-p/-m/-n overrides -- the script's own defaults
             * (VID 0x32BB/PID 0x0004, "Hiby"/"R1") already match what the
             * stock binary passes explicitly, confirmed by reading the
             * script directly off the device. */
            char * argv[] = { (char *) "/usr/bin/uac_device_config.sh", (char *) "start", NULL };
            subprocess_run_checked(argv, NULL, 0, SCRIPT_TIMEOUT_MS, &exit_code);
            bool ok = exit_code == 0 && udc_becomes_bound(ANDROID0_UDC_PATH) && function_linked(ANDROID0_DIR, "uac_sa.");
            fprintf(stderr, "usb_mode_control: DAC start exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
            if (!ok) return false;
            usb_dac_bridge_start();
            return true;
        }
    }
    return false;
}

bool usb_mode_control_detect_current(usb_mode_t * out_mode) {
    if (udc_is_bound(ADB_DEMO_UDC_PATH) && function_linked(ADB_DEMO_DIR, "ffs.")) {
        *out_mode = USB_MODE_ADB;
        return true;
    }
    if (udc_is_bound(ANDROID0_UDC_PATH)) {
        if (function_linked(ANDROID0_DIR, "uac_sa.")) {
            *out_mode = USB_MODE_DAC;
            return true;
        }
        if (function_linked(ANDROID0_DIR, "mass_storage.")) {
            *out_mode = USB_MODE_STORAGE;
            return true;
        }
    }
    return false;
}
