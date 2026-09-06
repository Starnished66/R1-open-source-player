#ifndef HOSTNAME_APPLY_H
#define HOSTNAME_APPLY_H

#include <stdbool.h>

/* Applies the configured hostname by writing it to writable storage (/usr/data)
 * and bind-mounting it over the read-only /usr/resource/hostname and
 * /usr/resource/bt_name files, as well as calling sethostname() for the kernel.
 *
 * Invoked once during boot from gui_init() before Wi-Fi or Bluetooth initialization
 * scripts read their hostname files.
 *
 * Returns true immediately if hostname is NULL, empty, or under HOST_BUILD. */
bool hostname_apply(const char * hostname);

#endif /* HOSTNAME_APPLY_H */
