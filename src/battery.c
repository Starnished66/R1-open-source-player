#include "battery.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POWER_SUPPLY_DIR "/sys/class/power_supply"

/* Reads a single-line sysfs attribute (e.g. ".../battery/capacity") into
 * `out`, trimming the trailing newline. Returns false if the file doesn't
 * exist or is empty -- normal on host, where none of this exists at all. */
static bool read_sysfs_attr(const char * device_name, const char * attr, char * out, size_t out_size) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/%s", POWER_SUPPLY_DIR, device_name, attr);

    FILE * f = fopen(path, "r");
    if (!f) return false;

    bool ok = fgets(out, (int) out_size, f) != NULL;
    fclose(f);
    if (!ok) return false;

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return len > 0;
}

/* Same dynamic scan the stock hiby_player itself uses (confirmed via
 * strings on the real binary: it enumerates every entry under
 * /sys/class/power_supply, reads each one's "type", and treats whichever
 * one reports "Battery" as
 * the battery -- rather than hardcoding a driver-specific directory name
 * like "battery" or "axp2101-battery", which could change across firmware
 * updates if the fuel-gauge driver is ever swapped).
 *
 * Real-hardware testing found the R1 actually exposes *two* entries typed
 * "Battery": "axp_battery" (a raw/uncalibrated PMIC sub-node) and "battery"
 * (the real fuel gauge, reporting the correct percentage).
 *
 * Real-device bug report (2026-08-08): the charge limiter (configured to
 * stop at 85%) was reported stopping at two DIFFERENT values on two
 * different physical units -- 86% and 91% -- confirmed both on the same,
 * already-fixed charge_limiter.c build. A spread that wide, differing
 * *per unit*, doesn't fit a timing/lag explanation (that would produce a
 * small, fairly consistent overshoot); it fits this scan silently reading
 * from a different physical sensor on each unit. The previous version of
 * this scan preferred "whichever Battery-typed entry reports a nonzero
 * capacity first, in readdir()'s own enumeration order" -- a real fix for
 * the specific case already found (axp_battery reading exactly capacity=0
 * at one observed moment), but readdir() order isn't guaranteed identical
 * across physical units/boots, and axp_battery's raw PMIC voltage-based
 * SOC estimate is notoriously inaccurate near full charge (voltage curves
 * flatten out non-linearly there) -- if it happens to report ANY nonzero
 * value before "battery" is enumerated, this scan would silently lock onto
 * the wrong, less accurate sensor, with no way to tell from the outside.
 * Fixed to deterministically prefer an entry actually named "battery" (the
 * confirmed real fuel-gauge driver name above) over any other
 * Battery-typed entry, rather than trusting enumeration order or a
 * nonzero-value heuristic -- falls back to the old best-effort scan only
 * if no entry is literally named "battery", for robustness on any other
 * device/variant where the real gauge might be named differently. */
int battery_get_percent(void) {
    DIR * dir = opendir(POWER_SUPPLY_DIR);
    if (!dir) return -1;

    int named_battery_result = -1;
    bool found_named_battery = false;
    int fallback_result = -1;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char type[32];
        if (!read_sysfs_attr(entry->d_name, "type", type, sizeof(type))) continue;
        if (strcmp(type, "Battery") != 0) continue;

        char capacity_str[16];
        if (!read_sysfs_attr(entry->d_name, "capacity", capacity_str, sizeof(capacity_str))) continue;

        int capacity = atoi(capacity_str);
        if (strcmp(entry->d_name, "battery") == 0) {
            named_battery_result = capacity;
            found_named_battery = true;
            break; /* the real fuel gauge, unambiguous -- no need to keep scanning */
        }
        if (fallback_result < 0 || capacity > 0) fallback_result = capacity;
    }

    closedir(dir);
    return found_named_battery ? named_battery_result : fallback_result;
}

/* Same "Battery"-typed scan and same axp_battery-vs-battery ambiguity as
 * battery_get_percent() (see its own comment for the real-device bug this
 * fixes) -- shared by battery_is_charging() and battery_is_full() below,
 * since both just want this same scan's "status" string rather than a
 * pre-collapsed bool, and duplicating the whole scan a third time isn't
 * worth avoiding one extra fixed-size string copy. The previous "prefer
 * whichever entry reports the highest capacity" tiebreak had the identical
 * failure mode as battery_get_percent()'s old logic: axp_battery's raw,
 * uncalibrated capacity estimate could easily read *higher* than the real
 * gauge's near full charge, silently winning this comparison and handing
 * back axp_battery's own (less trustworthy) status string. Fixed the same
 * way -- deterministically prefer an entry literally named "battery",
 * falling back to the old best-effort comparison only if no such entry
 * exists. Returns false (leaving *out_status untouched) if no Battery-typed
 * device is found at all.
 *
 * Real-device bug report (2026-08-08): after a while of use, the status
 * bar's battery percentage/icon froze AND the UI went white with menus
 * showing text only, no icons -- at the same time. Root cause: the
 * "battery" fast-path below did `return true` directly, skipping the
 * closedir(dir) that only sat after the loop -- leaking the opendir()
 * file descriptor on every single call. This is the *normal* path on real
 * hardware (this device does have an entry literally named "battery", see
 * this function's own history above), so it leaked one fd on essentially
 * every status-bar refresh tick, not just some rare fallback case. A
 * leaked fd or two a second exhausts a typical ~1024 embedded-Linux
 * process fd limit in well under ten minutes -- once that hits, every
 * later open()/fopen() call in the whole process starts failing, not just
 * this one: sysfs battery reads (the frozen icon/percentage) and PNG
 * asset loading via LVGL's posix fs driver (the white UI/text-only menus,
 * since glyphs are baked into the binary and don't need a file open) fail
 * together for exactly that reason. Unrelated to the charge limiter
 * feature (already inert regardless, see charge_limiter.c) -- this leak
 * exists independent of whether that's enabled. */
static bool read_best_battery_status(char * out_status, size_t out_size) {
    DIR * dir = opendir(POWER_SUPPLY_DIR);
    if (!dir) return false;

    bool found = false;
    int best_capacity = -1;
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char type[32];
        if (!read_sysfs_attr(entry->d_name, "type", type, sizeof(type))) continue;
        if (strcmp(type, "Battery") != 0) continue;

        char capacity_str[16];
        int capacity = read_sysfs_attr(entry->d_name, "capacity", capacity_str, sizeof(capacity_str)) ? atoi(capacity_str) : -1;

        char status[24];
        if (!read_sysfs_attr(entry->d_name, "status", status, sizeof(status))) continue;

        if (strcmp(entry->d_name, "battery") == 0) {
            snprintf(out_status, out_size, "%s", status);
            found = true;
            break; /* the real fuel gauge, unambiguous -- no need to keep scanning */
        }
        if (!found || capacity > best_capacity) {
            found = true;
            best_capacity = capacity;
            snprintf(out_status, out_size, "%s", status);
        }
    }

    closedir(dir);
    return found;
}

bool battery_is_charging(void) {
    char status[24];
    if (!read_best_battery_status(status, sizeof(status))) return false;
    return strcmp(status, "Charging") == 0 || strcmp(status, "Full") == 0;
}

/* Distinct from battery_is_charging() (which also counts "Full" as
 * charging, since external power is still present either way) -- this is
 * specifically for callers that need to tell "actively charging, not
 * topped up yet" apart from "charging complete", like led_control.c's
 * red-while-charging/blue-when-full split. */
bool battery_is_full(void) {
    char status[24];
    if (!read_best_battery_status(status, sizeof(status))) return false;
    return strcmp(status, "Full") == 0;
}
