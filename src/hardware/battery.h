#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>

/* Battery percentage (0-100), or -1 if no battery power_supply device could
 * be found (e.g. running on the host simulator, or the class just doesn't
 * exist). Never faked -- callers should treat -1 as "no data", not 0%. */
int battery_get_percent(void);

/* UI-only stabilized percentage. Keeps physically impossible direction
 * changes and fuel-gauge recalibration jumps out of the top bar while the
 * raw getter above remains available to safety/charge-limit logic. */
int battery_get_display_percent(void);

/* Physical external-power state, independent of whether the PMIC charger
 * itself is enabled.  This distinction matters while the 85% charge limiter
 * is holding: it disables charging, so the battery status can legitimately
 * say "Discharging" even though USB/car power is still connected.
 *
 * UNKNOWN is deliberately distinct from DISCONNECTED.  Callers that can
 * trigger destructive actions (Car Mode powers the device off) must ignore a
 * transient sysfs read failure instead of treating it as a cable removal. */
typedef enum {
    BATTERY_EXTERNAL_POWER_UNKNOWN = -1,
    BATTERY_EXTERNAL_POWER_DISCONNECTED = 0,
    BATTERY_EXTERNAL_POWER_CONNECTED = 1,
} battery_external_power_state_t;

battery_external_power_state_t battery_get_external_power_state(void);

/* True if the selected battery status says "Charging" or "Full".  This is
 * charger activity/status, not a reliable physical-cable detector while the
 * 85% limiter is holding; use battery_get_external_power_state() for that. */
bool battery_is_charging(void);

/* True only for "Full" specifically (plugged in, topped up) -- distinct
 * from battery_is_charging(), which also counts "Full" as charging. False
 * on the host simulator or if no battery device is found. */
bool battery_is_full(void);

#endif /* BATTERY_H */
