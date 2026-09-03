#ifndef BALANCED_OUTPUT_STATUS_H
#define BALANCED_OUTPUT_STATUS_H

#include <stdbool.h>

/* Balanced-output jack detection via the kernel's switch class, mirroring
 * headphone_status.c's own headphone_is_connected() exactly. Unlike that
 * one, this has NOT been confirmed against real hardware yet -- see
 * balanced_output_status.c's own comment on where the sysfs path comes
 * from and what's actually verified versus pattern-inferred. Reads false
 * (no balanced connection) on host and on any board/device where the node
 * doesn't exist, same honest "no data" treatment as headphone_is_connected(). */
bool balanced_headphone_is_connected(void);

#endif
