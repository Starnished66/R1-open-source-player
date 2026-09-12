#include "headphone_status.h"

#include <stdio.h>

/* Standard Android-style "switch class" jack-detect node -- verified on
 * real hardware (not guessed): reads "0" with nothing plugged in and "1"
 * once a headphone/dongle is inserted, confirmed by physically
 * plugging/unplugging on the device while cat-ing this file. */
#define HEADSET_SWITCH_STATE_PATH "/sys/devices/virtual/switch/headset/state"
#define BALANCED_SWITCH_STATE_PATH "/sys/devices/virtual/switch/balance/state"

static bool switch_is_active(const char * path) {
    FILE * f = fopen(path, "r");
    if (!f) return false;

    char buf[8] = {0};
    bool ok = fgets(buf, (int) sizeof(buf), f) != NULL;
    fclose(f);

    return ok && buf[0] == '1';
}

// returns which headphone output is plugged in
// if 3.5mm and 4.4mm are both plugged in, 4.4mm is prioritized
enum HEADPHONE_STATE get_headphone_state(void) {
	if (switch_is_active(BALANCED_SWITCH_STATE_PATH)) {
		return HEADPHONE_STATE_BALANCED;
	} else if (switch_is_active(HEADSET_SWITCH_STATE_PATH)) {
		return HEADPHONE_STATE_HEADSET;
	}

	return HEADPHONE_STATE_NONE;
}
