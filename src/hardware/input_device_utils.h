#ifndef INPUT_DEVICE_UTILS_H
#define INPUT_DEVICE_UTILS_H

#include <stdbool.h>
#include <stddef.h>

/* Scans /proc/bus/input/devices for an input device by its exact evdev
 * "Name" (e.g. "hyn_ts", "md-gpio-keys") and writes its /dev/input/eventN
 * node into out_path. Returns false if no device with that name is found. */
bool find_input_device_by_name(const char * name, char * out_path, size_t out_size);

#endif /* INPUT_DEVICE_UTILS_H */
