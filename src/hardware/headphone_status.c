#include "headphone_status.h"

#include <stdio.h>

/* Standard Android-style "switch class" jack-detect node -- verified on
 * real hardware (not guessed): reads "0" with nothing plugged in and "1"
 * once a headphone/dongle is inserted, confirmed by physically
 * plugging/unplugging on the device while cat-ing this file. */
#define HEADSET_SWITCH_STATE_PATH "/sys/devices/virtual/switch/headset/state"
#define BALANCED_SWITCH_STATE_PATH "/sys/devices/virtual/switch/balance/state"

// must be written "on" before headphone buttons can trigger input events
#define EARPODS_ADC_SW_PATH "/sys/devices/platform/earpods_adc/earpods_adc/earpods_adc_sw"

// returns true if headset 3.5mm jack is plugged in
bool headset_is_connected(void) {
    FILE * f = fopen(HEADSET_SWITCH_STATE_PATH, "r");
    if (!f) return false;

    char buf[8] = {0};
    bool ok = fgets(buf, (int) sizeof(buf), f) != NULL;
    fclose(f);

    return ok && buf[0] == '1';
}

// returns true if balanced 4.4mm jack is plugged in
bool balanced_is_connected(void) {
    FILE * f = fopen(BALANCED_SWITCH_STATE_PATH, "r");
    if (!f) return false;

    char buf[8] = {0};
    bool ok = fgets(buf, (int) sizeof(buf), f) != NULL;
    fclose(f);

    return ok && buf[0] == '1';
}

// enable button inputs for headsets with inline remote
static void set_earpods_adc_enabled(bool enabled) {
    FILE * f = fopen(EARPODS_ADC_SW_PATH, "w");
    if (!f) return;
    fputs(enabled ? "on" : "off", f);
    fclose(f);
}

// returns which headphone output is plugged in
// if 3.5mm and 4.4mm are both plugged in, 4.4mm is prioritized
// syncs earpods_adc_sw with headphone state
enum HEADPHONE_STATE get_headphone_state(void) {
    static enum HEADPHONE_STATE last_state = HEADPHONE_STATE_NONE;
    enum HEADPHONE_STATE state;

    if (balanced_is_connected()) {
        state = HEADPHONE_STATE_BALANCED;
    } else if (headset_is_connected()) {
        state = HEADPHONE_STATE_HEADSET;
    } else {
        state = HEADPHONE_STATE_NONE;
    }

    if (state != last_state) {
        set_earpods_adc_enabled(state == HEADPHONE_STATE_HEADSET);
        last_state = state;
    }

    return state;
}
