#ifndef INPUT_DEVICE_UTILS_H
#define INPUT_DEVICE_UTILS_H

#include <stdbool.h>
#include <stddef.h>

/* Scans /proc/bus/input/devices for an input device by its exact evdev
 * "Name" (e.g. "hyn_ts", "md-gpio-keys") and writes its /dev/input/eventN
 * node into out_path. Returns false if no device with that name is found. */
bool find_input_device_by_name(const char * name, char * out_path, size_t out_size);

/* Same as find_input_device_by_name(), but matches any device whose Name
 * contains `substring` rather than requiring an exact match -- for devices
 * whose name isn't fixed at compile time, e.g. BlueZ's own "input" plugin
 * names its virtual AVRCP keyboard "<connected accessory's own BT name>
 * (AVRCP)", which obviously can't be matched exactly. */
bool find_input_device_containing(const char * substring, char * out_path, size_t out_size);

#endif /* INPUT_DEVICE_UTILS_H */
