#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>

/* Battery percentage (0-100), or -1 if no battery power_supply device could
 * be found (e.g. running on the host simulator, or the class just doesn't
 * exist). Never faked -- callers should treat -1 as "no data", not 0%. */
int battery_get_percent(void);

/* True if external power is present -- power_supply "status" reads
 * "Charging" or "Full" (the latter still means plugged in, just topped up;
 * unplugging always reverts it to "Discharging"). False (not true) on the
 * host simulator or if no battery device is found, same as
 * battery_get_percent()'s -1 case. */
bool battery_is_charging(void);

/* True only for "Full" specifically (plugged in, topped up) -- distinct
 * from battery_is_charging(), which also counts "Full" as charging. False
 * on the host simulator or if no battery device is found. */
bool battery_is_full(void);

#endif /* BATTERY_H */
