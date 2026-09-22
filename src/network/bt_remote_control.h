#ifndef BT_REMOTE_CONTROL_H
#define BT_REMOTE_CONTROL_H

/* Classic Bluetooth Remote Control RFCOMM service.
 * UUID: 9d2f8a7e-6b4c-4d51-8e43-38f7a2c0b719
 * SDP service name: "Compas Remote Control".
 * The Android client discovers this UUID over SDP and connects to the
 * advertised RFCOMM channel. Each connection carries one HTTP request. */

/* Registers the discoverable RFCOMM profile while enabled. Safe to call more
 * than once. Registration is retried if D-Bus/BlueZ is not ready yet. */
void bt_remote_control_start(void);

/* Unregisters the profile and stops accepting new connections. */
void bt_remote_control_stop(void);

#endif /* BT_REMOTE_CONTROL_H */
