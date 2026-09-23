#ifndef BT_REMOTE_CONTROL_H
#define BT_REMOTE_CONTROL_H

#include <stdbool.h>

typedef enum {
    BT_REMOTE_CONTROL_DISABLED = 0,
    BT_REMOTE_CONTROL_WAITING,
    BT_REMOTE_CONTROL_READY,
    BT_REMOTE_CONTROL_FAILED
} bt_remote_control_status_t;

/* Classic Bluetooth Remote Control RFCOMM service.
 * UUID: 9d2f8a7e-6b4c-4d51-8e43-38f7a2c0b719
 * SDP service name: "Compas Remote Control".
 * BlueZ advertises RFCOMM channel 21 in SDP. The Android client discovers this
 * UUID and channel over SDP; clients should not hardcode the channel. Each
 * connection carries one HTTP request. */

/* Registers the discoverable RFCOMM profile while enabled. Safe to call more
 * than once. Registration is retried if D-Bus/BlueZ is not ready yet. */
void bt_remote_control_start(void);

/* Unregisters the profile and stops accepting new connections. */
void bt_remote_control_stop(void);

/* Lock-free snapshot of Profile1 registration. It never queries D-Bus and is
 * safe to read from the UI thread. FAILED means a registration attempt failed;
 * the service keeps retrying in the background. */
bt_remote_control_status_t bt_remote_control_status(void);
bool bt_remote_control_is_advertising(void);

#endif /* BT_REMOTE_CONTROL_H */
