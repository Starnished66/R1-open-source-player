#ifndef BLUETOOTH_RECONNECT_H
#define BLUETOOTH_RECONNECT_H

#include <stdbool.h>

/* Nonblocking Bluetooth auto-reconnect lifecycle. All subprocess work runs
 * on the private worker; poll/join are the only calls that may join it. */
bool bt_reconnect_start(const char * preferred_mac);
void bt_reconnect_cancel(void);
void bt_reconnect_poll(void);
bool bt_reconnect_busy(void);
void bt_reconnect_shutdown(void);

#endif /* BLUETOOTH_RECONNECT_H */
