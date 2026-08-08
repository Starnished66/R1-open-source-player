#include "wifi_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_INTERFACE "wlan0"

/* Runs `wpa_cli -i wlan0 <args>` and reads its stdout into out_buf.
 * `args` is always one of this file's own literal strings below, never
 * caller/user-controlled input, so building the command line with
 * snprintf is safe here. Returns false if wpa_cli can't be run at all
 * (not installed, no permission, etc.) or produced no output -- both
 * normal and expected on the host simulator. */
static bool run_wpa_cli(const char * args, char * out_buf, size_t out_buf_size) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "wpa_cli -i %s %s 2>/dev/null", WIFI_INTERFACE, args);

    FILE * p = popen(cmd, "r");
    if (!p) return false;

    size_t n = fread(out_buf, 1, out_buf_size - 1, p);
    out_buf[n] = '\0';
    pclose(p);
    return n > 0;
}

/* Conventional RSSI (dBm) buckets -- not extracted from the stock binary
 * (see wifi_status.h), just a reasonable mapping to the theme's 4 signal
 * icon levels. */
static int rssi_to_level(int rssi_dbm) {
    if (rssi_dbm >= -50) return 3;
    if (rssi_dbm >= -60) return 2;
    if (rssi_dbm >= -70) return 1;
    return 0;
}

bool wifi_get_status(int * out_signal_level) {
    char status_buf[512];
    if (!run_wpa_cli("status", status_buf, sizeof(status_buf))) return false;
    if (!strstr(status_buf, "wpa_state=COMPLETED")) return false;

    int level = 0;
    char signal_buf[512];
    if (run_wpa_cli("signal_poll", signal_buf, sizeof(signal_buf))) {
        const char * rssi_pos = strstr(signal_buf, "RSSI=");
        if (rssi_pos) level = rssi_to_level(atoi(rssi_pos + 5));
    }

    *out_signal_level = level;
    return true;
}
