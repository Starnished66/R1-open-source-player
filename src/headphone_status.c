#include "headphone_status.h"

#include <stdio.h>

/* Standard Android-style "switch class" jack-detect node -- verified on
 * real hardware (not guessed): reads "0" with nothing plugged in and "1"
 * once a headphone/dongle is inserted, confirmed by physically
 * plugging/unplugging on the device while cat-ing this file. */
#define HEADSET_SWITCH_STATE_PATH "/sys/devices/virtual/switch/headset/state"

bool headphone_is_connected(void) {
    FILE * f = fopen(HEADSET_SWITCH_STATE_PATH, "r");
    if (!f) return false;

    char buf[8] = {0};
    bool ok = fgets(buf, (int) sizeof(buf), f) != NULL;
    fclose(f);

    return ok && buf[0] == '1';
}
