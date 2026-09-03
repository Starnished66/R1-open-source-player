#include "balanced_output_status.h"

#include <stdio.h>

/* Standard Android-style "switch class" jack-detect node, same shape as
 * headphone_status.c's own HEADSET_SWITCH_STATE_PATH -- but that path was
 * confirmed by physically plugging/unplugging on a live device, and this
 * one is not yet: it's pattern-inferred from the R3 Pro II's own stock
 * firmware, where sa_sound_switch.ko is loaded with a real, configured
 * `sass_balance_det_gpio` (unlike the R1's build of the same module, which
 * leaves that parameter unset since the R1 has no balanced port), and that
 * parameter's naming mirrors the already-confirmed headset one exactly.
 * Verify this exact path against a live device before trusting it beyond
 * a first attempt. */
#define BALANCED_SWITCH_STATE_PATH "/sys/devices/virtual/switch/balance/state"

bool balanced_headphone_is_connected(void) {
    FILE * f = fopen(BALANCED_SWITCH_STATE_PATH, "r");
    if (!f) return false;

    char buf[8] = {0};
    bool ok = fgets(buf, (int) sizeof(buf), f) != NULL;
    fclose(f);

    return ok && buf[0] == '1';
}
