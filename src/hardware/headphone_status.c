#include "headphone_status.h"
#include "settings.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Standard Android-style "switch class" jack-detect node -- verified on
 * real hardware (not guessed): reads "0" with nothing plugged in and "1"
 * once a headphone/dongle is inserted, confirmed by physically
 * plugging/unplugging on the device while cat-ing this file. */
#define HEADSET_SWITCH_STATE_PATH "/sys/devices/virtual/switch/headset/state"
#define BALANCED_SWITCH_STATE_PATH "/sys/devices/virtual/switch/balance/state"

/* Must be written "on" before headphone buttons can trigger input events. */
#define EARPODS_ADC_SW_PATH "/sys/devices/platform/earpods_adc/earpods_adc/earpods_adc_sw"

static bool switch_is_active(const char * path) {
    FILE * f = fopen(path, "r");
    if (!f) return false;

    char buf[8] = {0};
    bool ok = fgets(buf, (int) sizeof(buf), f) != NULL;
    fclose(f);

    return ok && buf[0] == '1';
}

/* Keep headphone detection pure; ADC configuration is synchronized separately. */
enum HEADPHONE_STATE get_headphone_state(void) {
    if (switch_is_active(BALANCED_SWITCH_STATE_PATH)) return HEADPHONE_STATE_BALANCED;
    if (switch_is_active(HEADSET_SWITCH_STATE_PATH)) return HEADPHONE_STATE_HEADSET;
    return HEADPHONE_STATE_NONE;
}

#ifndef HOST_BUILD
static pthread_mutex_t earpods_adc_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool earpods_adc_state_applied = false;
static bool earpods_adc_applied_enabled = false;
static bool earpods_adc_error_reported = false;

static bool set_earpods_adc_enabled(bool enabled) {
    FILE * f = fopen(EARPODS_ADC_SW_PATH, "w");
    if (!f) {
        if (!earpods_adc_error_reported) {
            fprintf(stderr, "headphone_status: inline remote unavailable: cannot open %s (%s)\n",
                    EARPODS_ADC_SW_PATH, strerror(errno));
            earpods_adc_error_reported = true;
        }
        return false;
    }
    bool ok = fputs(enabled ? "on" : "off", f) != EOF;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        if (!earpods_adc_error_reported) {
            fprintf(stderr, "headphone_status: failed to set inline remote %s via %s (%s)\n",
                    enabled ? "on" : "off", EARPODS_ADC_SW_PATH, strerror(errno));
            earpods_adc_error_reported = true;
        }
        return false;
    }
    if (earpods_adc_error_reported) {
        fprintf(stderr, "headphone_status: inline remote sysfs control is available again\n");
        earpods_adc_error_reported = false;
    }
    return true;
}
#endif

void headphone_status_refresh_earpods_adc(void) {
#ifndef HOST_BUILD
    bool enabled = get_headphone_state() == HEADPHONE_STATE_HEADSET &&
                   current_settings.inline_remote_enabled;
    pthread_mutex_lock(&earpods_adc_mutex);
    if (!earpods_adc_state_applied || enabled != earpods_adc_applied_enabled) {
        if (set_earpods_adc_enabled(enabled)) {
            earpods_adc_applied_enabled = enabled;
            earpods_adc_state_applied = true;
        }
    }
    pthread_mutex_unlock(&earpods_adc_mutex);
#endif
}
