#ifndef WIFI_CONTROL_H
#define WIFI_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_MAX_SSID_LEN 33 /* 32 bytes + NUL, the 802.11 max SSID length */

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    int signal_level; /* 0 (weakest) - 3 (strongest), same bucketing as wifi_status.c */
    bool secured;
    bool is_current;
} wifi_network_t;

/* Whether the wpa_supplicant control socket for wlan0 exists -- i.e.
 * whether Wi-Fi has been turned on at all (independent of whether it's
 * actually associated to an access point yet; see wifi_get_status() in
 * wifi_status.h for that). */
bool wifi_control_is_enabled(void);

/* Enable/disable Wi-Fi via the stock firmware's own scripts
 * (/usr/bin/wifi_on.sh, /usr/bin/wifi_off.sh) rather than reimplementing
 * the ifconfig/wpa_supplicant/udhcpc dance ourselves. Brings wlan0 up,
 * starts wpa_supplicant against /data/wpa_supplicant.conf (auto-reconnecting
 * to any saved network), and gets a DHCP lease. Blocking for a second or
 * so; call off the UI thread. */
void wifi_control_enable(void);
void wifi_control_disable(void);

/* Kicks off an async radio scan (`wpa_cli scan`) -- results aren't ready
 * immediately, poll wifi_control_get_results() a couple of seconds later. */
void wifi_control_scan_start(void);

/* Fills out[] with up to max_count networks from the most recent scan
 * (deduplicated by SSID, strongest BSSID kept; hidden/blank-SSID entries
 * skipped since there's nothing useful to show or tap), returns how many
 * were written. */
int wifi_control_get_results(wifi_network_t * out, int max_count);

/* Joins `ssid`, with `password` (NULL/empty for an open network). The PSK
 * is derived from the passphrase via PBKDF2-HMAC-SHA1 (mbedTLS) per the
 * WPA-PSK spec (salt = SSID, 4096 iterations, 256-bit output), then handed
 * to wpa_cli as a precomputed hex PSK; both it and the hex-encoded SSID go
 * over argv via subprocess_run(), never through a shell, so arbitrary
 * characters the user typed can't be misinterpreted as shell syntax. Adds,
 * enables, and saves the network (persists across reboots). Blocking; takes
 * a few seconds for association, call off the UI thread. Returns true once
 * wpa_cli accepted the configuration -- doesn't itself confirm association
 * succeeded (poll wifi_get_status() afterward for that). */
bool wifi_control_connect(const char * ssid, const char * password);

/* Drops the current association (wpa_cli disconnect) without touching the
 * saved network profile -- wpa_supplicant stays in a disconnected state
 * until a new connect attempt or reconnect/reassociate, it won't silently
 * rejoin on its own. Blocking; call off the UI thread. */
bool wifi_control_disconnect(void);

typedef struct {
    int id; /* wpa_supplicant's own network id -- needed by wifi_control_forget() */
    char ssid[WIFI_MAX_SSID_LEN];
} wifi_saved_network_t;

/* Every network wpa_supplicant has saved (wpa_cli list_networks) -- distinct
 * from wifi_control_get_results()'s scan list: saved networks may not be
 * currently in range, and in-range networks may not be saved. */
int wifi_control_list_saved(wifi_saved_network_t * out, int max_count);

/* Removes a saved network by id (wpa_cli remove_network + save_config) --
 * drops the active association too if it's the one currently connected,
 * same as any other wpa_supplicant config change. Blocking; call off the
 * UI thread. */
bool wifi_control_forget(int id);

/* Reconnects to an already-saved network by id (wpa_cli select_network),
 * which reuses the existing profile's stored PSK instead of creating a new
 * network block. Deliberately not wifi_control_connect(ssid, NULL): that
 * always creates a fresh add_network entry, which called with no password
 * against an SSID already saved as secured would add a broken duplicate
 * open network instead of reconnecting. select_network also handles
 * disconnecting from whatever's currently associated -- no separate
 * disconnect call needed first. Blocking; call off the UI thread. */
bool wifi_control_connect_saved(int id);

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char ip[16];
    char gateway[16];
    char mac[18];
    int signal_level;
} wifi_info_t;

/* Current connection details for the Wi-Fi info screen -- returns false if
 * not currently associated (same wpa_state=COMPLETED check as
 * wifi_get_status()). Blocking (a couple of subprocess calls); call off
 * the UI thread. */
bool wifi_control_get_info(wifi_info_t * out);

/* DNS servers currently active, parsed from /etc/resolv.conf. That file is
 * a symlink to /tmp/resolv.conf and gets overwritten with DHCP-provided
 * servers on every renew/reconnect -- a manual override set via
 * wifi_control_set_dns() persists only until the next one. */
int wifi_control_get_dns(char out[][16], int max_count);
bool wifi_control_set_dns(const char * const * servers, int count);

#endif /* WIFI_CONTROL_H */
