#ifndef HEADPHONE_STATUS_H
#define HEADPHONE_STATUS_H

#include <stdbool.h>

/* Real headphone/line-out jack detection via the kernel's switch class,
 * confirmed on real R1 Pro hardware by physically plugging/unplugging while
 * watching the sysfs node change. Reads false (no headphone) on host, where
 * the node doesn't exist -- same honest "no data" treatment as battery.c
 * and wifi_status.c. */
bool headphone_is_connected(void);

#endif
