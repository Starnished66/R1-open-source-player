#ifndef WIFI_STATUS_H
#define WIFI_STATUS_H

#include <stdbool.h>

/* Returns true if connected to an access point (mirroring the stock
 * hiby_player's own check: `wpa_cli -i wlan0 status`, looking for
 * "wpa_state=COMPLETED" -- confirmed via `strings` on the real binary,
 * which shells out to wpa_cli rather than reading /proc/net/wireless or
 * any wireless ioctl/nl80211 directly) and, in that case, populates
 * *out_signal_level with a 0 (weakest) - 3 (strongest) bucket derived from
 * `wpa_cli -i wlan0 signal_poll`'s RSSI reading -- the real thresholds
 * used to pick between the stock theme's wifi_connect_0..3 icons aren't
 * recoverable from the binary's strings, so these are conventional RSSI
 * buckets, not extracted values. Returns false (leaving *out_signal_level
 * untouched) if not connected, or if wpa_cli/wpa_supplicant aren't
 * available at all (wifi off, or no wlan0 supplicant instance -- always
 * the case on the host simulator) -- callers should show the disconnected
 * icon in every such case, never faked as connected. */
bool wifi_get_status(int * out_signal_level);

#endif /* WIFI_STATUS_H */
