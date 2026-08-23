#include "usb_mode_control.h"
#include "subprocess.h"
#include "usb_dac_bridge.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#define POWER_SUPPLY_DIR "/sys/class/power_supply"

static bool path_exists(const char * path) {
    return access(path, F_OK) == 0;
}

static bool read_trimmed_file(const char * path, char * out, size_t out_size) {
    FILE * f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int) out_size, f) != NULL;
    fclose(f);
    if (!ok) return false;
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' '))
        out[--len] = '\0';
    return true;
}

bool usb_mode_control_cable_connected(void) {
#ifdef HOST_BUILD
    return false;
#else
    DIR * dir = opendir(POWER_SUPPLY_DIR);
    if (!dir) return false;
    bool connected = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[512], type[32], online[8];
        snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/type", entry->d_name);
        if (!read_trimmed_file(path, type, sizeof(type)) || strcasecmp(type, "Battery") == 0) continue;
        snprintf(path, sizeof(path), POWER_SUPPLY_DIR "/%s/online", entry->d_name);
        if (read_trimmed_file(path, online, sizeof(online)) && atoi(online) != 0) {
            connected = true;
            break;
        }
    }
    closedir(dir);
    return connected;
#endif
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
    if (len == 0 || strcmp(buf, "none") == 0) return false;
    return true;
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

/* True if some function link under <gadget_dir>/configs/c.1 starts with
 * `name_prefix` (e.g. "uac_sa." or "mass_storage.") -- checking for a
 * SPECIFIC expected function, not just "is anything bound", is what catches
 * the case a real device hit: android0's UDC bound but to a mass_storage.N
 * function left over from a different mode, while this app believed (and
 * told the user) DAC had succeeded. */
static bool function_linked(const char * gadget_dir, const char * name_prefix) {
    char path[256];
    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
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

/* Generic teardown of a configfs gadget directory -- doesn't
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
static void write_udc(const char * gadget_dir, const char * value) {
    char path[300];
    snprintf(path, sizeof(path), "%s/UDC", gadget_dir);
    FILE * udc_f = fopen(path, "w");
    if (udc_f) {
        fputs(value, udc_f);
        fclose(udc_f);
    }
}

/* Empty lun.0/file is the gadget-side eject. A host that still has MUSIC
 * mounted can make the first write fail; unbind UDC (below) then retry.
 * forced_eject is optional on this kernel -- ignore if the attr is absent. */
static void eject_mass_storage_luns(const char * gadget_dir) {
    char path[300];
    snprintf(path, sizeof(path), "%s/functions", gadget_dir);
    DIR * dir = opendir(path);
    if (!dir) return;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "mass_storage.", 13) != 0) continue;
        char attr[320];
        snprintf(attr, sizeof(attr), "%s/functions/%s/lun.0/forced_eject", gadget_dir, entry->d_name);
        FILE * ej = fopen(attr, "w");
        if (ej) { fputs("1\n", ej); fclose(ej); }
        snprintf(attr, sizeof(attr), "%s/functions/%s/lun.0/file", gadget_dir, entry->d_name);
        FILE * lun_f = fopen(attr, "w");
        if (lun_f) { fputs("\n", lun_f); fclose(lun_f); }
    }
    closedir(dir);
}

static bool rmdir_mass_storage_functions(const char * gadget_dir) {
    char path[300];
    snprintf(path, sizeof(path), "%s/functions", gadget_dir);
    bool leftover = false;
    DIR * dir = opendir(path);
    if (!dir) return true;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char func_path[300];
        snprintf(func_path, sizeof(func_path), "%s/functions/%s", gadget_dir, entry->d_name);
        if (strncmp(entry->d_name, "mass_storage.", 13) == 0) {
            char lun_path[320];
            snprintf(lun_path, sizeof(lun_path), "%s/functions/%s/lun.0/file", gadget_dir, entry->d_name);
            FILE * lun_f = fopen(lun_path, "w");
            if (lun_f) { fputs("\n", lun_f); fclose(lun_f); }
        }
        if (rmdir(func_path) != 0 && path_exists(func_path)) {
            fprintf(stderr, "usb_mode_control: rmdir %s failed (%s)\n", func_path, strerror(errno));
            leftover = true;
        }
    }
    closedir(dir);
    return !leftover;
}

static bool force_clean_gadget_dir(const char * gadget_dir) {
    char path[300];

    /* Eject the LUN while still bound so a PC sees media-removed rather
     * than a surprise disconnect, then unbind. Stock storage_stop() does
     * the opposite and, if rmdir is EBUSY, re-binds UDC -- which is why
     * ADB/DAC only worked after the SD card was physically removed. */
    eject_mass_storage_luns(gadget_dir);
    write_udc(gadget_dir, "\n");
    write_udc(gadget_dir, "none\n");
    usleep(200000);
    eject_mass_storage_luns(gadget_dir);

    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    DIR * cfg = opendir(path);
    if (cfg) {
        struct dirent * entry;
        while ((entry = readdir(cfg)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char link_path[300];
            snprintf(link_path, sizeof(link_path), "%s/configs/c.1/%s", gadget_dir, entry->d_name);
            unlink(link_path);
        }
        closedir(cfg);
    }

    for (int i = 0; i < 10; i++) {
        if (rmdir_mass_storage_functions(gadget_dir)) break;
        usleep(200000);
        eject_mass_storage_luns(gadget_dir);
    }

    snprintf(path, sizeof(path), "%s/configs/c.1/strings/0x409", gadget_dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/configs/c.1", gadget_dir);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/strings/0x409", gadget_dir);
    rmdir(path);
    rmdir(gadget_dir);
    return !path_exists(gadget_dir);
}

static bool configfs_has_gadgets(void) {
    DIR * dir = opendir("/sys/kernel/config/usb_gadget");
    if (!dir) return false;
    bool found = false;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] != '.') { found = true; break; }
    }
    closedir(dir);
    return found;
}

/* The stock ADB and DAC start scripts both deliberately refuse to run if
 * configfs is already mounted. Their stop scripts normally unmount it, but
 * our fallback cleanup previously did not; one imperfect Storage teardown
 * therefore made every later ADB/DAC attempt fail until reboot. */
static bool unmount_empty_configfs(void) {
    if (!path_exists("/sys/kernel/config/usb_gadget")) return true;
    if (configfs_has_gadgets()) return false;
    int exit_code = -1;
    char * argv[] = { (char *) "/bin/umount", (char *) "/sys/kernel/config", NULL };
    subprocess_run_checked(argv, NULL, 0, 3000, &exit_code);
    if (path_exists("/sys/kernel/config/usb_gadget")) {
        char * lazy[] = { (char *) "/bin/umount", (char *) "-l", (char *) "/sys/kernel/config", NULL };
        subprocess_run_checked(lazy, NULL, 0, 3000, &exit_code);
    }
    return !path_exists("/sys/kernel/config/usb_gadget");
}

static bool wait_path(const char * path, int attempts) {
    for (int i = 0; i < attempts; i++) {
        if (path_exists(path)) return true;
        usleep(50000);
    }
    return false;
}

static bool start_adb_gadget(void) {
    int exit_code = -1;
    char * argv[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "start", NULL };
    subprocess_run_checked(argv, NULL, 0, SCRIPT_TIMEOUT_MS, &exit_code);
    if (exit_code != 0) {
        fprintf(stderr, "usb_mode_control: S440adb start exit=%d\n", exit_code);
        return false;
    }
    /* S440adb backgrounds adbserver.sh, which mounts functionfs then starts
     * adbd. The UDC bind is a separate stock script and must wait until
     * that mount exists -- otherwise udc_becomes_bound() times out and the
     * UI reports "Failed to switch to ADB" even though setup is in flight. */
    wait_path("/dev/usb-ffs/adb", SETTLE_RETRY_LIMIT);
    char * bind_argv[] = { (char *) "/sbin/usb_adb_enable.sh", NULL };
    subprocess_run_checked(bind_argv, NULL, 0, 5000, &exit_code);
    bool ok = udc_becomes_bound(ADB_DEMO_UDC_PATH) && function_linked(ADB_DEMO_DIR, "ffs.");
    fprintf(stderr, "usb_mode_control: ADB bind exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
    return ok;
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

    /* Do not call usb_dev_mass_storage.sh stop. If rmdir of mass_storage.0
     * fails because the host still has the volume mounted, that script
     * re-binds UDC and returns 0 -- Storage stays up and every later
     * ADB/DAC start refuses. Eject + force_clean below never re-bind. */
    sync();

    /* Guarantee a clean slate regardless of whether the stock stop scripts
     * actually finished -- see force_clean_gadget_dir()'s own comment. */
    if (path_exists(ANDROID0_DIR)) {
        fprintf(stderr, "usb_mode_control: android0 survived the stop scripts, force-cleaning\n");
        for (int i = 0; i < 10 && path_exists(ANDROID0_DIR); i++) {
            if (force_clean_gadget_dir(ANDROID0_DIR)) break;
            usleep(200000);
        }
    }
    if (path_exists(ADB_DEMO_DIR)) {
        fprintf(stderr, "usb_mode_control: adb_demo survived the stop scripts, force-cleaning\n");
        for (int i = 0; i < 10 && path_exists(ADB_DEMO_DIR); i++) {
            if (force_clean_gadget_dir(ADB_DEMO_DIR)) break;
            usleep(200000);
        }
    }
    /* A leftover unbound gadget dir is not fatal: Storage reuses android0,
     * ADB lives under a different name, and the DAC start script now drops
     * an unbound android0 itself. Only a still-bound UDC blocks the switch. */
    if (udc_is_bound(ANDROID0_UDC_PATH) || udc_is_bound(ADB_DEMO_UDC_PATH)) {
        fprintf(stderr, "usb_mode_control: UDC still bound after teardown; refusing to start mode=%d\n", (int) mode);
        return false;
    }

    /* Prefer an unmounted configfs so stock start scripts are happy.
     * If umount fails on an empty leftover (common after Storage, or a
     * recovery flash with the cable still in), ADB/DAC start scripts now
     * tolerate that and mkdir their gadget on the existing mount. */
    if (mode != USB_MODE_STORAGE && !unmount_empty_configfs())
        fprintf(stderr, "usb_mode_control: configfs still mounted before mode=%d; continuing\n", (int) mode);

    int exit_code = -1;
    switch (mode) {
        case USB_MODE_STORAGE: {
            char * argv[] = { (char *) "/usr/bin/usb_dev_mass_storage.sh", (char *) "start", (char *) "/dev/mmcblk0", NULL };
            subprocess_run_checked(argv, NULL, 0, SCRIPT_TIMEOUT_MS, &exit_code);
            bool ok = exit_code == 0 && udc_becomes_bound(ANDROID0_UDC_PATH) && function_linked(ANDROID0_DIR, "mass_storage.");
            fprintf(stderr, "usb_mode_control: Storage start exit=%d -> %s\n", exit_code, ok ? "ok" : "FAILED");
            return ok;
        }
        case USB_MODE_ADB:
            return start_adb_gadget();
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
