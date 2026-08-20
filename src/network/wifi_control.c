#include "wifi_control.h"
#include "subprocess.h"

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIFI_INTERFACE "wlan0"

bool wifi_control_is_enabled(void) {
    return access("/var/run/wpa_supplicant/" WIFI_INTERFACE, F_OK) == 0;
}

/* Real-device bug report: WiFi wasn't automatically reconnecting to saved
 * networks (most visibly after suspend/resume, which tears the radio down
 * and calls this to bring it back -- see power_suspend.c's own
 * radio_restore_thread_func()). Root cause, confirmed live: /usr/bin/
 * wifi_on.sh (stock firmware script, not this project's own -- not
 * something this build ships or can patch) starts wpa_supplicant, waits a
 * hardcoded 1.3s, then fires udhcpc once with -q (exit once it gets a
 * lease, or gives up). A real AP's association + WPA handshake can easily
 * take longer than 1.3s, especially right after a radio power-cycle --
 * when it does, udhcpc's one DHCPDISCOVER attempt goes out before the
 * interface has any real L2 connectivity, gets no response, and udhcpc
 * just exits instead of retrying. Reproduced directly on-device: wpa_cli
 * status read wpa_state=COMPLETED (fully associated) with no ip_address=
 * line at all, udhcpc no longer even in the process list -- the radio was
 * genuinely connected and would stay that way forever with no IP, since
 * nothing else ever re-kicks DHCP.
 *
 * Fixed here, not in wifi_on.sh (can't patch stock firmware, and this is
 * the one place already treated as "slow, call from a background thread"
 * by every caller -- see wifi_toggle_thread_func()'s own comment): poll
 * wpa_cli status for up to ~10s for either an IP to show up on its own (the
 * script's own udhcpc call winning the race, the common case) or
 * association to complete; if it associated but still has no IP once that
 * window closes, re-run udhcpc ourselves once, the same invocation
 * wifi_on.sh's own last line uses. */
void wifi_control_enable(void) {
    char * argv[] = { (char *) "/usr/bin/wifi_on.sh", NULL };
    subprocess_run(argv, NULL, 0);

    bool associated = false, got_ip = false;
    for (int i = 0; i < 20 && !got_ip; i++) {
        usleep(500000);
        char status_buf[512];
        char * status_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "status",
                                 NULL };
        if (!subprocess_run(status_argv, status_buf, sizeof(status_buf))) continue;
        associated = strstr(status_buf, "wpa_state=COMPLETED") != NULL;
        got_ip = strstr(status_buf, "\nip_address=") != NULL;
    }
    if (associated && !got_ip) {
        char * dhcp_argv[] = { (char *) "udhcpc", (char *) "-b", (char *) "-i", (char *) WIFI_INTERFACE,
                               (char *) "-q", NULL };
        subprocess_run(dhcp_argv, NULL, 0);
    }
}

void wifi_control_disable(void) {
    char * argv[] = { (char *) "/usr/bin/wifi_off.sh", NULL };
    subprocess_run(argv, NULL, 0);
}

void wifi_control_scan_start(void) {
    char * argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "scan", NULL };
    subprocess_run(argv, NULL, 0);
}

/* Conventional RSSI (dBm) buckets, matching wifi_status.c's own -- not
 * extracted from the stock binary (there's no equivalent "list networks"
 * feature in it to reverse from), just a reasonable mapping to the same
 * 4 signal icon levels. */
static int rssi_to_level(int rssi_dbm) {
    if (rssi_dbm >= -50) return 3;
    if (rssi_dbm >= -60) return 2;
    if (rssi_dbm >= -70) return 1;
    return 0;
}

int wifi_control_get_results(wifi_network_t * out, int max_count) {
    char buf[8192];
    char * scan_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "scan_results",
                           NULL };
    if (!subprocess_run(scan_argv, buf, sizeof(buf))) return 0;

    char status_buf[512];
    char current_ssid[WIFI_MAX_SSID_LEN] = "";
    char * status_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "status", NULL };
    if (subprocess_run(status_argv, status_buf, sizeof(status_buf))) {
        const char * p = strstr(status_buf, "\nssid=");
        if (p) {
            p += 6;
            const char * end = strchr(p, '\n');
            size_t len = end ? (size_t) (end - p) : strlen(p);
            if (len >= sizeof(current_ssid)) len = sizeof(current_ssid) - 1;
            memcpy(current_ssid, p, len);
            current_ssid[len] = '\0';
        }
    }

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(buf, "\n", &line_save);
    bool first = true;
    while (line && count < max_count) {
        if (first) {
            first = false; /* "bssid / frequency / signal level / flags / ssid" header row */
            line = strtok_r(NULL, "\n", &line_save);
            continue;
        }

        char * tab_save = NULL;
        char * bssid = strtok_r(line, "\t", &tab_save);
        char * freq = strtok_r(NULL, "\t", &tab_save);
        char * signal = strtok_r(NULL, "\t", &tab_save);
        char * flags = strtok_r(NULL, "\t", &tab_save);
        char * ssid = strtok_r(NULL, "\t", &tab_save);
        (void) bssid;
        (void) freq;

        if (ssid && ssid[0] != '\0' && signal && flags) {
            int level = rssi_to_level(atoi(signal));
            bool secured = strstr(flags, "WPA") != NULL || strstr(flags, "WEP") != NULL;
            bool is_current = current_ssid[0] != '\0' && strcmp(current_ssid, ssid) == 0;

            int existing = -1;
            for (int i = 0; i < count; i++) {
                if (strcmp(out[i].ssid, ssid) == 0) {
                    existing = i;
                    break;
                }
            }
            if (existing >= 0) {
                /* Same SSID seen from another BSSID (multiple APs on one
                 * network) -- keep the strongest signal seen for it. */
                if (level > out[existing].signal_level) out[existing].signal_level = level;
            } else {
                snprintf(out[count].ssid, sizeof(out[count].ssid), "%s", ssid);
                out[count].signal_level = level;
                out[count].secured = secured;
                out[count].is_current = is_current;
                count++;
            }
        }

        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

static void bytes_to_hex(const unsigned char * data, size_t len, char * out_hex) {
    for (size_t i = 0; i < len; i++) snprintf(out_hex + i * 2, 3, "%02x", data[i]);
}

bool wifi_control_connect(const char * ssid, const char * password) {
    char out[256];

    char * add_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "add_network", NULL };
    if (!subprocess_run(add_argv, out, sizeof(out)) || strncmp(out, "FAIL", 4) == 0) return false;
    int id = atoi(out);
    if (id < 0) return false;
    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", id);

    char ssid_hex[WIFI_MAX_SSID_LEN * 2 + 1];
    size_t ssid_len = strlen(ssid);
    if (ssid_len > WIFI_MAX_SSID_LEN - 1) ssid_len = WIFI_MAX_SSID_LEN - 1;
    bytes_to_hex((const unsigned char *) ssid, ssid_len, ssid_hex);
    ssid_hex[ssid_len * 2] = '\0';

    char * set_ssid_argv[] = { (char *) "wpa_cli",     (char *) "-i",  (char *) WIFI_INTERFACE,
                               (char *) "set_network", id_str,         (char *) "ssid",
                               ssid_hex,                NULL };
    if (!subprocess_run(set_ssid_argv, out, sizeof(out)) || strncmp(out, "OK", 2) != 0) {
        char * remove_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE,
                                 (char *) "remove_network", id_str, NULL };
        subprocess_run(remove_argv, NULL, 0);
        return false;
    }

    /* scan_ssid=1 makes wpa_supplicant send active probe requests naming
     * this exact SSID instead of only matching a passively-seen beacon --
     * required for hidden (non-broadcasting) networks, which is exactly
     * what Manual SSID Entry is for. Harmless for a normal broadcasting
     * network too, so it's set unconditionally for every network added. */
    char * set_scan_ssid_argv[] = { (char *) "wpa_cli",     (char *) "-i",  (char *) WIFI_INTERFACE,
                                    (char *) "set_network", id_str,         (char *) "scan_ssid",
                                    (char *) "1",            NULL };
    subprocess_run(set_scan_ssid_argv, out, sizeof(out));

    if (password && password[0] != '\0') {
        unsigned char psk[32];
        mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1, (const unsigned char *) password, strlen(password),
                                      (const unsigned char *) ssid, strlen(ssid), 4096, sizeof(psk), psk);
        char psk_hex[sizeof(psk) * 2 + 1];
        bytes_to_hex(psk, sizeof(psk), psk_hex);
        psk_hex[sizeof(psk) * 2] = '\0';

        char * set_psk_argv[] = { (char *) "wpa_cli",     (char *) "-i",  (char *) WIFI_INTERFACE,
                                  (char *) "set_network", id_str,         (char *) "psk",
                                  psk_hex,                 NULL };
        if (!subprocess_run(set_psk_argv, out, sizeof(out)) || strncmp(out, "OK", 2) != 0) {
            char * remove_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE,
                                     (char *) "remove_network", id_str, NULL };
            subprocess_run(remove_argv, NULL, 0);
            return false;
        }
    } else {
        /* Open network -- matches /etc/wpa_supplicant.conf's own stub
         * network{} block for open networks on this device. */
        char * set_keymgmt_argv[] = { (char *) "wpa_cli",     (char *) "-i",  (char *) WIFI_INTERFACE,
                                      (char *) "set_network", id_str,         (char *) "key_mgmt",
                                      (char *) "NONE",        NULL };
        subprocess_run(set_keymgmt_argv, out, sizeof(out));
    }

    char * enable_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE,
                             (char *) "enable_network", id_str, NULL };
    subprocess_run(enable_argv, out, sizeof(out));

    char * save_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "save_config",
                           NULL };
    subprocess_run(save_argv, NULL, 0);

    return true;
}

bool wifi_control_disconnect(void) {
    char out[256];
    char * argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "disconnect", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return false;
    return strncmp(out, "OK", 2) == 0;
}

/* "network id / ssid / bssid / flags" table, same format for both
 * list_networks (all saved) and the header this skips. */
int wifi_control_list_saved(wifi_saved_network_t * out, int max_count) {
    char buf[4096];
    char * argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "list_networks", NULL };
    if (!subprocess_run(argv, buf, sizeof(buf))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(buf, "\n", &line_save);
    bool first = true;
    while (line && count < max_count) {
        if (first) {
            first = false;
            line = strtok_r(NULL, "\n", &line_save);
            continue;
        }
        char * tab_save = NULL;
        char * id_str = strtok_r(line, "\t", &tab_save);
        char * ssid = strtok_r(NULL, "\t", &tab_save);
        if (id_str && ssid) {
            out[count].id = atoi(id_str);
            snprintf(out[count].ssid, sizeof(out[count].ssid), "%s", ssid);
            count++;
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

bool wifi_control_connect_saved(int id) {
    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", id);

    char out[256];

    /* Sets scan_ssid=1 here too (not just in wifi_control_connect()) since
     * this path reuses an already-persisted profile via select_network
     * rather than recreating it -- a saved profile from before the
     * hidden-SSID fix existed would otherwise stay stuck at scan_ssid=0.
     * Harmless no-op for a normal broadcasting network; save_config
     * persists the repair so it doesn't need repeating. */
    char * set_scan_ssid_argv[] = { (char *) "wpa_cli",     (char *) "-i",  (char *) WIFI_INTERFACE,
                                    (char *) "set_network", id_str,         (char *) "scan_ssid",
                                    (char *) "1",            NULL };
    subprocess_run(set_scan_ssid_argv, out, sizeof(out));

    char * select_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "select_network",
                             id_str, NULL };
    if (!subprocess_run(select_argv, out, sizeof(out)) || strncmp(out, "OK", 2) != 0) return false;

    char * save_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "save_config", NULL };
    subprocess_run(save_argv, NULL, 0);
    return true;
}

bool wifi_control_forget(int id) {
    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", id);

    char out[256];
    char * remove_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "remove_network",
                             id_str, NULL };
    if (!subprocess_run(remove_argv, out, sizeof(out)) || strncmp(out, "OK", 2) != 0) return false;

    char * save_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "save_config",
                           NULL };
    subprocess_run(save_argv, NULL, 0);
    return true;
}

bool wifi_control_get_info(wifi_info_t * out) {
    memset(out, 0, sizeof(*out));

    char status_buf[512];
    char * status_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "status", NULL };
    if (!subprocess_run(status_argv, status_buf, sizeof(status_buf))) return false;
    if (!strstr(status_buf, "wpa_state=COMPLETED")) return false;

    const char * p = strstr(status_buf, "\nssid=");
    if (p) {
        p += 6;
        const char * end = strchr(p, '\n');
        size_t len = end ? (size_t) (end - p) : strlen(p);
        if (len >= sizeof(out->ssid)) len = sizeof(out->ssid) - 1;
        memcpy(out->ssid, p, len);
        out->ssid[len] = '\0';
    }
    p = strstr(status_buf, "\nip_address=");
    if (p) sscanf(p + 12, "%15[^\n]", out->ip);

    char signal_buf[512];
    char * signal_argv[] = { (char *) "wpa_cli", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "signal_poll",
                             NULL };
    if (subprocess_run(signal_argv, signal_buf, sizeof(signal_buf))) {
        const char * rssi_pos = strstr(signal_buf, "RSSI=");
        if (rssi_pos) out->signal_level = rssi_to_level(atoi(rssi_pos + 5));
    }

    char route_buf[256];
    char * route_argv[] = { (char *) "ip", (char *) "route", (char *) "show", (char *) "default", NULL };
    if (subprocess_run(route_argv, route_buf, sizeof(route_buf))) {
        const char * via = strstr(route_buf, "via ");
        if (via) sscanf(via + 4, "%15s", out->gateway);
    }

    FILE * f = fopen("/sys/class/net/" WIFI_INTERFACE "/address", "r");
    if (f) {
        if (fgets(out->mac, sizeof(out->mac), f)) out->mac[strcspn(out->mac, "\n")] = '\0';
        fclose(f);
    }

    return true;
}

int wifi_control_get_dns(char out[][16], int max_count) {
    FILE * f = fopen("/etc/resolv.conf", "r");
    if (!f) return 0;

    int count = 0;
    char line[128];
    while (count < max_count && fgets(line, sizeof(line), f)) {
        char ip[16];
        if (sscanf(line, "nameserver %15s", ip) == 1) {
            snprintf(out[count], 16, "%s", ip);
            count++;
        }
    }
    fclose(f);
    return count;
}

bool wifi_control_set_dns(const char * const * servers, int count) {
    FILE * f = fopen("/etc/resolv.conf", "w");
    if (!f) return false;
    for (int i = 0; i < count; i++) {
        if (servers[i][0] != '\0') fprintf(f, "nameserver %s\n", servers[i]);
    }
    fclose(f);
    return true;
}
