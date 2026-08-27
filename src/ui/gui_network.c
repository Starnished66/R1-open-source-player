extern int subprocess_run(char * const argv[], char ** out_output, int timeout_sec);
#include "gui.h"
#include "gui_network.h"
#include "gui_text_input.h"
#include "audio.h"
#include "plugin_manager.h"
#include "import_web.h"
#include "gui_settings.h"
#include "screen_builders.h"
#include "settings.h"
#include "assets.h"
#include "device_config.h"
#include "wifi_status.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
#include "usb_mode_control.h"
#include "usb_dac_bridge.h"
#include "firmware_update.h"
#include "gui_subsonic.h"
#include "airplay_control.h"
#include "airplay_bridge.h"
#include "airplay_metadata.h"
#include "dlna_control.h"
#include "remote_control.h"
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

/* Screens owned by this module */
static lv_obj_t * wifi_screen;
static lv_obj_t * wifi_info_screen;
static lv_obj_t * wifi_dns_screen;
static lv_obj_t * bt_screen;
static lv_obj_t * bt_dac_screen;
static lv_obj_t * bt_dac_overlay_screen;
static lv_obj_t * bt_dac_stream_label;
static lv_obj_t * bt_codec_screen;
static lv_obj_t * usb_mode_screen;
static lv_obj_t * usb_dac_overlay_screen;
static lv_obj_t * usb_dac_hint_label;
static lv_obj_t * usb_dac_input_label;
static lv_obj_t * usb_dac_path_label;
static lv_obj_t * import_wifi_screen;
static lv_obj_t * airplay_screen;
static lv_obj_t * airplay_overlay_screen;
static lv_obj_t * airplay_overlay_cover_img;
static lv_obj_t * airplay_overlay_title_label;
static bool airplay_overlay_showing = false;
static lv_image_dsc_t airplay_overlay_cover_dsc;
static uint8_t * airplay_overlay_cover_bytes = NULL;
/* This screen's own stack slot, recorded right before nav_push() -- same
 * import_web_stop_nav_slot pattern used below for Import via Wi-Fi's busy
 * screen. Needed because the overlay can be torn down by session end while
 * the user has already navigated to some other screen on top of it (or
 * underneath it in history); without spawning it, nav_pop() alone would
 * either pop the wrong screen or leave this now-defunct one buried in the
 * back-navigation history for a later "back" to resurface. */
static int airplay_overlay_nav_slot = -1;
static lv_obj_t * dlna_screen;
static lv_obj_t * remote_control_screen;
static lv_obj_t * wireless_screen;
static lv_obj_t * resume_mode_screen;
static lv_obj_t * resume_mode_list;
static lv_obj_t * play_pause_button_mode_screen;
static lv_obj_t * font_size_screen;
static lv_obj_t * replaygain_mode_screen;

/* Externs to gui.c state and functions */
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_remove_stack_slot(int depth);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode, lv_obj_t ** out_title, const char * body_text, const char * confirm_text, lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row, const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb, lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);
extern lv_color_t accent_lv_color(void);
extern lv_obj_t * add_pill_chevron_row(lv_obj_t * list, const char * text, lv_event_cb_t cb);
extern lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
extern lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text);
extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern void generic_back_cb(lv_event_t * e);
extern void finalize_screen_navigation(lv_obj_t * screen);
static gui_busy_handle_t wifi_connect_saved_token = 0;
extern gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
extern void gui_busy_hide(gui_busy_handle_t handle);

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif


static gui_busy_handle_t wifi_connect_token = 0;
extern bt_device_t bt_scan_results[];
extern int bt_scan_result_count;

/* ---- Wi-Fi settings screen ----
 *
 * Enable/disable toggle at top; everything else (info/manual entry/DNS,
 * memorized networks, available networks) only appears once Wi-Fi is on,
 * rebuilt into the one dynamic wifi_list container by populate_wifi_screen()
 * -- reachable both from the Wireless submenu's Wi-Fi tile and from a
 * long-press on the quick-access drawer's wifi icon (see
 * quick_drawer_wifi_long_press_cb near build_quick_drawer). Scan/connect/
 * disconnect/forget all share the same background-thread-plus-polled-flag
 * shape as the Subsonic connect/download machinery above, and reuse its
 * subsonic_downloading_screen/label as the shared "please wait" interstitial
 * rather than building a second one. */

#define WIFI_MAX_RESULTS 32
#define WIFI_MAX_SAVED 32
static wifi_network_t wifi_scan_results[WIFI_MAX_RESULTS];
static int wifi_scan_result_count = 0;
static wifi_saved_network_t wifi_saved_results[WIFI_MAX_SAVED];
static int wifi_saved_result_count = 0;
static lv_obj_t * wifi_list;
static lv_obj_t * wifi_rescan_btn;
static bool wifi_enable_pending_feedback = false;
static char wifi_connect_pending_ssid[WIFI_MAX_SSID_LEN];

static void start_wifi_scan(void);

static pthread_t wifi_connect_thread;
static bool wifi_connect_active = false;
static atomic_bool wifi_connect_done_flag = false;
static volatile bool wifi_connect_succeeded = false;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char password[256];
} wifi_connect_request_t;

static void * wifi_connect_thread_func(void * arg) {
    wifi_connect_request_t * req = (wifi_connect_request_t *) arg;
    bool accepted = wifi_control_connect(req->ssid, req->password[0] != '\0' ? req->password : NULL);
    free(req);

    /* wifi_control_connect() returning true only confirms wpa_cli accepted
     * the configuration, not that association actually happened -- a wrong
     * password fails silently at that layer (see its own doc comment).
     * Poll the real link state for a few seconds before calling this a
     * genuine success, same real-world wait the "Connecting to X..." screen
     * already covers. */
    bool associated = false;
    if (accepted) {
        for (int i = 0; i < 10 && !associated; i++) {
            int level;
            associated = wifi_get_status(&level);
            if (!associated) usleep(500000);
        }
    }
    wifi_connect_succeeded = accepted && associated;
    atomic_store_explicit(&wifi_connect_done_flag, true, memory_order_release); /* written last -- poll_wifi_connect only checks this flag */
    return NULL;
}

static void start_wifi_connect(const char * ssid, const char * password) {
    wifi_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->ssid, sizeof(req->ssid), "%s", ssid);
    snprintf(req->password, sizeof(req->password), "%s", password ? password : "");

    atomic_store_explicit(&wifi_connect_done_flag, false, memory_order_relaxed);
    wifi_connect_active = true;

    wifi_connect_token = gui_busy_show("Connecting to", ssid);

        if (pthread_create(&wifi_connect_thread, NULL, wifi_connect_thread_func, req) != 0) {
        wifi_connect_active = false;
        free(req);
        gui_busy_hide(wifi_connect_token);
        show_error_toast("Thread launch failed");
    }
}

void poll_wifi_connect(void) {
    if (!wifi_connect_active || !atomic_load_explicit(&wifi_connect_done_flag, memory_order_acquire)) return;

    wifi_connect_active = false;
    pthread_join(wifi_connect_thread, NULL);
    gui_busy_hide(wifi_connect_token);
    if (!wifi_connect_succeeded) {
        show_error_toast("Couldn't connect to Wi-Fi network");
    }
    start_wifi_scan(); /* refresh so the list reflects the new connection state */
}

/* Reconnecting to an already-memorized network -- deliberately a separate
 * thread/flag pair from the plain connect above (not a shared "mode" on the
 * same one) rather than reusing wifi_connect_active's state, matching this
 * file's existing convention of one dedicated pair per background Wi-Fi
 * operation (scan/connect/disconnect/forget already each have their own).
 * See wifi_control_connect_saved()'s own doc comment for why this can't
 * just call start_wifi_connect(ssid, NULL) instead. */
static pthread_t wifi_connect_saved_thread;
static bool wifi_connect_saved_active = false;
static atomic_bool wifi_connect_saved_done_flag = false;
static volatile bool wifi_connect_saved_succeeded = false;

typedef struct {
    int id;
} wifi_connect_saved_request_t;

static void * wifi_connect_saved_thread_func(void * arg) {
    wifi_connect_saved_request_t * req = (wifi_connect_saved_request_t *) arg;
    bool accepted = wifi_control_connect_saved(req->id);
    free(req);

    bool associated = false;
    if (accepted) {
        for (int i = 0; i < 10 && !associated; i++) {
            int level;
            associated = wifi_get_status(&level);
            if (!associated) usleep(500000);
        }
    }
    wifi_connect_saved_succeeded = accepted && associated;
    atomic_store_explicit(&wifi_connect_saved_done_flag, true, memory_order_release); /* written last -- poll_wifi_connect_saved only checks this flag */
    return NULL;
}

static void start_wifi_connect_saved(int id, const char * ssid) {
    wifi_connect_saved_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->id = id;

    atomic_store_explicit(&wifi_connect_saved_done_flag, false, memory_order_relaxed);
    wifi_connect_saved_active = true;

    wifi_connect_saved_token = gui_busy_show("Connecting to", ssid);

        if (pthread_create(&wifi_connect_saved_thread, NULL, wifi_connect_saved_thread_func, req) != 0) {
        wifi_connect_saved_active = false;
        free(req);
        gui_busy_hide(wifi_connect_saved_token);
        show_error_toast("Thread launch failed");
    }
}

void poll_wifi_connect_saved(void) {
    if (!wifi_connect_saved_active || !atomic_load_explicit(&wifi_connect_saved_done_flag, memory_order_acquire)) return;

    wifi_connect_saved_active = false;
    pthread_join(wifi_connect_saved_thread, NULL);
    gui_busy_hide(wifi_connect_saved_token);
    if (!wifi_connect_saved_succeeded) {
        show_error_toast("Couldn't connect to Wi-Fi network");
    }
    start_wifi_scan(); /* refresh so the list reflects the new connection state */
}

static pthread_t wifi_disconnect_thread;
static bool wifi_disconnect_active = false;
static atomic_bool wifi_disconnect_done_flag = false;

static void * wifi_disconnect_thread_func(void * arg) {
    (void) arg;
    wifi_control_disconnect();
    atomic_store_explicit(&wifi_disconnect_done_flag, true, memory_order_release); /* written last -- poll_wifi_disconnect only checks this flag */
    return NULL;
}

static void start_wifi_disconnect(void) {
    atomic_store_explicit(&wifi_disconnect_done_flag, false, memory_order_relaxed);
    wifi_disconnect_active = true;
        if (pthread_create(&wifi_disconnect_thread, NULL, wifi_disconnect_thread_func, NULL) != 0) {
        wifi_disconnect_active = false;
        show_error_toast("Thread launch failed");
    }
}

void poll_wifi_disconnect(void) {
    if (!wifi_disconnect_active || !atomic_load_explicit(&wifi_disconnect_done_flag, memory_order_acquire)) return;
    wifi_disconnect_active = false;
    pthread_join(wifi_disconnect_thread, NULL);
    start_wifi_scan(); /* refresh so the list reflects the disconnected state */
}

static void wifi_password_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    start_wifi_connect(wifi_connect_pending_ssid, text);
}

static void wifi_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    wifi_network_t * net = &wifi_scan_results[index];
    if (net->is_current) {
        start_wifi_disconnect();
        return;
    }

    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", net->ssid);
    if (net->secured) {
        show_text_entry("Wi-Fi Password", NULL, true, false, wifi_password_entered_cb, NULL);
    } else {
        start_wifi_connect(net->ssid, NULL);
    }
}

static void wifi_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* An explicit rescan is a new discovery snapshot, not an append to the
     * previous one. Clear stale access points immediately so the screen
     * cannot imply that an old result is still currently visible while the
     * radio is collecting its replacement set. Saved networks remain in
     * their own section because they are configuration, not scan results. */
    wifi_scan_result_count = 0;
    populate_wifi_screen(wifi_control_is_enabled());
    start_wifi_scan();
}

static void set_wifi_rescan_active(bool active) {
    if (!wifi_rescan_btn) return;
    lv_label_set_text(wifi_rescan_btn, active ? "Scanning..." : "Rescan");
    lv_obj_set_style_text_color(wifi_rescan_btn,
                                active ? lv_color_make(160, 160, 160) : accent_lv_color(), 0);
    lv_obj_align(wifi_rescan_btn, LV_ALIGN_TOP_RIGHT, -20,
                 STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    if (active) {
        lv_obj_remove_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(wifi_rescan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(wifi_rescan_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

static pthread_t wifi_scan_thread;
static bool wifi_scan_active = false;
static atomic_bool wifi_scan_done_flag = false;

static void * wifi_scan_thread_func(void * arg) {
    (void) arg;
    /* No auto-enable here anymore -- the screen's own toggle row is now the
     * explicit way to turn Wi-Fi on, so a stray Rescan tap while it's off
     * should just find nothing rather than silently flipping the radio on
     * behind the toggle's back. */
    wifi_control_scan_start();
    sleep(3); /* wpa_cli's scan is async -- give the radio time before reading scan_results */
    wifi_scan_result_count = wifi_control_get_results(wifi_scan_results, WIFI_MAX_RESULTS);
    atomic_store_explicit(&wifi_scan_done_flag, true, memory_order_release); /* written last -- poll_wifi_scan only checks this flag */
    return NULL;
}

/* Runs fully in the background, same as the stock player -- no busy
 * screen, no navigation at all. The list just updates in place
 * (populate_wifi_screen(), below) once the scan finishes, whichever screen
 * the user happens to be on by then. An earlier version pushed a
 * "Scanning for networks..." interstitial shared with bt scan/the bt
 * toggle's own overlay -- see git history if that's ever needed again, but
 * it was also the source of a real, repeatedly-hit stuck-screen bug
 * (multiple uncoordinated users of one shared overlay, plus the overlay
 * getting buried under further navigation), which not having an overlay at
 * all sidesteps entirely. */
static void start_wifi_scan(void) {
    if (wifi_scan_active) return;

    atomic_store_explicit(&wifi_scan_done_flag, false, memory_order_relaxed);
    wifi_scan_active = true;
    set_wifi_rescan_active(true);

    if (pthread_create(&wifi_scan_thread, NULL, wifi_scan_thread_func, NULL) != 0) {
        wifi_scan_active = false;
        set_wifi_rescan_active(false);
    }
}

void poll_wifi_scan(void) {
    if (!wifi_scan_active || !atomic_load_explicit(&wifi_scan_done_flag, memory_order_acquire)) return;

    wifi_scan_active = false;
    pthread_join(wifi_scan_thread, NULL);
    set_wifi_rescan_active(false);
    populate_wifi_screen(wifi_control_is_enabled()); /* refresh the list either way -- worth keeping current even if the user already backed out */
}

static void show_wifi_toggle_pending_async(void * user_data) {
    bool enabled = (bool) (intptr_t) user_data;
    if (!gui_navigation_is_top(gui_network_get_wifi_screen())) return;

    wifi_enable_pending_feedback = enabled;
    if (enabled) wifi_scan_result_count = 0;
    populate_wifi_screen(enabled);
    set_wifi_rescan_active(enabled || wifi_scan_active);
}

void gui_network_show_wifi_toggle_pending(bool enabled) {
    /* Rebuilding wifi_list synchronously from its toggle row callback would
     * delete LVGL's active event target. Defer by one UI turn, matching the
     * safe pattern used elsewhere for destructive screen rebuilds. */
    lv_async_call(show_wifi_toggle_pending_async, (void *) (intptr_t) enabled);
}

void gui_network_wifi_toggle_completed(bool enabled) {
    wifi_enable_pending_feedback = false;
    if (!gui_navigation_is_top(gui_network_get_wifi_screen())) {
        set_wifi_rescan_active(wifi_scan_active);
        return;
    }

    populate_wifi_screen(enabled);
    if (enabled) start_wifi_scan();
    else set_wifi_rescan_active(wifi_scan_active);
}

static pthread_t wifi_forget_thread;
static bool wifi_forget_active = false;
static atomic_bool wifi_forget_done_flag = false;

typedef struct {
    int id;
} wifi_forget_request_t;

static void * wifi_forget_thread_func(void * arg) {
    wifi_forget_request_t * req = (wifi_forget_request_t *) arg;
    wifi_control_forget(req->id);
    free(req);
    atomic_store_explicit(&wifi_forget_done_flag, true, memory_order_release); /* written last -- poll_wifi_forget only checks this flag */
    return NULL;
}

static void start_wifi_forget(int id) {
    wifi_forget_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->id = id;
    atomic_store_explicit(&wifi_forget_done_flag, false, memory_order_relaxed);
    wifi_forget_active = true;
        if (pthread_create(&wifi_forget_thread, NULL, wifi_forget_thread_func, req) != 0) {
        wifi_forget_active = false;
        free(req);
    }
}

void poll_wifi_forget(void) {
    if (!wifi_forget_active || !atomic_load_explicit(&wifi_forget_done_flag, memory_order_acquire)) return;
    wifi_forget_active = false;
    pthread_join(wifi_forget_thread, NULL);
    start_wifi_scan(); /* refresh both memorized and available sections */
}

/* ---- Wi-Fi saved-network action popup ------------------------------------
 * Real-device bug report: tapping a memorized network used to call
 * start_wifi_forget() directly and unconditionally -- there was no way to
 * just reconnect to one, and no confirmation before it got forgotten either.
 * Mirrors bt_action_popup's exact shape (same hand-built top-layer overlay,
 * same tap-outside-to-dismiss backdrop) for a consistent feel between the
 * two screens, per that same feedback ("just like on the bluetooth page").
 * Unlike Bluetooth's popup, both actions are always offered here -- a
 * memorized network is by definition not the live association status, so
 * "Connect" is always at least plausible (also covers reconnecting after a
 * manual disconnect), and "Forget" always applies to anything memorized. */
static lv_obj_t * wifi_action_popup;
static lv_obj_t * wifi_action_popup_backdrop;
static lv_obj_t * wifi_action_popup_title;
static int wifi_action_popup_network_index = -1;

static void hide_wifi_action_popup(void) {
    lv_obj_add_flag(wifi_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_action_popup, LV_OBJ_FLAG_HIDDEN);
    wifi_action_popup_network_index = -1;
}

static void wifi_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_wifi_action_popup();
}

static void wifi_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (wifi_action_popup_network_index < 0 || wifi_action_popup_network_index >= wifi_saved_result_count) {
        hide_wifi_action_popup();
        return;
    }
    wifi_saved_network_t * net = &wifi_saved_results[wifi_action_popup_network_index];
    int id = net->id;
    char ssid[WIFI_MAX_SSID_LEN];
    snprintf(ssid, sizeof(ssid), "%s", net->ssid);
    hide_wifi_action_popup();
    start_wifi_connect_saved(id, ssid);
}

static void wifi_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (wifi_action_popup_network_index < 0 || wifi_action_popup_network_index >= wifi_saved_result_count) {
        hide_wifi_action_popup();
        return;
    }
    int id = wifi_saved_results[wifi_action_popup_network_index].id;
    hide_wifi_action_popup();
    start_wifi_forget(id);
}

void build_wifi_action_popup(void) {
    wifi_action_popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &wifi_action_popup_title, NULL, "Connect",
                                             accent_lv_color(), wifi_action_connect_cb, NULL, "Forget",
                                             lv_color_make(255, 120, 120), wifi_action_forget_cb, NULL,
                                             wifi_action_popup_backdrop_cb, &wifi_action_popup_backdrop);
}

static void show_wifi_action_popup(int index) {
    wifi_action_popup_network_index = index;
    lv_label_set_text(wifi_action_popup_title, wifi_saved_results[index].ssid);

    lv_obj_remove_flag(wifi_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_action_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_action_popup_backdrop);
    lv_obj_move_foreground(wifi_action_popup);
}

static void wifi_saved_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_wifi_action_popup(index);
}

/* ---- Wi-Fi Info screen (read-only) ---- */

static lv_obj_t * wifi_info_list;

static void populate_wifi_info_screen(void) {
    lv_obj_clean(wifi_info_list);

    wifi_info_t info;
    if (!wifi_control_get_info(&info)) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, "Not connected");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }

    static const char * signal_labels[] = { "Weak", "Fair", "Good", "Excellent" };
    int signal_index = (info.signal_level >= 0 && info.signal_level <= 3) ? info.signal_level : 0;
    char lines[5][80];
    snprintf(lines[0], sizeof(lines[0]), "SSID: %s", info.ssid);
    snprintf(lines[1], sizeof(lines[1]), "IP Address: %s", info.ip[0] ? info.ip : "-");
    snprintf(lines[2], sizeof(lines[2]), "Gateway: %s", info.gateway[0] ? info.gateway : "-");
    snprintf(lines[3], sizeof(lines[3]), "MAC Address: %s", info.mac[0] ? info.mac : "-");
    snprintf(lines[4], sizeof(lines[4]), "Signal: %s", signal_labels[signal_index]);

    for (int i = 0; i < 5; i++) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, lines[i]);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        lv_obj_set_style_pad_top(label, 12, 0);
    }
}

static lv_obj_t * build_wifi_info_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Wi-Fi Info", &title_label, &wifi_info_list);
}

static void open_wifi_info_screen(void) {
    populate_wifi_info_screen();
    nav_push(wifi_info_screen);
}

static void wifi_info_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_info_screen();
}

/* ---- Manual SSID entry -- chains the same show_text_entry() flow scan
 * results already use for a secured network's password, just starting from
 * a typed SSID instead of a tapped scan row. An empty password submission
 * connects as an open network (start_wifi_connect() already treats an empty
 * password as "no password"), so there's no separate "is this secured?"
 * question to ask up front. ---- */

static void wifi_manual_ssid_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", text);
    show_text_entry("Wi-Fi Password", NULL, true, false, wifi_password_entered_cb, NULL);
}

static void wifi_manual_entry_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Network Name (SSID)", NULL, false, false, wifi_manual_ssid_entered_cb, NULL);
}

/* ---- DNS settings screen -- view/edit the 2 servers currently in
 * /etc/resolv.conf (see wifi_control_get_dns()'s own doc comment for why
 * this is a "until the next DHCP renewal" override, not a permanent one).
 * ---- */

static lv_obj_t * wifi_dns_list;
static char wifi_dns_servers[2][16];
static int wifi_dns_server_count = 0;

static void wifi_dns_row_cb(lv_event_t * e);

static void populate_wifi_dns_screen(void) {
    lv_obj_clean(wifi_dns_list);

    char current[2][16];
    int count = wifi_control_get_dns(current, 2);
    for (int i = 0; i < 2; i++) {
        snprintf(wifi_dns_servers[i], sizeof(wifi_dns_servers[i]), "%s", i < count ? current[i] : "");
    }
    wifi_dns_server_count = count;

    static const char * slot_labels[2] = { "Primary DNS", "Secondary DNS" };
    for (int i = 0; i < 2; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%s: %s", slot_labels[i], wifi_dns_servers[i][0] ? wifi_dns_servers[i] : "Not set");
        lv_obj_t * row = add_pill_chevron_row(wifi_dns_list, text, NULL);
        lv_obj_add_event_cb(row, wifi_dns_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void wifi_dns_entered_cb(const char * text, void * user_data) {
    int slot = (int) (intptr_t) user_data;
    snprintf(wifi_dns_servers[slot], sizeof(wifi_dns_servers[slot]), "%s", text);
    if (slot + 1 > wifi_dns_server_count) wifi_dns_server_count = slot + 1;

    const char * servers[2] = { wifi_dns_servers[0], wifi_dns_servers[1] };
    wifi_control_set_dns(servers, wifi_dns_server_count);
    populate_wifi_dns_screen();
}

static void wifi_dns_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int slot = (int) (intptr_t) lv_event_get_user_data(e);
    show_text_entry(slot == 0 ? "Primary DNS" : "Secondary DNS", wifi_dns_servers[slot], false, false, wifi_dns_entered_cb,
                    (void *) (intptr_t) slot);
}

static lv_obj_t * build_wifi_dns_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("DNS Settings", &title_label, &wifi_dns_list);
}

static void open_wifi_dns_screen(void) {
    populate_wifi_dns_screen();
    nav_push(wifi_dns_screen);
}

static void wifi_dns_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_dns_screen();
}

/* ---- Master rebuild for the Wi-Fi screen's one dynamic list: toggle row,
 * then (only while enabled) the info/manual-entry/DNS rows, memorized
 * networks, and available networks -- called after every state change
 * (toggle, scan, connect, disconnect, forget) rather than patched
 * incrementally, same full-rebuild convention as every other list in this
 * file. ---- */
/* enabled is passed in rather than read via wifi_control_is_enabled()
 * internally so the tap-to-toggle handler (quick_drawer_wifi_event_cb) can
 * pass its own optimistic target state instead of the real (not-yet-caught-
 * up) one -- see that function's own comment. Every other caller just
 * passes the real wifi_control_is_enabled() value, same as before this was
 * a parameter. */
void populate_wifi_screen(bool enabled) {
    lv_obj_clean(wifi_list);

    if (wifi_rescan_btn) {
        if (enabled) lv_obj_remove_flag(wifi_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* quick_drawer_wifi_event_cb is the SAME real toggle-thread trigger the
     * drawer's own wifi icon uses -- no drawer-specific logic in it, so
     * it's reused directly here rather than duplicating the thread-kickoff
     * boilerplate. poll_wifi_toggle() repopulates this screen once the
     * toggle completes. */
    add_pill_toggle_row(wifi_list, "Wi-Fi", enabled, quick_drawer_wifi_event_cb);

    if (!enabled) return;

    add_pill_chevron_row(wifi_list, "Wi-Fi Info", wifi_info_row_cb);
    add_pill_chevron_row(wifi_list, "Manual SSID Entry", wifi_manual_entry_row_cb);
    add_pill_chevron_row(wifi_list, "DNS Settings", wifi_dns_settings_row_cb);

    add_section_header(wifi_list, "Memorized Networks");
    /* During an optimistic cold enable there is no wpa_supplicant control
     * socket yet. Keep the last cached saved-network rows instead of
     * launching synchronous wpa_cli subprocesses that cannot succeed and
     * would undermine the immediate feedback this state exists to provide. */
    if (!wifi_enable_pending_feedback)
        wifi_saved_result_count = wifi_control_list_saved(wifi_saved_results, WIFI_MAX_SAVED);
    if (wifi_saved_result_count == 0) {
        lv_obj_t * label = lv_label_create(wifi_list);
        lv_label_set_text(label, "No memorized networks");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }
    /* Real-device bug report: no indicator of which saved network is
     * actually connected -- Available Networks below already shows this
     * (net->is_current, from the scan results), but the scan and the
     * saved-network list are two entirely separate queries (wpa_cli
     * status vs. list_networks), so nothing here ever cross-referenced
     * them. wifi_control_get_info() (already used by the Wi-Fi Info
     * screen) is the same "ask wpa_cli status directly" source of truth
     * as is_current itself, just reused here by SSID match instead of
     * scan-result identity. */
    wifi_info_t current_wifi_info;
    bool wifi_currently_connected = !wifi_enable_pending_feedback &&
                                    wifi_control_get_info(&current_wifi_info);
    for (int i = 0; i < wifi_saved_result_count; i++) {
        lv_obj_t * row = lv_obj_create(wifi_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        bool is_connected = wifi_currently_connected && strcmp(wifi_saved_results[i].ssid, current_wifi_info.ssid) == 0;
        char text[WIFI_MAX_SSID_LEN + 20];
        snprintf(text, sizeof(text), "%s%s", wifi_saved_results[i].ssid, is_connected ? "  - Connected" : "");

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, text);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_saved_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }

    add_section_header(wifi_list, "Available Networks");
    for (int i = 0; i < wifi_scan_result_count; i++) {
        wifi_network_t * net = &wifi_scan_results[i];

        lv_obj_t * row = lv_obj_create(wifi_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * icon = lv_image_create(row);
        char icon_path[40];
        snprintf(icon_path, sizeof(icon_path), "topbar/wifi_connect_%d.png", net->signal_level);
        lv_image_set_src(icon, asset_path(icon_path));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);

        lv_obj_t * label = lv_label_create(row);
        char text[256];
        snprintf(text, sizeof(text), "%.128s%s%s", net->ssid, net->secured ? "  (secured)" : "",
                 net->is_current ? "  - Connected" : "");
        lv_label_set_text(label, text);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 64, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
    if (wifi_scan_result_count == 0) {
        lv_obj_t * label = lv_label_create(wifi_list);
        lv_label_set_text(label, wifi_enable_pending_feedback ? "Scanning for networks..." : "No networks found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }
}

static lv_obj_t * build_wifi_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    lv_obj_t * scr = build_subsonic_list_screen("Wi-Fi", &title_label, &wifi_list);

    wifi_rescan_btn = lv_label_create(scr);
    lv_label_set_text(wifi_rescan_btn, "Rescan");
    lv_obj_set_style_text_color(wifi_rescan_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(wifi_rescan_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(wifi_rescan_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_rescan_btn, wifi_rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

void open_wifi_screen(void) {
    bool enabled = wifi_control_is_enabled();
    populate_wifi_screen(enabled);
    nav_push(wifi_screen);
    if (enabled) start_wifi_scan(); /* auto-refresh -- matches the old always-scan-on-open behavior, but only when there's a radio to scan with */
}

static void wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_screen();
}

/* ---- Bluetooth settings screen ----
 *
 * Same shape as the Wi-Fi screen above: full scan + pair/connect, reachable
 * from the Wireless submenu's Bluetooth tile and from a long-press on the
 * drawer's bt icon. bt_control_scan() blocks for its whole scan window
 * itself (bluetoothctl's own --timeout), so the scan thread has no separate
 * sleep to add, unlike wifi_scan_thread_func above. */

static lv_obj_t * bt_list;

static void start_bt_scan(void);

static pthread_t bt_connect_thread;
static bool bt_connect_active = false;
static atomic_bool bt_connect_done_flag = false;
static volatile bool bt_connect_succeeded = false;

/* Which device's row (see add_bt_device_row()) should show inline
 * "Connecting..."/"Failed to connect" status -- an earlier version of this
 * blocked the whole screen with a "Pairing & connecting..." overlay
 * instead, same class of issue as the scan/toggle overlays removed
 * elsewhere in this file (see start_wifi_scan()'s comment): running fully
 * in the background with inline per-row feedback, matching the stock
 * player, means there's no screen to get stuck on at all. */
static char bt_connecting_mac[18] = "";
static char bt_connect_failed_mac[18] = "";

typedef struct {
    char mac[18];
} bt_connect_request_t;

static void * bt_connect_thread_func(void * arg) {
    bt_connect_request_t * req = (bt_connect_request_t *) arg;
    bt_connect_succeeded = bt_control_connect(req->mac);
    free(req);
    atomic_store_explicit(&bt_connect_done_flag, true, memory_order_release); /* written last -- poll_bt_connect only checks this flag */
    return NULL;
}

static void start_bt_connect(const char * mac) {
    if (bt_connect_active) return; /* one at a time -- bt_connect_thread/succeeded are single shared slots */

    bt_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->mac, sizeof(req->mac), "%s", mac);

    atomic_store_explicit(&bt_connect_done_flag, false, memory_order_relaxed);
    bt_connect_active = true;
    snprintf(bt_connecting_mac, sizeof(bt_connecting_mac), "%s", mac);
    bt_connect_failed_mac[0] = '\0';
    populate_bt_screen(); /* immediate inline "Connecting..." on that row */

        if (pthread_create(&bt_connect_thread, NULL, bt_connect_thread_func, req) != 0) {
        bt_connect_active = false;
        free(req);
        if (bt_list) lv_obj_invalidate(bt_list);
    }
}

void poll_bt_connect(void) {
    if (!bt_connect_active || !atomic_load_explicit(&bt_connect_done_flag, memory_order_acquire)) return;

    bt_connect_active = false;
    pthread_join(bt_connect_thread, NULL);
    if (!bt_connect_succeeded) {
        snprintf(bt_connect_failed_mac, sizeof(bt_connect_failed_mac), "%s", bt_connecting_mac);
    }
    bt_connecting_mac[0] = '\0';
    start_bt_scan(); /* refresh so the list reflects the new pair/connect state -- runs in the background too, see its own comment */
}

static pthread_t bt_forget_thread;
static bool bt_forget_active = false;
static atomic_bool bt_forget_done_flag = false;

typedef struct {
    char mac[18];
} bt_forget_request_t;

/* Kept alive past the thread completing (freed in poll_bt_forget() instead
 * of the thread func itself) so the completion handler still knows which
 * device to update in bt_scan_results -- see poll_bt_forget()'s own
 * comment for why. */
static bt_forget_request_t * bt_forget_pending_req = NULL;

static void * bt_forget_thread_func(void * arg) {
    bt_forget_request_t * req = (bt_forget_request_t *) arg;
    bt_control_forget(req->mac);
    atomic_store_explicit(&bt_forget_done_flag, true, memory_order_release); /* written last -- poll_bt_forget only checks this flag */
    return NULL;
}

static void start_bt_forget(const char * mac) {
    bt_forget_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->mac, sizeof(req->mac), "%s", mac);
    bt_forget_pending_req = req;

    atomic_store_explicit(&bt_forget_done_flag, false, memory_order_relaxed);
    bt_forget_active = true;
        if (pthread_create(&bt_forget_thread, NULL, bt_forget_thread_func, req) != 0) {
        bt_forget_active = false;
        free(req);
    }
}

/* Real-device feedback: forgetting a device used to refresh the list via
 * start_bt_scan() -- a brand new 6+ second discovery scan with its own busy
 * overlay flashing on screen, surprising ("it shouldn't be triggered") for
 * what's really just a local state change we already know the outcome of.
 * Updating bt_scan_results directly and redrawing is instant and needs no
 * scan at all -- the forgotten device just moves from the Paired section to
 * Available (or disappears next real scan, if it's not actually in range). */
void poll_bt_forget(void) {
    if (!bt_forget_active || !atomic_load_explicit(&bt_forget_done_flag, memory_order_acquire)) return;
    bt_forget_active = false;
    pthread_join(bt_forget_thread, NULL);

    for (int i = 0; i < bt_scan_result_count; i++) {
        if (strcmp(bt_scan_results[i].mac, bt_forget_pending_req->mac) == 0) {
            bt_scan_results[i].paired = false;
            bt_scan_results[i].connected = false;
            break;
        }
    }
    free(bt_forget_pending_req);
    bt_forget_pending_req = NULL;
    populate_bt_screen();
}

/* ---- Bluetooth device action popup --------------------------------------
 * Tapping a device row used to implicitly Forget (if ->connected) or
 * Connect (otherwise) -- that meant a paired-but-not-currently-connected
 * device had NO path to being forgotten at all, since the only route to
 * start_bt_forget() required ->connected. This replaces that with an
 * explicit popup offering just the actions that make sense for the
 * device's actual state. Same hand-built top-layer overlay shape as
 * error_toast/volume_popup above (this codebase doesn't use LVGL's
 * lv_msgbox anywhere), plus a tap-outside-to-dismiss backdrop. */
static lv_obj_t * bt_action_popup;
static lv_obj_t * bt_action_popup_backdrop;
static lv_obj_t * bt_action_popup_title;
static lv_obj_t * bt_action_connect_row;
static lv_obj_t * bt_action_forget_row;
static int bt_action_popup_device_index = -1;

static void hide_bt_action_popup(void) {
    lv_obj_add_flag(bt_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bt_action_popup, LV_OBJ_FLAG_HIDDEN);
    bt_action_popup_device_index = -1;
}

static void bt_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_action_popup();
}

static void bt_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (bt_action_popup_device_index < 0 || bt_action_popup_device_index >= bt_scan_result_count) {
        hide_bt_action_popup();
        return;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", bt_scan_results[bt_action_popup_device_index].mac);
    hide_bt_action_popup();
    start_bt_connect(mac);
}

static void bt_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (bt_action_popup_device_index < 0 || bt_action_popup_device_index >= bt_scan_result_count) {
        hide_bt_action_popup();
        return;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", bt_scan_results[bt_action_popup_device_index].mac);
    hide_bt_action_popup();
    start_bt_forget(mac);
}

void build_bt_action_popup(void) {
    bt_action_popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &bt_action_popup_title, NULL, "Connect",
                                           accent_lv_color(), bt_action_connect_cb, &bt_action_connect_row, "Forget",
                                           lv_color_make(255, 120, 120), bt_action_forget_cb, &bt_action_forget_row,
                                           bt_action_popup_backdrop_cb, &bt_action_popup_backdrop);
}

static void show_bt_action_popup(int index) {
    bt_action_popup_device_index = index;
    bt_device_t * dev = &bt_scan_results[index];

    lv_label_set_text(bt_action_popup_title, dev->name[0] != '\0' ? dev->name : dev->mac);

    /* Connect makes sense unless already connected. Forget makes sense for
     * anything paired OR connected (the "OR connected" is just a safety net
     * -- a connected-but-somehow-not-paired device shouldn't be able to land
     * on a popup with neither action available). */
    bool show_connect = !dev->connected;
    bool show_forget = dev->paired || dev->connected;
    /* No manual reposition needed -- bt_action_popup's rows are flex-column
     * children now (build_confirm_popup()), so hiding one lets the other
     * reflow up to take its place automatically. */
    if (show_connect) lv_obj_remove_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    if (show_forget) lv_obj_remove_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(bt_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(bt_action_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bt_action_popup_backdrop);
    lv_obj_move_foreground(bt_action_popup);
}

static void bt_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_bt_action_popup(index);
}

static void bt_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* bt_scan_results contains both paired devices and transient discovery
     * results. Preserve the former (pairing is persistent state), but drop
     * every old Available Devices entry before beginning a new discovery
     * window. The completed scan replaces this compacted set as usual. */
    int paired_count = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (!bt_scan_results[i].paired) continue;
        if (paired_count != i) bt_scan_results[paired_count] = bt_scan_results[i];
        paired_count++;
    }
    bt_scan_result_count = paired_count;
    populate_bt_screen();
    start_bt_scan();
}

static lv_obj_t * bt_rescan_btn;

static void set_bt_rescan_active(bool active) {
    if (!bt_rescan_btn) return;
    lv_label_set_text(bt_rescan_btn, active ? "Scanning..." : "Rescan");
    lv_obj_set_style_text_color(bt_rescan_btn,
                                active ? lv_color_make(160, 160, 160) : accent_lv_color(), 0);
    lv_obj_align(bt_rescan_btn, LV_ALIGN_TOP_RIGHT, -20,
                 STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    if (active) {
        lv_obj_remove_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(bt_rescan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(bt_rescan_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* Real-device finding (confirmed via `bluetoothctl devices` directly): a
 * device with no advertised name doesn't come back with an empty name at
 * all -- bluetoothctl itself substitutes the device's own MAC address,
 * dashes instead of colons (e.g. mac "5D:C4:96:7E:56:88" -> name
 * "5D-C4-96-7E-56-88"), as a placeholder. bt_scan_results[i].name[0] ==
 * '\0' (an earlier version of the "hide unnamed devices" filter below)
 * essentially never matches real-world unnamed devices because of this --
 * confirmed live as "not reliable, doesn't hide them". Detects that exact
 * placeholder shape instead of assuming an empty string. */
static bool bt_name_is_mac_placeholder(const bt_device_t * dev) {
    if (dev->name[0] == '\0') return true;
    size_t mac_len = strlen(dev->mac);
    if (strlen(dev->name) != mac_len) return false;
    for (size_t i = 0; i < mac_len; i++) {
        char m = dev->mac[i];
        char n = dev->name[i];
        if (m == ':') { if (n != '-' && n != ':') return false; }
        else if (m != n) return false;
    }
    return true;
}

/* Extra row height (beyond LIST_ROW_WIDTH's normal LIST_ROW_HEIGHT) when a
 * row shows the codec subtitle line -- one more small-font line plus a
 * little breathing room, not a full second LIST_ROW_HEIGHT line. */
#define BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT 40

static void add_bt_device_row(lv_obj_t * parent, int index) {
    bt_device_t * dev = &bt_scan_results[index];

    /* Codec is only meaningful for the ONE device actually carrying A2DP
     * audio right now -- dev->connected alone isn't enough (a paired,
     * link-level-connected non-audio BLE peripheral would also read
     * connected=true here), so this also checks the row's own MAC against
     * bt_connected_mac_cached, the accessory bt_control_get_connected_
     * device_mac()/_codec() actually queried. Both come from the same
     * background poll as the rest of this screen's live state (see
     * refresh_bt_icon_thread_func()) -- never a synchronous bluealsa-cli
     * call here on the UI thread. */
    bool show_codec = dev->connected && bt_connected_codec_cached[0] != '\0' &&
                       strcasecmp(dev->mac, bt_connected_mac_cached) == 0;

    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT + (show_codec ? BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT : 0));
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    char text[96];
    const char * status;
    if (strcmp(dev->mac, bt_connecting_mac) == 0) status = "  - Connecting...";
    else if (strcmp(dev->mac, bt_connect_failed_mac) == 0) status = "  - Failed to connect";
    else if (dev->connected) status = "  - Connected";
    else if (dev->paired) status = "  - Paired";
    else status = "";
    snprintf(text, sizeof(text), "%s%s", dev->name[0] != '\0' ? dev->name : dev->mac, status);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    if (show_codec) {
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, LIST_ROW_LABEL_INSET, 14);
    } else {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    }

    if (show_codec) {
        lv_obj_t * codec_label = lv_label_create(row);
        lv_label_set_text(codec_label, bt_connected_codec_cached);
        lv_obj_add_style(codec_label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(codec_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_align(codec_label, LV_ALIGN_BOTTOM_LEFT, LIST_ROW_LABEL_INSET, -12);
    }

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, bt_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
}

static pthread_t bt_scan_thread;
static bool bt_scan_active = false;
static atomic_bool bt_scan_done_flag = false;

static void * bt_scan_thread_func(void * arg) {
    (void) arg;
    bt_scan_result_count = bt_control_scan(6, bt_scan_results, BT_MAX_RESULTS);
    atomic_store_explicit(&bt_scan_done_flag, true, memory_order_release); /* written last -- poll_bt_scan only checks this flag */
    return NULL;
}

/* Runs fully in the background, same as the stock player -- no busy
 * screen, no navigation at all. See start_wifi_scan()'s own comment for
 * why (same mechanism, same real incident, just the Bluetooth side of it). */
static void start_bt_scan(void) {
    if (bt_scan_active) return;

    atomic_store_explicit(&bt_scan_done_flag, false, memory_order_relaxed);
    bt_scan_active = true;
    set_bt_rescan_active(true);

    if (pthread_create(&bt_scan_thread, NULL, bt_scan_thread_func, NULL) != 0) {
        bt_scan_active = false;
        set_bt_rescan_active(false);
    }
}

void poll_bt_scan(void) {
    if (!bt_scan_active || !atomic_load_explicit(&bt_scan_done_flag, memory_order_acquire)) return;

    bt_scan_active = false;
    pthread_join(bt_scan_thread, NULL);
    set_bt_rescan_active(false);
    populate_bt_screen(); /* refresh the list either way -- worth keeping current even if the user already backed out */
}

/* ---- Bluetooth DAC screen -- lets this device accept incoming A2DP audio
 * (another device streaming TO it, using it as an external DAC) rather than
 * only ever sending audio out. See bt_control_apply_output_settings()'s own
 * doc comment for exactly what toggling this does at the bluealsa/
 * discoverability level -- it's a real running-service restart, not just a
 * flag flip. ---- */

static lv_obj_t * bt_dac_list;

static void bt_dac_enable_row_cb(lv_event_t * e);

/* Explanation + a single "Enable" action, not a toggle -- turning Bluetooth
 * DAC on takes over the whole screen with its own overlay (see
 * build_bt_dac_overlay_screen() below), matching USB DAC mode's own design
 * (build_usb_dac_overlay_screen()): the device can't sensibly be used as a
 * normal music player while it's set up to receive and play audio FROM
 * another device, so that isn't something a background toggle should hide.
 * open_bt_dac_screen() only ever reaches this screen when Bluetooth DAC is
 * currently off -- while on, it goes straight to the overlay instead. */
void populate_bt_dac_screen(void) {
    lv_obj_clean(bt_dac_list);

    lv_obj_t * explanation = lv_label_create(bt_dac_list);
    lv_label_set_text(explanation,
                      "When on, this device stays visible and pairable to other Bluetooth devices, "
                      "so a phone or computer can stream audio TO it and play through this device's "
                      "own output -- using it as an external DAC.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, 24, 0);
    lv_obj_set_style_pad_top(explanation, 12, 0);
    lv_obj_set_style_pad_bottom(explanation, 12, 0);

    lv_obj_t * row = add_pill_row_base(bt_dac_list, "Enable Bluetooth DAC");
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, bt_dac_enable_row_cb, LV_EVENT_CLICKED, NULL);
}

/* Shared by bt_dac_enable_row_cb() and open_bt_dac_screen()'s "already
 * enabled" shortcut -- ALWAYS re-applies the actual bluealsa/bt-agent
 * sink configuration rather than trusting current_settings.bt_dac_mode_enabled
 * as proof the underlying pipeline is really in that state. Real-device
 * incident: the two could desync -- the flag stayed true (and the overlay
 * kept showing on re-entry) while bluealsa had actually drifted back to
 * its plain a2dp-source baseline (observed after live debugging that
 * involved manually restarting bluetoothd/bluealsa outside the app's own
 * control, but the same desync could happen from any crash or external
 * interference) -- leaving the user staring at a "Bluetooth DAC mode"
 * screen that silently wasn't receiving audio at all, with nothing to
 * indicate why. Re-applying unconditionally on every visit, not just the
 * first enable, is cheap (a few subprocess spawns) and makes the overlay
 * a reliable guarantee of the underlying state instead of a one-time
 * side effect that can go stale. */
static void enable_bt_dac_and_show_overlay(void) {
    current_settings.bt_dac_mode_enabled = true;
    settings_save(&current_settings);
    start_bt_apply_output_settings(true, current_settings.bt_volume_sync_enabled);

    /* This device's incoming-audio consumers (aplay -D bluealsa for
     * Bluetooth DAC, shairport -o ot for AirPlay) and this app's own
     * tinyalsa playback all target the same physical ALSA hardware (hw:0,0
     * / "hibysoundcard") -- real-device testing confirmed fighting over it
     * is a genuine problem, not just a theoretical one. So all three are
     * mutually exclusive: enabling Bluetooth DAC stops local playback (see
     * play_track_at_from()/toggle_play_pause()'s own guards) and turns off
     * AirPlay if it was on. */
    if (audio_is_playing()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    if (current_settings.wifi_dac_mode_enabled) {
        current_settings.wifi_dac_mode_enabled = false;
        settings_save(&current_settings);
        airplay_control_stop();
    }

    nav_push(bt_dac_overlay_screen);
}

static void bt_dac_enable_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    enable_bt_dac_and_show_overlay();
}

static lv_obj_t * build_bt_dac_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Bluetooth DAC", &title_label, &bt_dac_list);
}

static void open_bt_dac_screen(void) {
    /* Bluetooth itself has to be on before any of this makes sense -- only
     * actually reachable via the DAC home tile's "Bluetooth DAC" row
     * (build_dac_home_screen()): the Bluetooth settings screen's own
     * "Bluetooth DAC" chevron row only exists while populate_bt_screen()
     * has already gated its whole extra-rows section on Bluetooth being
     * powered (see its own "if (!powered) return;"), so that path can
     * never reach here with Bluetooth off in the first place -- but the DAC
     * home tile has no such gating, so tapping it with Bluetooth off used
     * to head straight into enable_bt_dac_and_show_overlay(), which just
     * spawns bluealsa/bt-agent against a radio that isn't even on. */
    if (!bt_is_powered_cached) {
        show_error_toast("Enable Bluetooth in settings to use BT DAC mode");
        return;
    }
    /* Already on -- re-apply and go straight to the overlay rather than
     * showing the "explanation + Enable" screen for a mode that's already
     * supposed to be active (see enable_bt_dac_and_show_overlay()'s own
     * comment on why re-applying here, not just trusting the flag,
     * matters). */
    if (current_settings.bt_dac_mode_enabled) {
        enable_bt_dac_and_show_overlay();
        return;
    }
    populate_bt_dac_screen();
    nav_push(bt_dac_screen);
}

void bt_dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bt_dac_screen();
}

/* ---- Bluetooth DAC overlay + leave-confirmation popup -- same shape as
 * USB DAC mode's own (build_usb_dac_overlay_screen()/
 * build_usb_dac_leave_popup()): full-screen takeover, no swipe-to-back, the
 * only way out is the back button which asks for confirmation first. ---- */
static lv_obj_t * bt_dac_leave_popup;
static lv_obj_t * bt_dac_leave_popup_backdrop;

static void hide_bt_dac_leave_popup(void) {
    lv_obj_add_flag(bt_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bt_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);
}

static void bt_dac_leave_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
}

static void bt_dac_leave_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
}

static void bt_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
    nav_pop(); /* leave the DAC overlay screen */

    current_settings.bt_dac_mode_enabled = false;
    settings_save(&current_settings);
    start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
}

static void bt_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_remove_flag(bt_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(bt_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bt_dac_leave_popup_backdrop);
    lv_obj_move_foreground(bt_dac_leave_popup);
}

void build_bt_dac_leave_popup(void) {
    bt_dac_leave_popup = build_confirm_popup("Leave Bluetooth DAC mode?", LV_LABEL_LONG_WRAP, NULL, NULL, "Leave",
                                              lv_color_make(255, 120, 120), bt_dac_leave_confirm_cb, NULL, "Cancel",
                                              accent_lv_color(), bt_dac_leave_cancel_cb, NULL,
                                              bt_dac_leave_popup_backdrop_cb, &bt_dac_leave_popup_backdrop);
}

static lv_obj_t * build_bt_dac_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, bt_dac_overlay_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("bt/bt.png"));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t * status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Bluetooth DAC mode");
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

    lv_obj_t * hint_label = lv_label_create(scr);
    lv_label_set_text(hint_label, "This device is now receiving Bluetooth audio");
    lv_obj_add_style(hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(hint_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align_to(hint_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    bt_dac_stream_label = lv_label_create(scr);
    lv_label_set_text(bt_dac_stream_label, "Waiting for Bluetooth stream…");
    lv_obj_add_style(bt_dac_stream_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(bt_dac_stream_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(bt_dac_stream_label, LV_PCT(90));
    lv_obj_set_style_text_align(bt_dac_stream_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(bt_dac_stream_label, hint_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    return scr;
}

/* ---- Codec selection screen -- single-select list, current choice shown
 * via an accent-colored border rather than a checkmark glyph (same
 * indicator build_accent_color_screen's swatches use; safer than a unicode
 * checkmark that this project's subset LVGL fonts may not actually have a
 * glyph for). Writes straight into /usr/data/alsa.conf via
 * bt_control_set_codec() -- see its own doc comment for the LDAC_HQ/
 * LDAC_SQ caveat. ---- */

typedef struct {
    const char * value; /* matches player_settings_t.bt_codec / bt_control_set_codec()'s argument */
    const char * label;
} bt_codec_option_t;

static const bt_codec_option_t bt_codec_options[] = {
    { "auto", "Auto" },      { "ldac_hq", "LDAC Quality" }, { "ldac_sq", "LDAC Standard" },
    { "aptx", "aptX" },      { "aac", "AAC" },              { "sbc", "SBC" },
};
#define BT_CODEC_OPTION_COUNT (sizeof(bt_codec_options) / sizeof(bt_codec_options[0]))

static lv_obj_t * bt_codec_list;

static void bt_codec_option_row_cb(lv_event_t * e);

static void populate_bt_codec_screen(void) {
    lv_obj_clean(bt_codec_list);
    for (size_t i = 0; i < BT_CODEC_OPTION_COUNT; i++) {
        bool selected = strcmp(current_settings.bt_codec, bt_codec_options[i].value) == 0;
        lv_obj_t * row = add_pill_row_base(bt_codec_list, bt_codec_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, bt_codec_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void bt_codec_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    snprintf(current_settings.bt_codec, sizeof(current_settings.bt_codec), "%s", bt_codec_options[index].value);
    settings_save(&current_settings);
    bt_control_set_codec(current_settings.bt_codec);
    populate_bt_codec_screen();
}

static lv_obj_t * build_bt_codec_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Codec", &title_label, &bt_codec_list);
}

static void open_bt_codec_screen(void) {
    populate_bt_codec_screen();
    nav_push(bt_codec_screen);
}

static void bt_codec_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bt_codec_screen();
}

/* ---- Resume Last Track selection screen (Settings > Playback > Resume Last
 * Track) -- same accent-colored-border single-select shape as Font Size
 * just below, but takes effect on the NEXT cold boot (there's nothing to
 * apply live -- this only ever matters at startup), so no reboot popup is
 * needed, just a toast confirming the choice. See gui_init()'s own resume
 * path for what each option actually does and the Subsonic-cache guard it
 * shares with Car Mode's separate, always-on resume mechanism. ---- */

typedef struct {
    int mode; /* matches player_settings_t.resume_mode */
    const char * label;
} resume_mode_option_t;

static const resume_mode_option_t resume_mode_options[] = {
    { 0, "Off" }, { 1, "Resume and Play" }, { 2, "Resume, but Paused" },
};
#define RESUME_MODE_OPTION_COUNT (sizeof(resume_mode_options) / sizeof(resume_mode_options[0]))

static void resume_mode_option_row_cb(lv_event_t * e);

static void populate_resume_mode_screen(void) {
    lv_obj_clean(resume_mode_list);
    for (size_t i = 0; i < RESUME_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.resume_mode == resume_mode_options[i].mode;
        lv_obj_t * row = add_pill_row_base(resume_mode_list, resume_mode_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, resume_mode_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void resume_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.resume_mode = resume_mode_options[index].mode;
    settings_save(&current_settings);
    populate_resume_mode_screen();
    show_info_toast("Applies next time you launch the app");
}

static lv_obj_t * build_resume_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Resume Last Track", &title_label, &resume_mode_list);
}

static void open_resume_mode_screen(void) {
    populate_resume_mode_screen();
    nav_push(resume_mode_screen);
}

void resume_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_resume_mode_screen();
}

/* ---- Play/Pause button behavior selection screen (Settings > Playback >
 * Play/Pause Button) -- same accent-colored-border single-select shape as
 * Resume Last Track just above. Feature request: the physical play/pause
 * button can act as Previous Track instead, or combine both via a short
 * double-click window -- see handle_physical_play_pause_press() near
 * update_timer_cb for the actual dispatch. Unlike Resume Last Track, this
 * takes effect on the very next physical button press, not just at next
 * boot. ---- */

typedef struct {
    int mode; /* matches player_settings_t.play_pause_button_mode */
    const char * label;
} play_pause_button_mode_option_t;

static const play_pause_button_mode_option_t play_pause_button_mode_options[] = {
    { 0, "Play/Pause" }, { 1, "Previous Track" }, { 2, "Play/Pause + Previous Track (Double-Click)" },
};
#define PLAY_PAUSE_BUTTON_MODE_OPTION_COUNT (sizeof(play_pause_button_mode_options) / sizeof(play_pause_button_mode_options[0]))

static lv_obj_t * play_pause_button_mode_list;

static void play_pause_button_mode_option_row_cb(lv_event_t * e);

static void populate_play_pause_button_mode_screen(void) {
    lv_obj_clean(play_pause_button_mode_list);
    for (size_t i = 0; i < PLAY_PAUSE_BUTTON_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.play_pause_button_mode == play_pause_button_mode_options[i].mode;
        lv_obj_t * row = add_pill_row_base(play_pause_button_mode_list, play_pause_button_mode_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, play_pause_button_mode_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void play_pause_button_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.play_pause_button_mode = play_pause_button_mode_options[index].mode;
    settings_save(&current_settings);
    populate_play_pause_button_mode_screen();
    show_info_toast("Applies immediately");
}

static lv_obj_t * build_play_pause_button_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Play/Pause Button", &title_label, &play_pause_button_mode_list);
}

static void open_play_pause_button_mode_screen(void) {
    populate_play_pause_button_mode_screen();
    nav_push(play_pause_button_mode_screen);
}

void play_pause_button_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_play_pause_button_mode_screen();
}

/* ---- Font size selection screen (Settings > Font Size) -- same
 * accent-colored-border single-select shape as the Bluetooth codec screen
 * just above, but selecting a row only saves the setting and shows a
 * toast rather than applying anything live -- see gui.c's own
 * apply_font_size_tier() and fallback_font.h's fallback_font_init_early()
 * for why an already-built screen can't be restyled in place, and needs
 * an app restart to pick up the new tier instead. ---- */

typedef struct {
    int tier; /* matches player_settings_t.font_size_tier */
    const char * label;
} font_size_option_t;

static const font_size_option_t font_size_options[] = {
    { 0, "Small" }, { 1, "Medium" }, { 2, "BlindMF" },
};
#define FONT_SIZE_OPTION_COUNT (sizeof(font_size_options) / sizeof(font_size_options[0]))

static lv_obj_t * font_size_list;

static void font_size_option_row_cb(lv_event_t * e);

static void populate_font_size_screen(void) {
    lv_obj_clean(font_size_list);
    for (size_t i = 0; i < FONT_SIZE_OPTION_COUNT; i++) {
        bool selected = current_settings.font_size_tier == font_size_options[i].tier;
        lv_obj_t * row = add_pill_row_base(font_size_list, font_size_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, font_size_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

/* Defined later, alongside build_eq_reset_popup()/build_factory_reset_popup()
 * (same hand-built top-layer confirmation-popup shape) -- forward-declared
 * here since font_size_option_row_cb() needs it well before that point in
 * the file. */

/* show_font_size_reboot_popup is in gui_settings.c */



static void font_size_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.font_size_tier = font_size_options[index].tier;
    settings_save(&current_settings);
    populate_font_size_screen();
    show_font_size_reboot_popup();
}

static lv_obj_t * build_font_size_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Font Size", &title_label, &font_size_list);
}

static void open_font_size_screen(void) {
    populate_font_size_screen();
    nav_push(font_size_screen);
}

void font_size_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_font_size_screen();
}

/* ---- Lyrics Text Size (Settings -> Display) -- real-device request: the
 * fullscreen lyrics view needed its own separate size control, decoupled
 * from the rest of the app's own Font Size above (see settings.h's own
 * lyrics_font_size_tier comment and fallback_font.h's app_font_lyrics for
 * why). Same single-select, accent-border, reboot-to-apply shape as Font
 * Size itself, just 2 options (Medium/Large) instead of 3. ---- */

typedef struct {
    int tier; /* matches player_settings_t.lyrics_font_size_tier -- 1 or 2 only */
    const char * label;
} lyrics_font_size_option_t;









/* ---- ReplayGain mode (Settings -> Playback) -- same single-select,
 * accent-border shape as Lyrics Text Size/Font Size above, but live-apply
 * rather than reboot-to-apply: the setting itself is just which of a
 * track's own already-parsed gain fields resolve_replaygain() picks, no
 * font atlas or anything else expensive to reload, so the change can take
 * effect as soon as the next track starts (same "avoid a mid-song loudness
 * jump" reasoning the old on/off toggle already had -- only the already-
 * queued next track and subsequent starts adopt the new preference, not
 * the one playing right now). Replaces that old plain toggle with a real
 * 3-way choice. ---- */

typedef struct {
    int mode; /* matches player_settings_t.replaygain_mode */
    const char * label;
} replaygain_mode_option_t;

static const replaygain_mode_option_t replaygain_mode_options[] = {
    { 0, "Off" }, { 1, "Per Track" }, { 2, "Per Album" },
};
#define REPLAYGAIN_MODE_OPTION_COUNT (sizeof(replaygain_mode_options) / sizeof(replaygain_mode_options[0]))

static lv_obj_t * replaygain_mode_list;

static void replaygain_mode_option_row_cb(lv_event_t * e);

static void populate_replaygain_mode_screen(void) {
    lv_obj_clean(replaygain_mode_list);
    for (size_t i = 0; i < REPLAYGAIN_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.replaygain_mode == replaygain_mode_options[i].mode;
        lv_obj_t * row = add_pill_row_base(replaygain_mode_list, replaygain_mode_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, replaygain_mode_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void replaygain_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.replaygain_mode = replaygain_mode_options[index].mode;
    settings_save(&current_settings);
    populate_replaygain_mode_screen();
    if (gui_player_has_active_track()) arm_next_track_for_audio(gui_player_get_playlist_index());
}

static lv_obj_t * build_replaygain_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("ReplayGain", &title_label, &replaygain_mode_list);
}

static void open_replaygain_mode_screen(void) {
    populate_replaygain_mode_screen();
    nav_push(replaygain_mode_screen);
}

void replaygain_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_replaygain_mode_screen();
}

/* ---- USB device mode (Settings > USB Mode) -- single-select list, same
 * shape as the Bluetooth codec screen just above (accent-colored border on
 * the current choice). Applying a mode is real configfs/sysfs work (see
 * usb_mode_control.c), so it runs on its own thread and gets polled from
 * update_timer_cb like every other real-device toggle in this file, rather
 * than blocking the UI thread the way bt_codec_option_row_cb's direct call
 * does. ---- */

typedef struct {
    usb_mode_t mode;
    const char * label;
} usb_mode_option_t;

static const usb_mode_option_t usb_mode_options[] = {
    { USB_MODE_STORAGE, "Storage" },
    { USB_MODE_DAC, "USB DAC" },
    { USB_MODE_ADB, "ADB" },
};
#define USB_MODE_OPTION_COUNT (sizeof(usb_mode_options) / sizeof(usb_mode_options[0]))

static lv_obj_t * usb_mode_list;

static pthread_t usb_mode_switch_thread;
static bool usb_mode_switch_active = false;
static atomic_bool usb_mode_switch_done_flag = false;
static volatile bool usb_mode_switch_succeeded = false;
static usb_mode_t usb_mode_switch_target;
static bool usb_cable_state_initialized;
static bool usb_cable_was_connected;
static bool usb_storage_rebind_pending;

static void usb_mode_option_row_cb(lv_event_t * e);
static void usb_mode_adb_toggle_cb(lv_event_t * e);

/* ADB is a separate toggle, not a third equal option alongside Storage/DAC
 * -- matches the stock firmware's own USB device mode screen (confirmed by
 * the user against a real stock device: ADB overrides Storage/DAC while
 * on, turning it off falls back to the other two). This also matches the
 * actual gadget architecture: ADB lives under its own "adb_demo" configfs
 * directory, entirely separate from the "android0" one Storage and DAC
 * share (see usb_mode_control.c), so it really is orthogonal rather than a
 * third position in the same rotation. */
static void populate_usb_mode_screen(void) {
    lv_obj_clean(usb_mode_list);

    bool adb_active = current_settings.usb_mode == (int) USB_MODE_ADB;
    add_pill_toggle_row(usb_mode_list, "ADB", adb_active, usb_mode_adb_toggle_cb);

    for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
        if (usb_mode_options[i].mode == USB_MODE_ADB) continue; /* handled by the toggle above */

        bool selected = !adb_active && current_settings.usb_mode == (int) usb_mode_options[i].mode;
        lv_obj_t * row = add_pill_row_base(usb_mode_list, usb_mode_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);

        if (adb_active) {
            /* Storage/DAC aren't meaningful choices while ADB owns the USB
             * port -- dimmed and inert rather than removed, so the row
             * stays put (no layout jump) and it's visually clear these
             * exist but aren't the current mode. */
            lv_obj_set_style_opa(row, LV_OPA_50, 0);
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, usb_mode_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        }
    }
}

static void * usb_mode_switch_thread_func(void * arg) {
    (void) arg;
    usb_mode_switch_succeeded = usb_mode_control_apply(usb_mode_switch_target);
    atomic_store_explicit(&usb_mode_switch_done_flag, true, memory_order_release); /* written last -- poll_usb_mode_switch only checks this flag */
    return NULL;
}

void start_usb_mode_switch(usb_mode_t target) {
    if (usb_mode_switch_active) return; /* already switching -- ignore taps until it lands, same guard as wifi/bt toggles */
    usb_mode_switch_target = target;
    usb_mode_switch_active = true;
    atomic_store_explicit(&usb_mode_switch_done_flag, false, memory_order_relaxed);
    int rc = pthread_create(&usb_mode_switch_thread, NULL, usb_mode_switch_thread_func, NULL);
    if (rc != 0) {
        /* Without this rollback, a transient low-memory/thread-creation
         * failure leaves the guard latched forever: every later tap is
         * ignored until the player or device is restarted. */
        usb_mode_switch_active = false;
        show_error_toast("Could not start USB mode switch");
    }
}

void poll_usb_mode_switch(void) {
    if (!usb_mode_switch_active || !atomic_load_explicit(&usb_mode_switch_done_flag, memory_order_acquire)) return;
    usb_mode_switch_active = false;
    pthread_join(usb_mode_switch_thread, NULL);

    /* usb_mode_control_apply() only reports success once the target
     * gadget is actually verified bound, not just "the script ran" -- on
     * failure current_settings.usb_mode is deliberately left untouched
     * (the switch genuinely didn't happen) rather than optimistically
     * recording what was merely requested. */
    if (!usb_mode_switch_succeeded) {
        const char * label = "USB mode";
        for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
            if (usb_mode_options[i].mode == usb_mode_switch_target) { label = usb_mode_options[i].label; break; }
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Failed to switch to %s", label);
        show_error_toast(msg);
        populate_usb_mode_screen(); /* in case current_settings.usb_mode's actual row needs re-highlighting */
        return;
    }

    current_settings.usb_mode = (int) usb_mode_switch_target;
    settings_save(&current_settings);
    populate_usb_mode_screen(); /* refresh which row shows the accent border */

    /* DAC mode takes over the whole screen with its own overlay (matching
     * the stock firmware's hiby_usb.view) -- the device can't sensibly be
     * used as a normal music player while it's presenting itself to a PC
     * as a USB sound card. Storage/ADB have no such conflict (the app
     * keeps running normally), so they don't get an overlay. */
    if (usb_mode_switch_target == USB_MODE_DAC) {
        /* Same three-way mutual exclusion as bt_dac_toggle_cb()/
         * airplay_toggle_cb() -- all three (local playback, Bluetooth DAC,
         * AirPlay) fight over the same hw:0,0 output. usb_dac_bridge_start()
         * (called from usb_mode_control_apply(), already run on the
         * background thread by this point) already called audio_stop()
         * itself, but that only stops the backend -- the play button icon
         * needs its own explicit refresh, same as every other call site
         * that stops playback out from under the UI. */
        if (audio_is_playing() || audio_is_paused()) {
            audio_stop();
            set_play_button_state(false);
            plugin_manager_notify_stopped();
        }
        if (current_settings.bt_dac_mode_enabled) {
            current_settings.bt_dac_mode_enabled = false;
            settings_save(&current_settings);
            start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        }
        if (current_settings.wifi_dac_mode_enabled) {
            current_settings.wifi_dac_mode_enabled = false;
            settings_save(&current_settings);
            airplay_control_stop();
        }
        nav_push(usb_dac_overlay_screen);
    }
}

/* A fresh boot intentionally has no gadget bound, but previously nothing
 * reacted when a PC was plugged in: Storage only appeared after manually
 * switching to DAC/ADB and back, because those taps happened to perform the
 * missing UDC bind. Reapply Storage on every physical connection edge so a
 * PC enumerates it immediately. Never override an active DAC/ADB session. */
void poll_usb_storage_hotplug(void) {
    bool connected = usb_mode_control_cable_connected();
    if (!usb_cable_state_initialized) {
        usb_cable_state_initialized = true;
        usb_cable_was_connected = connected;
        /* Storage is the default gadget when a PC is plugged in, including
         * a cable already present at boot. ADB and DAC stay opt-in from
         * Settings; usb_mode_control_apply() tears Storage down first. */
        usb_storage_rebind_pending = connected;
    } else if (connected && !usb_cable_was_connected) {
        usb_storage_rebind_pending = true;
    }
    usb_cable_was_connected = connected;

    if (!connected || !usb_storage_rebind_pending || usb_mode_switch_active) return;

    usb_mode_t live_mode;
    bool have_live_mode = usb_mode_control_detect_current(&live_mode);
    if (have_live_mode && (live_mode == USB_MODE_DAC || live_mode == USB_MODE_ADB)) {
        usb_storage_rebind_pending = false;
        return;
    }

    usb_storage_rebind_pending = false;
    start_usb_mode_switch(USB_MODE_STORAGE);
}

static void usb_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    usb_mode_t target = usb_mode_options[index].mode;
    if (current_settings.usb_mode == (int) target) return;
    start_usb_mode_switch(target);
}

static void usb_mode_adb_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool currently_adb = current_settings.usb_mode == (int) USB_MODE_ADB;
    /* Turning ADB off falls back to Storage -- the stock default, and the
     * safer of the other two since DAC takes over the whole screen with
     * its own overlay rather than just quietly changing what the USB port
     * does. */
    start_usb_mode_switch(currently_adb ? USB_MODE_STORAGE : USB_MODE_ADB);
}

static lv_obj_t * build_usb_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("USB Mode", &title_label, &usb_mode_list);
}

static void open_usb_mode_screen(void) {
    /* current_settings.usb_mode is only a UI hint (see settings.h's own
     * comment: never re-applied to hardware on startup, so it can't be
     * trusted to reflect reality after a reboot or any change made outside
     * this app) -- correct it against live gadget state whenever that
     * state is unambiguous, rather than showing a stale/wrong row
     * highlighted, which is exactly what was happening before (always
     * defaulted to showing Storage selected regardless of what was
     * actually connected). Just sysfs reads, fast enough for the UI
     * thread. */
    usb_mode_t detected;
    if (usb_mode_control_detect_current(&detected) && current_settings.usb_mode != (int) detected) {
        current_settings.usb_mode = (int) detected;
        settings_save(&current_settings);
    }
    populate_usb_mode_screen();
    nav_push(usb_mode_screen);
}

void usb_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_usb_mode_screen();
}

/* ---- USB DAC mode overlay + leave-confirmation popup --------------------
 * Full-screen takeover matching the stock firmware's hiby_usb.view (a
 * centered USB icon + a top-left back button, nothing else) -- deliberately
 * skips finalize_screen_navigation()'s swipe-to-back/swipe-to-player-screen
 * wiring, same reasoning as build_import_wifi_screen()'s own comment: the
 * only way out is the back button, which asks for confirmation rather than
 * leaving immediately, and an accidental swipe bypassing that would defeat
 * the point. The confirmation popup itself reuses bt_action_popup's own
 * hand-built top-layer overlay shape (this codebase doesn't use LVGL's
 * lv_msgbox anywhere). ---- */
static lv_obj_t * usb_dac_leave_popup;
static lv_obj_t * usb_dac_leave_popup_backdrop;

static void hide_usb_dac_leave_popup(void) {
    lv_obj_add_flag(usb_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usb_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);
}

static void usb_dac_leave_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup(); /* tap-outside = cancel, matches bt_action_popup precedent */
}

static void usb_dac_leave_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup();
}

static void usb_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup();
    nav_pop(); /* leave the DAC overlay screen */
    start_usb_mode_switch(USB_MODE_STORAGE); /* "switch back to Storage (Default)" */
}

static void usb_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_remove_flag(usb_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(usb_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(usb_dac_leave_popup_backdrop);
    lv_obj_move_foreground(usb_dac_leave_popup);
}

void build_usb_dac_leave_popup(void) {
    usb_dac_leave_popup = build_confirm_popup("Leave USB DAC mode?", LV_LABEL_LONG_WRAP, NULL, NULL, "Leave",
                                               lv_color_make(255, 120, 120), usb_dac_leave_confirm_cb, NULL, "Cancel",
                                               accent_lv_color(), usb_dac_leave_cancel_cb, NULL,
                                               usb_dac_leave_popup_backdrop_cb, &usb_dac_leave_popup_backdrop);
}

static lv_obj_t * build_usb_dac_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, usb_dac_overlay_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("usb/usb.png"));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t * status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "USB DAC mode");
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

    usb_dac_hint_label = lv_label_create(scr);
    lv_label_set_text(usb_dac_hint_label, "Waiting for USB audio…");
    lv_obj_add_style(usb_dac_hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_hint_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align_to(usb_dac_hint_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    usb_dac_input_label = lv_label_create(scr);
    lv_obj_add_style(usb_dac_input_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_input_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align_to(usb_dac_input_label, usb_dac_hint_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    usb_dac_path_label = lv_label_create(scr);
    lv_obj_add_style(usb_dac_path_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_path_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align_to(usb_dac_path_label, usb_dac_input_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    return scr;
}

static void set_label_if_changed(lv_obj_t * label, const char * text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void format_rate(char * out, size_t size, unsigned int rate) {
    if (rate && rate % 1000 == 0) snprintf(out, size, "%u kHz", rate / 1000);
    else if (rate) snprintf(out, size, "%.1f kHz", (double) rate / 1000.0);
    else snprintf(out, size, "Unknown rate");
}

static void dac_stream_labels_timer_cb(lv_timer_t * timer) {
    (void) timer;
    bt_dac_stream_info_t bt;
    bt_control_get_dac_stream_info(&bt);
    char text[160];
    if (!bt.available || !bt.running) {
        snprintf(text, sizeof(text), "Waiting for Bluetooth stream…");
    } else {
        char rate[32];
        format_rate(rate, sizeof(rate), bt.sample_rate);
        const char * format = bt.bit_depth ? NULL : (bt.pcm_format[0] ? bt.pcm_format : "Unknown format");
        if (bt.bit_depth)
            snprintf(text, sizeof(text), "%s · %s · %u-bit", bt.codec[0] ? bt.codec : "Unknown codec", rate, bt.bit_depth);
        else
            snprintf(text, sizeof(text), "%s · %s · %s", bt.codec[0] ? bt.codec : "Unknown codec", rate, format);
    }
    set_label_if_changed(bt_dac_stream_label, text);

    usb_dac_stream_info_t usb;
    usb_dac_bridge_get_stream_info(&usb);
    set_label_if_changed(usb_dac_hint_label, usb.streaming ? "This device is now a USB sound card" : "Waiting for USB audio…");
    char input_rate[32], output_rate[32];
    format_rate(input_rate, sizeof(input_rate), usb.input_sample_rate);
    format_rate(output_rate, sizeof(output_rate), usb.output_sample_rate);
    snprintf(text, sizeof(text), "USB input: %s · %u-bit", input_rate, usb.input_bit_depth);
    set_label_if_changed(usb_dac_input_label, text);
    snprintf(text, sizeof(text), "DAC path: %s · %u-bit", output_rate, usb.output_bit_depth);
    set_label_if_changed(usb_dac_path_label, text);
}

static void bt_volume_sync_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_volume_sync_enabled = !current_settings.bt_volume_sync_enabled;
    settings_save(&current_settings);
    start_bt_apply_output_settings(current_settings.bt_dac_mode_enabled, current_settings.bt_volume_sync_enabled);
    populate_bt_screen();
}

/* Real-device feedback: "bluetooth screen is showing a lot of devices
 * without a name" -- nearby BLE beacons/accessories that never broadcast
 * one show up as raw MAC addresses (add_bt_device_row()'s own fallback)
 * cluttering the Available Devices list. Purely a display filter, unlike
 * the volume-sync toggle above -- no bluealsa/bluetoothctl state to push,
 * just a settings flag and a re-render. Paired devices are never filtered
 * by this (see populate_bt_screen()'s own loop) -- you wouldn't have
 * paired with one you couldn't identify in the first place. */
static void bt_hide_unnamed_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_hide_unnamed_devices = !current_settings.bt_hide_unnamed_devices;
    settings_save(&current_settings);
    populate_bt_screen();
}

/* ---- Master rebuild for the Bluetooth screen's one dynamic list: toggle
 * row, then (only while powered) volume sync/DAC/Codec rows, Paired
 * Devices, and Available Devices -- same full-rebuild-on-any-change
 * convention as populate_wifi_screen() above. bt_scan_results already holds
 * both paired and freshly-discovered devices from one bt_control_scan()
 * call; this just splits that single list into the two sections by
 * ->paired rather than needing two separate scans. ---- */
void populate_bt_screen(void) {
    /* Real-device bug: this screen rebuilds from scratch on every ~5s
     * connection poll tick (see poll_refresh_bt_icon()'s own call site),
     * and lv_obj_clean() below resets the list's scroll position to the
     * top along with destroying its children -- confirmed live: with
     * enough devices to need scrolling, the periodic rebuild snapped the
     * view back to the top every cycle, making it impossible to actually
     * read anything past the first screenful. Captured before the clean
     * and restored after rebuilding (both exit paths -- the early return
     * just below when Bluetooth is off has nothing to restore into, but
     * costs nothing to always capture/restore symmetrically). */
    int32_t saved_scroll_y = lv_obj_get_scroll_y(bt_list);

    lv_obj_clean(bt_list);

    /* bt_is_powered_cached, not bt_control_is_powered() -- same reasoning
     * as external_dac_block_reason()'s own use of the cache (see its
     * comment): calling bt_control_is_powered() directly here forked
     * bluetoothctl show synchronously on the UI thread every time this
     * screen opened, which on a real device produced a multi-second freeze
     * entering the Bluetooth submenu (a milder version of the same
     * pre-existing hang class start_refresh_bt_icon()'s own comment
     * documents). The cache is kept fresh in the background regardless. */
    bool powered = bt_is_powered_cached;
    if (bt_rescan_btn) {
        if (powered) lv_obj_remove_flag(bt_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }
    /* quick_drawer_bt_event_cb is the SAME real toggle-thread trigger the
     * drawer's own bt icon uses -- see populate_wifi_screen()'s identical
     * reasoning for wifi. */
    add_pill_toggle_row(bt_list, "Bluetooth", powered, quick_drawer_bt_event_cb);

    if (!powered) {
        lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
        return;
    }

    add_pill_toggle_row(bt_list, "Bluetooth Volume Sync", current_settings.bt_volume_sync_enabled,
                        bt_volume_sync_toggle_cb);
    add_pill_chevron_row(bt_list, "Bluetooth DAC", bt_dac_settings_row_cb);
    add_pill_chevron_row(bt_list, "Codec", bt_codec_settings_row_cb);
    add_pill_toggle_row(bt_list, "Hide Unnamed Devices", current_settings.bt_hide_unnamed_devices,
                        bt_hide_unnamed_toggle_cb);

    add_section_header(bt_list, "Paired Devices");
    int paired_shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (!bt_scan_results[i].paired) continue;
        add_bt_device_row(bt_list, i);
        paired_shown++;
    }
    if (paired_shown == 0) {
        lv_obj_t * label = lv_label_create(bt_list);
        lv_label_set_text(label, "No paired devices");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }

    add_section_header(bt_list, "Available Devices");
    int available_shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (bt_scan_results[i].paired) continue;
        if (current_settings.bt_hide_unnamed_devices && bt_name_is_mac_placeholder(&bt_scan_results[i])) continue;
        add_bt_device_row(bt_list, i);
        available_shown++;
    }
    if (available_shown == 0) {
        lv_obj_t * label = lv_label_create(bt_list);
        lv_label_set_text(label, "No devices found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }

    lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
}

static lv_obj_t * build_bluetooth_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    lv_obj_t * scr = build_subsonic_list_screen("Bluetooth", &title_label, &bt_list);

    bt_rescan_btn = lv_label_create(scr);
    lv_label_set_text(bt_rescan_btn, "Rescan");
    lv_obj_set_style_text_color(bt_rescan_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(bt_rescan_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(bt_rescan_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bt_rescan_btn, bt_rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

void open_bluetooth_screen(void) {
    populate_bt_screen();
    nav_push(bt_screen);
    if (bt_is_powered_cached) start_bt_scan(); /* auto-refresh -- matches the old always-scan-on-open behavior, but only when there's a radio to scan with -- cached, not a fresh bt_control_is_powered() call, same reasoning as populate_bt_screen() */
}

static void bt_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bluetooth_screen();
}

/* ---- Import via Wi-Fi screen -- this is just the on-device "here's the
 * address to open" front end; the actual file-manager webapp is the stock
 * firmware's own thttpd + CGI binaries, managed via import_web.h. ---- */

static lv_obj_t * import_wifi_status_label;
static lv_obj_t * import_wifi_url_label;
#if LV_USE_QRCODE
static lv_obj_t * import_wifi_qrcode;
#endif

static void populate_import_wifi_screen(void) {
    wifi_info_t info;
    if (!wifi_control_get_info(&info) || info.ip[0] == '\0') {
        lv_label_set_text(import_wifi_status_label, "Connect to Wi-Fi first");
        lv_label_set_text(import_wifi_url_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(import_wifi_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        return;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://%s:4399", info.ip);
    lv_label_set_text(import_wifi_status_label, "Open this address on your phone or computer:");
    lv_label_set_text(import_wifi_url_label, url);
#if LV_USE_QRCODE
    lv_obj_remove_flag(import_wifi_qrcode, LV_OBJ_FLAG_HIDDEN);
    lv_qrcode_update(import_wifi_qrcode, url, strlen(url));
#endif
}

/* ---- "Update music database?" confirmation, shown on leaving Import via
 * Wi-Fi -- mirrors build_delete_song_popup()'s own centered-card/two-row
 * shape. Real-device feedback: a full rescan used to run unconditionally on
 * every exit from this screen, even if the user never actually uploaded
 * anything (just looked at the QR code, or backed out immediately) --
 * wasteful on a real library, and gave no way to skip it. Leaving the screen
 * and tearing down the HTTP server (import_web_stop(), now backgrounded --
 * see poll_import_web_stop()'s own comment) both still happen
 * unconditionally regardless of the choice made here; only the rescan
 * itself is behind it. This popup is shown once that teardown finishes,
 * not right when back is tapped. */
static lv_obj_t * import_rescan_popup;
static lv_obj_t * import_rescan_popup_backdrop;

static void hide_import_rescan_popup(void) {
    lv_obj_add_flag(import_rescan_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(import_rescan_popup, LV_OBJ_FLAG_HIDDEN);
}

static void import_rescan_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_import_rescan_popup();
}

static void import_rescan_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_import_rescan_popup();
}

static void import_rescan_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_import_rescan_popup();
    start_library_rescan();
}

/* Shared "are you sure?" 2-button confirmation popup shape -- backdrop +
 * centered card + wrapped title + two full-width buttons. Real-device bug
 * report: this exact shape was independently hand-duplicated (fixed 400-
 * 420px-wide, ~220px-tall popup, title/buttons at fixed pixel y-offsets)
 * across roughly 15 different popups in this file, all tuned for the
 * small/medium font tiers -- BlindMF's much bigger text broke each one
 * the same way (title/button text overflowing those fixed offsets), first
 * caught here on the EQ reset and "Update music database?" popups.
 * LV_SIZE_CONTENT + flex column throughout, same structural fix already
 * proven on the PEQ screen's own cards, so the popup's height (and the
 * gap between its title and buttons) follows however tall the rendered
 * text actually is at whatever tier is active, instead of a number that
 * only happened to be enough for the original tier. Returns the popup
 * object and writes the backdrop to *out_backdrop; the caller wires up
 * its own show/hide functions and owns both objects same as before --
 * this only replaces how each popup's insides get built, not the
 * hide/show/backdrop-tap machinery already established per popup. */
lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode,
                                       lv_obj_t ** out_title, const char * body_text, const char * confirm_text,
                                       lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row,
                                       const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                                       lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop) {
    lv_obj_t * top = lv_layer_top();

    lv_obj_t * backdrop = lv_obj_create(top);
    lv_obj_set_size(backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * popup = lv_obj_create(top);
    lv_obj_set_width(popup, lv_pct(84));
    lv_obj_set_height(popup, LV_SIZE_CONTENT);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(popup, 16, 0);
    lv_obj_add_style(popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 0, 0);
    lv_obj_set_style_pad_all(popup, 20, 0);
    lv_obj_set_style_pad_row(popup, 14, 0);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * title = lv_label_create(popup);
    lv_label_set_text(title, title_text);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, title_long_mode);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    if (out_title) *out_title = title;

    if (body_text) {
        lv_obj_t * body = lv_label_create(popup);
        lv_label_set_text(body, body_text);
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_style(body, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    }

    lv_obj_t * confirm_row = lv_obj_create(popup);
    lv_obj_set_width(confirm_row, lv_pct(100));
    lv_obj_set_height(confirm_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(confirm_row, 14, 0);
    lv_obj_set_style_radius(confirm_row, 12, 0);
    lv_obj_set_style_bg_opa(confirm_row, 0, 0);
    lv_obj_set_style_border_width(confirm_row, 0, 0);
    lv_obj_remove_flag(confirm_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(confirm_row, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * confirm_label = lv_label_create(confirm_row);
    lv_label_set_text(confirm_label, confirm_text);
    lv_obj_set_style_text_color(confirm_label, confirm_color, 0);
    lv_obj_set_style_text_font(confirm_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(confirm_label);
    if (out_confirm_row) *out_confirm_row = confirm_row;

    lv_obj_t * cancel_row = lv_obj_create(popup);
    lv_obj_set_width(cancel_row, lv_pct(100));
    lv_obj_set_height(cancel_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(cancel_row, 14, 0);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, cancel_text);
    lv_obj_set_style_text_color(cancel_label, cancel_color, 0);
    lv_obj_set_style_text_font(cancel_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(cancel_label);
    if (out_cancel_row) *out_cancel_row = cancel_row;

    *out_backdrop = backdrop;
    return popup;
}

/* Shared N-row action-menu popup shape -- backdrop + centered card + a
 * column of tappable text rows (e.g. "List"/"Queue"/"Add to Playlist"/
 * "EQ"/"Delete"). Same real-device bug and same structural fix as
 * build_confirm_popup() just above (LV_SIZE_CONTENT/flex column instead of
 * a fixed-pixel box with rows at fixed y-offsets, tuned only for the
 * small/medium font tiers) -- this is the OTHER hand-duplicated popup
 * shape in this file, used by menus rather than yes/no confirmations.
 * Returns the popup object and writes the backdrop to *out_backdrop, same
 * ownership split as build_confirm_popup(). */
/* build_menu_popup is in gui.c */


static void build_import_rescan_popup(void) {
    /* lv_color_make(160,160,160) matches style_theme_text_muted's own
     * default (screen_builders.c) verbatim -- a raw color snapshot here
     * rather than lv_obj_add_style(&style_theme_text_muted), same
     * build-time-snapshot tradeoff the accent-colored "Update" button
     * right next to it already had before this popup was ever migrated to
     * build_confirm_popup() (accent_lv_color() below is also just read
     * once, at build time, not a live-tracking style). */
    import_rescan_popup = build_confirm_popup("Update music database?", LV_LABEL_LONG_WRAP, NULL, NULL, "Update",
                                               accent_lv_color(), import_rescan_confirm_cb, NULL, "Not now",
                                               lv_color_make(160, 160, 160), import_rescan_cancel_cb, NULL,
                                               import_rescan_popup_backdrop_cb, &import_rescan_popup_backdrop);
}

/* Real-device feedback: leaving Import via Wi-Fi used to just hang -- no
 * visible feedback at all -- for however long import_web_stop() took (its
 * own comment: killall -9 across up to 8 process names, plus a bounded
 * pgrep-polled wait for all of them to actually be reaped, up to ~500ms of
 * genuine subprocess churn) before the screen finally changed, since that
 * call used to run right on this UI-thread click handler. Same background-
 * thread-plus-polled-done-flag shape as every other multi-hundred-ms
 * operation in this file (start_wifi_connect()/poll_wifi_connect() is the
 * closest match: same "push the shared busy screen, no numeric progress to
 * show" shape), reusing subsonic_downloading_screen/_label rather than
 * building new UI just for this one line of text. */
static pthread_t import_web_stop_thread;
static bool import_web_stop_active = false;
static atomic_bool import_web_stop_done_flag = false;
/* Import via Wi-Fi screen's own stack slot, recorded right before the busy
 * screen gets pushed on top of it -- spliced out via nav_remove_stack_slot()
 * once teardown finishes (same pattern text_entry_commit()/
 * poll_subsonic_download() already use), so backing out later lands on
 * whatever was open before Import via Wi-Fi, not on this now-defunct
 * "Closing Web Server..." screen or a stale extra copy of the screen
 * underneath it. */
static int import_web_stop_nav_slot = -1;

/* True from a successful open_import_wifi_screen() until teardown (either
 * the back button or gui_network_handle_wifi_disabled()) is INITIATED --
 * i.e. "the web server is running or a stop for it hasn't been kicked off
 * yet". Distinct from import_web_stop_active (which only covers the actual
 * background-thread window): this is what lets gui_network_handle_wifi_
 * disabled() tell "nothing to do" apart from "need to tear down", and
 * flips false the instant teardown starts so a second caller (back button
 * racing the wifi-disabled path, or vice versa) sees it as already handled
 * rather than starting a second worker. */
static bool import_web_active = false;

/* Set by gui_network_handle_wifi_disabled() right before it kicks off the
 * same async stop worker/busy-overlay import_wifi_back_cb() below uses --
 * consumed once by poll_import_web_stop() so the "Update music database?"
 * popup (import_rescan_popup) only ever appears after a real user-initiated
 * exit, never merely because Wi-Fi disappeared out from under the screen. */
static bool import_web_stop_suppress_rescan_popup = false;

static void * import_web_stop_thread_func(void * arg) {
    (void) arg;
    import_web_stop();
    atomic_store_explicit(&import_web_stop_done_flag, true, memory_order_release); /* written last -- poll_import_web_stop() only checks this flag */
    return NULL;
}

void poll_import_web_stop(void) {
    if (!import_web_stop_active || !atomic_load_explicit(&import_web_stop_done_flag, memory_order_acquire)) return;

    import_web_stop_active = false;
    pthread_join(import_web_stop_thread, NULL);

    if (import_web_stop_nav_slot >= 0 && import_web_stop_nav_slot < gui_navigation_get_depth()) {
        nav_remove_stack_slot(import_web_stop_nav_slot);
    }
    gui_busy_hide(import_web_stop_token);
    import_web_stop_nav_slot = -1;

    bool suppress_popup = import_web_stop_suppress_rescan_popup;
    import_web_stop_suppress_rescan_popup = false;
    if (suppress_popup) return;

    lv_obj_remove_flag(import_rescan_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(import_rescan_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(import_rescan_popup_backdrop);
    lv_obj_move_foreground(import_rescan_popup);
}

/* Shared by import_wifi_back_cb() (user-initiated exit) and import_wifi_
 * handle_wifi_disabled() (Wi-Fi-loss-initiated) below -- both need the exact
 * same async stop-worker/busy-overlay sequence, just with a different
 * suppress_popup value. import_web_active is deliberately only cleared
 * AFTER pthread_create() actually succeeds, not optimistically beforehand:
 * if thread launch fails, import_web_stop() never runs and thttpd/
 * udp_server stay up, so leaving import_web_active true keeps that
 * reflected -- a retry (another back-button tap, or a later Wi-Fi-disabled
 * confirmation) still sees a stop as needed instead of wrongly treating
 * this as already handled and silently leaving the servers running
 * forever. */
static void import_web_stop_start(bool suppress_popup) {
    import_web_stop_suppress_rescan_popup = suppress_popup;
    import_web_stop_nav_slot = gui_navigation_get_depth() - 1; /* this screen's own slot, before pushing the busy screen on top of it */
    import_web_stop_token = gui_busy_show("Closing\nWeb Server...", "");

    atomic_store_explicit(&import_web_stop_done_flag, false, memory_order_relaxed);
    import_web_stop_active = true;
    if (pthread_create(&import_web_stop_thread, NULL, import_web_stop_thread_func, NULL) != 0) {
        import_web_stop_active = false;
        import_web_stop_suppress_rescan_popup = false;
        gui_busy_hide(import_web_stop_token);
        show_error_toast("Thread launch failed");
        return; /* import_web_active stays true -- nothing was actually stopped */
    }
    import_web_active = false; /* only now that the stop is genuinely in flight */
}

static void import_wifi_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    import_web_stop_start(false); /* a real user exit always wants the rescan prompt */
}

/* Wi-Fi-disabled counterpart to import_wifi_back_cb() above -- triggered by
 * gui_network_handle_wifi_disabled() instead of the user's own back-button
 * tap, and skips the rescan popup on completion since nothing was actually
 * imported this session. import_web_active being true guarantees import_
 * wifi_screen is the currently active screen: it skips finalize_screen_
 * navigation() (no swipe-to-back/swipe-up-home), so the explicit back
 * button -- which only runs import_web_stop_start() once it actually
 * launches the worker -- is the only way to leave it. Guarding on
 * import_web_stop_active additionally means a teardown already in flight
 * (from either caller) is left alone rather than racing a second worker
 * against it. */
static void import_wifi_handle_wifi_disabled(void) {
    if (!import_web_active || import_web_stop_active) return; /* not running, or already tearing down */
    import_web_stop_start(true);
}

static lv_obj_t * build_import_wifi_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, import_wifi_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Import via Wi-Fi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    import_wifi_status_label = lv_label_create(scr);
    lv_obj_set_width(import_wifi_status_label, lv_pct(90));
    lv_label_set_long_mode(import_wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(import_wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(import_wifi_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(import_wifi_status_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(import_wifi_status_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 30);

#if LV_USE_QRCODE
    import_wifi_qrcode = lv_qrcode_create(scr);
    lv_qrcode_set_size(import_wifi_qrcode, 220);
    lv_qrcode_set_dark_color(import_wifi_qrcode, lv_color_black());
    lv_qrcode_set_light_color(import_wifi_qrcode, lv_color_white());
    lv_obj_set_style_border_width(import_wifi_qrcode, 4, 0);
    lv_obj_set_style_border_color(import_wifi_qrcode, lv_color_white(), 0);
    lv_obj_align(import_wifi_qrcode, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 110);
#endif

    import_wifi_url_label = lv_label_create(scr);
    lv_obj_set_style_text_color(import_wifi_url_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(import_wifi_url_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_align(import_wifi_url_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 350);

    /* Deliberately skips finalize_screen_navigation()'s swipe-to-back --
     * an accidental swipe here would leave thttpd/udp_server running with
     * nothing to tear them down, same reasoning as text_entry_screen
     * skipping it for its keyboard. The back button above is the only way
     * out, and always runs import_web_stop(). */
    return scr;
}

/* ---- Shared Wi-Fi dependency guard for AirPlay/DLNA/Remote Control/Import
 * via Wi-Fi -- all four require the Wi-Fi radio/interface to be enabled
 * (NOT association with an access point; individual screens keep showing
 * their own existing "connect first" state for that). Checks gui_shell.c's
 * effective state (an in-flight disable counts as already off, see that
 * function's own comment) rather than wifi_control_is_enabled() directly,
 * shows the exact required toast, and returns false -- callers must bail
 * out without navigating, touching a setting, or starting anything. Shared
 * by both the tile taps below (screen not open yet) and each feature's own
 * enable/toggle path (screen may already be open when Wi-Fi goes away). */
static bool wifi_feature_guard(void) {
    if (gui_shell_wifi_effective_enabled()) return true;
    show_error_toast("Enable WiFi to access");
    return false;
}

static void open_import_wifi_screen(void) {
    if (!wifi_feature_guard()) return;
    import_web_start();
    populate_import_wifi_screen();
    nav_push(import_wifi_screen);
    import_web_active = true;
}

static void import_wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_import_wifi_screen();
}

/* ---- AirPlay screen -- WiFi analogue of Bluetooth DAC mode, see
 * airplay_control.h for the real mechanism (stock firmware's own shairport
 * binary). Same shape as build_bt_dac_screen(): a toggle plus an
 * explanation, and the same three-way mutual exclusion with local playback
 * and Bluetooth DAC. ---- */

static lv_obj_t * airplay_list;

static void airplay_toggle_cb(lv_event_t * e);

static void populate_airplay_screen(void) {
    lv_obj_clean(airplay_list);
    add_pill_toggle_row(airplay_list, "AirPlay", current_settings.wifi_dac_mode_enabled, airplay_toggle_cb);

    lv_obj_t * explanation = lv_label_create(airplay_list);
    lv_label_set_text(explanation,
                      "When on, this device is visible to AirPlay senders on your Wi-Fi network -- stream "
                      "audio from an iPhone, iPad, or Mac to be played through this device's own output.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, 24, 0);
    lv_obj_set_style_pad_top(explanation, 12, 0);
}

/* /usr/resource/hostname is the same file wifi_on.sh already reads for the
 * DHCP hostname it advertises -- reusing it here keeps the name AirPlay
 * senders see consistent with how this device already identifies itself on
 * the network, rather than inventing a second name. "HiBy_Music" matches
 * that script's own fallback default when the file is missing. */
void get_device_name(char * out, size_t out_size) {
    snprintf(out, out_size, "HiBy_Music");
    FILE * f = fopen("/usr/resource/hostname", "r");
    if (f) {
        if (fgets(out, (int) out_size, f)) out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

static void airplay_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool turning_on = !current_settings.wifi_dac_mode_enabled;
    /* Guard the enable path even though the tile itself is already guarded
     * (airplay_tile_cb() below) -- this screen can already be open when
     * Wi-Fi goes away, so a stale toggle tap must still be rejected here.
     * Checked before touching the setting at all, so a rejected enable
     * never even briefly persists as on. Turning OFF is always allowed,
     * Wi-Fi state notwithstanding -- an already-enabled feature must still
     * be toggleable off. */
    if (turning_on && !wifi_feature_guard()) {
        populate_airplay_screen(); /* nothing changed, but keeps the toggle row's own drawn state honest */
        return;
    }
    current_settings.wifi_dac_mode_enabled = turning_on;
    settings_save(&current_settings);

    if (current_settings.wifi_dac_mode_enabled) {
        char name[64];
        get_device_name(name, sizeof(name));
        if (!airplay_control_start(name)) {
            /* Startup is transactional (see airplay_control_start()'s own
             * comment) -- a false return means nothing was actually left
             * running, so just undo the toggle rather than reporting the
             * device as "on" when it can't produce sound or be discovered. */
            current_settings.wifi_dac_mode_enabled = false;
            settings_save(&current_settings);
            show_error_toast("Failed to enable AirPlay");
            populate_airplay_screen();
            return;
        }

        /* Deliberately does NOT touch local playback here -- toggling
         * AirPlay only makes this device discoverable/ready to receive
         * (airplay_bridge_start() starts in a LISTENING state, see its own
         * comment); an actual incoming AirPlay stream starting is what
         * stops local playback (gui_network.c's gui_network_poll_airplay_
         * overlay(), reacting to airplay_bridge_is_streaming()), not this
         * toggle. Still mutually exclusive with Bluetooth DAC mode
         * specifically, unlike local playback: BT DAC mode's bluealsa-aplay
         * writes straight to the local hw:0 device as an entirely separate
         * process, outside this app's own audio_output.c arbitration, so
         * the two really can conflict at the hardware level the moment
         * AirPlay does start streaming -- turning BT DAC off here, rather
         * than only when AirPlay actually starts streaming, avoids ever
         * having both simultaneously fighting over real hardware. */
        if (current_settings.bt_dac_mode_enabled) {
            current_settings.bt_dac_mode_enabled = false;
            settings_save(&current_settings);
            start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        }
    } else {
        airplay_control_stop();
    }

    populate_airplay_screen();
}

static lv_obj_t * build_airplay_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("AirPlay", &title_label, &airplay_list);
}

static void open_airplay_screen(void) {
    populate_airplay_screen();
    nav_push(airplay_screen);
}

static void airplay_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!wifi_feature_guard()) return;
    open_airplay_screen();
}

/* ---- AirPlay overlay: shown only while a stream is actually active -----
 * An earlier pass tried reusing the Player screen's own widgets for this
 * (hiding its transport controls while AirPlay played) -- dropped after
 * real-device testing: a shadowed-global bug silently left prev/next
 * unhideable, the layout looked broken with only some controls hidden,
 * and it coupled this feature into player-owned globals with no other
 * reason to know about AirPlay. A dedicated overlay, same shape as
 * bt_dac_overlay_screen/usb_dac_overlay_screen, has none of that: nothing
 * to hide, nothing to accidentally leave half-hidden. Unlike those two
 * static overlays, this one has real dynamic content (cover art + song
 * name), fed by airplay_metadata.c the same way the old Player-screen
 * integration was, just applied to this screen's own widgets instead.
 * No back-button confirmation popup (unlike BT DAC's) -- there is nothing
 * to "leave" here in the sense of a mode switch; navigating away just
 * stops looking at it, the stream itself keeps flowing regardless of
 * which screen is on top, and gui_network_poll_airplay_overlay() below
 * pops back out on its own once the stream actually ends. ---- */

static void airplay_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

static lv_obj_t * build_airplay_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, airplay_overlay_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    airplay_overlay_cover_img = lv_image_create(scr);
    lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));
    lv_obj_align(airplay_overlay_cover_img, LV_ALIGN_CENTER, 0, -60);

    airplay_overlay_title_label = lv_label_create(scr);
    lv_label_set_text(airplay_overlay_title_label, "AirPlay");
    lv_obj_add_style(airplay_overlay_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(airplay_overlay_title_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_set_width(airplay_overlay_title_label, lv_pct(85));
    lv_label_set_long_mode(airplay_overlay_title_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(airplay_overlay_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(airplay_overlay_title_label, airplay_overlay_cover_img, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

    /* The AirPlay settings tile's own "wireless/airplay.png" is a stock
     * theme2 asset with a completely different (colored, wireless-grid)
     * style -- not the recognizable AirPlay-audio glyph (concentric arcs
     * over an upward triangle). This is a project-owned custom asset
     * instead: assets/theme2/playing_plane/airplay_logo_white.png, a
     * solid-white recolor of the standard glyph (transparent background,
     * alpha-preserved from the original anti-aliased edges) sized for this
     * overlay specifically. Resolved via asset_path()'s own THEME_OVERRIDE_
     * ROOT mechanism on target (see assets.c's own comment -- exactly the
     * "asset this app adds with no stock equivalent" case that exists for)
     * and committed directly under assets/theme2/ for the host build,
     * matching stream_media/subsonic.png's own precedent. */
    lv_obj_t * airplay_logo = lv_image_create(scr);
    lv_image_set_src(airplay_logo, asset_path("playing_plane/airplay_logo_white.png"));
    lv_obj_align(airplay_logo, LV_ALIGN_BOTTOM_MID, 0, -24);

    return scr;
}

/* Poll from update_timer_cb, same shape as every other background-thread-
 * to-UI-thread poll in this codebase. Shows/hides the overlay based on
 * airplay_bridge_is_streaming() transitions and applies whatever metadata
 * has arrived meanwhile -- decoupled from whether the overlay happens to
 * be the currently active screen, so metadata keeps updating even if the
 * user navigated away from it. */
void gui_network_poll_airplay_overlay(void) {
    /* Self-heals a respawn failure from airplay_control_disconnect_active_
     * stream()'s kill-and-respawn (gui_player.c, triggered by local playback
     * resuming while AirPlay was streaming) -- that path deliberately
     * ignores airplay_control_start()'s return value (see its own comment),
     * so this is the only place such a failure ever gets noticed and the
     * persisted setting corrected, same as airplay_toggle_cb()'s own
     * synchronous failure handling a few hundred lines up in this file. */
    if (current_settings.wifi_dac_mode_enabled && !airplay_control_is_active()) {
        current_settings.wifi_dac_mode_enabled = false;
        settings_save(&current_settings);
        show_error_toast("AirPlay stopped unexpectedly");
        populate_airplay_screen();
    }

    bool streaming = airplay_bridge_is_streaming();
    if (streaming != airplay_overlay_showing) {
        airplay_overlay_showing = streaming;
        if (streaming) {
            /* airplay_bridge.c's own reader thread already called
             * audio_stop() before flipping is_streaming() true (see its own
             * comment) -- that only stops the backend, same as usb_mode_
             * control_apply()'s own DAC-mode takeover a few hundred lines up
             * in this file; the play button icon and plugin state still
             * need their own explicit refresh here on the UI thread, or
             * they'd keep claiming playback is active/paused for a track
             * that AirPlay just silently took the output device out from
             * under. */
            set_play_button_state(false);
            plugin_manager_notify_stopped();

            airplay_overlay_nav_slot = gui_navigation_get_depth(); /* this screen's own slot, about to be occupied */
            nav_push(airplay_overlay_screen);
        } else {
            /* Reset to defaults here, on SESSION END, not on the next
             * session's start -- metadata for a new session can (and
             * regularly does) arrive slightly before airplay_bridge_is_
             * streaming() flips true for it (see the unconditional-apply
             * comment below), and resetting on start would wipe that
             * early update right back out, leaving the "AirPlay"
             * placeholder up for the entire session since nothing else
             * would trigger a re-apply. Resetting here instead means by
             * the time a NEW session's early metadata can possibly arrive,
             * this session's leftover content is already gone -- closing
             * the same staleness gap without racing a legitimate early
             * update for the next session. */
            lv_label_set_text(airplay_overlay_title_label, "AirPlay");
            free(airplay_overlay_cover_bytes);
            airplay_overlay_cover_bytes = NULL;
            lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));

            if (lv_screen_active() == airplay_overlay_screen) {
                /* Only auto-dismiss if the user is still looking at it. */
                nav_pop();
            } else if (airplay_overlay_nav_slot >= 0 && airplay_overlay_nav_slot < gui_navigation_get_depth()) {
                /* User already navigated elsewhere with the overlay still
                 * buried somewhere in their back-navigation history --
                 * splice its now-defunct slot out entirely (same pattern as
                 * poll_import_web_stop()'s import_web_stop_nav_slot) rather
                 * than leaving it there for a later "back" to resurface a
                 * screen with no active stream behind it. */
                nav_remove_stack_slot(airplay_overlay_nav_slot);
            }
            airplay_overlay_nav_slot = -1;
        }
    }

    airplay_metadata_update_t upd;
    if (!airplay_metadata_consume_update(&upd)) return;

    /* Applied regardless of `streaming` -- the metadata and PCM bridge are
     * two independent threads/connections (airplay_metadata.c vs. airplay_
     * bridge.c), so a title/cover update for a session can arrive slightly
     * before airplay_bridge_is_streaming() actually flips true. Discarding
     * it here (an earlier version of this function did) meant the overlay
     * showed the plain "AirPlay" placeholder for the entire first track of
     * every session unless a later track change happened to resend it.
     * Applying it always is safe: the streaming->false reset above already
     * runs the instant a session ends, before a NEW session's own metadata
     * could possibly arrive, so nothing from a PREVIOUS session is ever
     * still sitting in these widgets by the time an early update for a new
     * one shows up here. */
    lv_label_set_text(airplay_overlay_title_label, upd.title[0] ? upd.title : "AirPlay");

    if (!upd.has_cover) {
        free(airplay_overlay_cover_bytes);
        airplay_overlay_cover_bytes = NULL;
        lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));
        return;
    }

    free(airplay_overlay_cover_bytes);
    airplay_overlay_cover_bytes = (uint8_t *) upd.cover_pixels;

    /* LV_IMAGE_HEADER_MAGIC is required, not decorative -- see gui_player.c's
     * poll_cover_decode() for the real-device corruption bug a zeroed magic
     * field causes (lv_bin_decoder.c "fixes up" what it treats as an old-
     * format header in place, silently overwriting the color format). */
    memset(&airplay_overlay_cover_dsc, 0, sizeof(airplay_overlay_cover_dsc));
    airplay_overlay_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    airplay_overlay_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    airplay_overlay_cover_dsc.header.w = AIRPLAY_COVER_WIDTH;
    airplay_overlay_cover_dsc.header.h = AIRPLAY_COVER_HEIGHT;
    airplay_overlay_cover_dsc.header.stride = AIRPLAY_COVER_WIDTH * 2;
    airplay_overlay_cover_dsc.data = airplay_overlay_cover_bytes;
    airplay_overlay_cover_dsc.data_size = (uint32_t) AIRPLAY_COVER_WIDTH * AIRPLAY_COVER_HEIGHT * 2;
    lv_image_set_src(airplay_overlay_cover_img, &airplay_overlay_cover_dsc);
}

/* ---- DLNA screen -- see dlna_control.h for the real mechanism (stock
 * firmware's own dmrd binary) and its documented limitations (Pause/Mute/
 * reliable-Volume-feedback/Seek are all confirmed broken inside dmrd
 * itself on real-device testing, independent of anything built here).
 * Same toggle-plus-explanation shape as build_airplay_screen(), but no
 * mutual exclusion with Bluetooth DAC/AirPlay -- see settings.h's own
 * comment on dlna_renderer_enabled for why. ---- */

static lv_obj_t * dlna_list;

static void dlna_toggle_cb(lv_event_t * e);

static void populate_dlna_screen(void) {
    lv_obj_clean(dlna_list);
    add_pill_toggle_row(dlna_list, "DLNA Renderer", current_settings.dlna_renderer_enabled, dlna_toggle_cb);

    lv_obj_t * explanation = lv_label_create(dlna_list);
    lv_label_set_text(explanation,
                      "When on, this device is visible to DLNA/UPnP controller apps on your Wi-Fi network -- cast "
                      "a track from one to play it here. Pause, mute, volume, and seek from the controller app "
                      "aren't supported; use this device's own controls instead once a track starts.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, 24, 0);
    lv_obj_set_style_pad_top(explanation, 12, 0);
}

static void dlna_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool turning_on = !current_settings.dlna_renderer_enabled;
    /* Same defensive enable guard as airplay_toggle_cb() -- this screen can
     * already be open when Wi-Fi is disabled elsewhere, so the tile guard
     * alone (dlna_tile_cb() below) isn't sufficient. */
    if (turning_on && !wifi_feature_guard()) {
        populate_dlna_screen();
        return;
    }
    current_settings.dlna_renderer_enabled = turning_on;
    settings_save(&current_settings);

    if (current_settings.dlna_renderer_enabled) {
        dlna_control_start();
    } else {
        dlna_control_stop();
    }

    populate_dlna_screen();
}

static lv_obj_t * build_dlna_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("DLNA", &title_label, &dlna_list);
}

static void open_dlna_screen(void) {
    populate_dlna_screen();
    nav_push(dlna_screen);
}

/* ---- Remote Control screen (Wireless -> "Open Link") -- see
 * remote_control.h for the Phase 1 (read-only Now Playing web page, no
 * playback control yet, no auth) scope. Toggle row styled like
 * build_dlna_screen()'s own row; the IP/QR/URL display below it is styled
 * exactly like build_import_wifi_screen()'s (same colors, sizes, and even
 * the same 20px QR-to-URL-label gap) -- this is the same "here's an
 * address, scan or type it" moment for the user, just for a different
 * feature. ---- */

static lv_obj_t * remote_control_toggle_img;
static lv_obj_t * remote_control_status_label;
static lv_obj_t * remote_control_url_label;
#if LV_USE_QRCODE
static lv_obj_t * remote_control_qrcode;
#endif

/* Re-chains remote_control_qrcode/remote_control_url_label below
 * remote_control_status_label's own CURRENT bottom edge -- must be called
 * again every time that label's text changes (all 3 branches below), not
 * just once at screen-build time: lv_obj_align_to() computes an absolute
 * position from the base object's size at the moment it's called, it does
 * NOT track the base object live, so a later lv_label_set_text() on a
 * differently-sized string (the whole point of this label -- it cycles
 * between "Turn this on...", "Connect to Wi-Fi first", and "Open this
 * address...", three different lengths) would otherwise leave the chain
 * still positioned for whatever text happened to be showing at build time. */
static void remote_control_relayout_below_status(void) {
    lv_obj_t * last = remote_control_status_label;
#if LV_USE_QRCODE
    lv_obj_align_to(remote_control_qrcode, last, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    last = remote_control_qrcode;
#endif
    lv_obj_align_to(remote_control_url_label, last, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
}

static void remote_control_refresh_address(void) {
    if (!current_settings.remote_control_enabled) {
        lv_label_set_text(remote_control_status_label,
                           "Turn this on to see the address here.");
        lv_label_set_text(remote_control_url_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        remote_control_relayout_below_status();
        return;
    }

    wifi_info_t info;
    if (!wifi_control_get_info(&info) || info.ip[0] == '\0') {
        lv_label_set_text(remote_control_status_label, "Connect to Wi-Fi first");
        lv_label_set_text(remote_control_url_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        remote_control_relayout_below_status();
        return;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://%s:8899", info.ip);
    lv_label_set_text(remote_control_status_label, "Open this address on your phone or computer:");
    lv_label_set_text(remote_control_url_label, url);
#if LV_USE_QRCODE
    lv_obj_remove_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
    lv_qrcode_update(remote_control_qrcode, url, strlen(url));
#endif
    remote_control_relayout_below_status();
}

static void remote_control_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool turning_on = !current_settings.remote_control_enabled;
    /* Same defensive enable guard as airplay_toggle_cb()/dlna_toggle_cb() --
     * this screen can already be open when Wi-Fi is disabled elsewhere. */
    if (turning_on && !wifi_feature_guard()) {
        remote_control_refresh_address(); /* nothing changed, but keeps the address text/QR state honest */
        return;
    }
    current_settings.remote_control_enabled = turning_on;
    settings_save(&current_settings);

    lv_image_set_src(remote_control_toggle_img,
                      asset_path(current_settings.remote_control_enabled ? "settings/on.png" : "settings/off.png"));
    /* Keeps the LV_STATE_CHECKED selector on remote_control_toggle_img's own
     * style_accent attachment (see build_remote_control_screen()'s own
     * comment) tracking reality, so the accent recolor only ever shows on
     * the ON sprite -- see add_pill_toggle_row()'s own bug report #2 for
     * why an unconditional recolor is wrong here. */
    if (current_settings.remote_control_enabled) lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(remote_control_toggle_img, LV_STATE_CHECKED);
    if (current_settings.remote_control_enabled) {
        remote_control_start();
    } else {
        remote_control_stop();
    }
    remote_control_refresh_address();
}

static lv_obj_t * build_remote_control_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Remote Control");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    /* Same font-tier-aware pill geometry as add_pill_row_base(), built
     * directly here (not via that helper) since it needs to sit above the
     * absolutely-positioned Import-Wi-Fi-style fields below rather than
     * inside a flex-column list. */
    lv_obj_t * toggle_row = lv_obj_create(scr);
    int32_t toggle_row_width = pill_row_default_width();
    lv_obj_set_size(toggle_row, toggle_row_width, 124);
    lv_obj_align(toggle_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 10);
    lv_obj_add_style(toggle_row, &style_theme_screen_bg, 0);
    if (toggle_row_width == 448) {
        lv_obj_set_style_bg_image_src(toggle_row, asset_path("touch_list/item_bg.png"), 0);
    } else {
        lv_obj_set_style_radius(toggle_row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(toggle_row, LIST_ROW_BG_COLOR, 0);
    }
    lv_obj_set_style_bg_opa(toggle_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(toggle_row, 0, 0);
    lv_obj_remove_flag(toggle_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toggle_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(toggle_row, remote_control_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * toggle_label = lv_label_create(toggle_row);
    lv_label_set_text(toggle_label, "Remote Control");
    lv_obj_add_style(toggle_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(toggle_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(toggle_label, LV_ALIGN_LEFT_MID, 24, 0);

    remote_control_toggle_img = lv_image_create(toggle_row);
    lv_image_set_src(remote_control_toggle_img,
                      asset_path(current_settings.remote_control_enabled ? "settings/on.png" : "settings/off.png"));
    lv_obj_align(remote_control_toggle_img, LV_ALIGN_RIGHT_MID, -20, 0);
    if (current_settings.remote_control_enabled) lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
    /* Real-device bug report: accent color wasn't applying to this toggle
     * either. Unlike add_pill_toggle_row() (rebuilt fresh on every screen
     * repopulation, so a plain local color read is fine there), this
     * screen is built exactly once at startup (see its own call site) and
     * never rebuilt -- needs the live-updating shared style so a later
     * accent color change still reaches it.
     *
     * Real-device bug report #2: LV_STATE_CHECKED selector, not the
     * unconditional 0 this originally used -- otherwise the recolor hits
     * both the ON and OFF sprite alike, making the toggle unreadable in
     * either state. lv_obj_add_state() above (and remote_control_toggle_
     * cb()'s own add/clear) keeps this selector matching which sprite is
     * actually showing. */
    lv_obj_add_style(remote_control_toggle_img, gui_theme_accent_style(), LV_STATE_CHECKED);

    lv_obj_t * explanation = lv_label_create(scr);
    lv_label_set_text(explanation,
                       "Lets anyone on your Wi-Fi network who opens the address below see what's playing, "
                       "control playback, and browse your library -- no password.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(explanation, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(explanation, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 150);

    remote_control_status_label = lv_label_create(scr);
    lv_obj_set_width(remote_control_status_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(remote_control_status_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    /* Real-device bug report: "Connect to Wi-Fi first" overlapped the
     * explanation text above it at bigger font tiers -- `explanation`
     * above wraps across more/taller lines as gui_theme_font(GUI_FONT_ROLE_SUBTEXT) grows (BlindMF),
     * but this label's own Y was a hardcoded absolute offset from the
     * title, sized assuming `explanation` always stayed within its
     * smallest-tier height. Anchored to `explanation`'s own actual bottom
     * edge instead, so it always clears however tall that text really
     * rendered. */
    lv_obj_align_to(remote_control_status_label, explanation, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);

#if LV_USE_QRCODE
    remote_control_qrcode = lv_qrcode_create(scr);
    lv_qrcode_set_size(remote_control_qrcode, 220);
    lv_qrcode_set_dark_color(remote_control_qrcode, lv_color_black());
    lv_qrcode_set_light_color(remote_control_qrcode, lv_color_white());
    lv_obj_set_style_border_width(remote_control_qrcode, 4, 0);
    lv_obj_set_style_border_color(remote_control_qrcode, lv_color_white(), 0);
#endif

    remote_control_url_label = lv_label_create(scr);
    lv_obj_set_style_text_color(remote_control_url_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(remote_control_url_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    /* Positions qrcode/url_label below remote_control_status_label for the
     * first time -- open_remote_control_screen() calls remote_control_
     * refresh_address() (which calls this same helper again) every time
     * this screen opens, so this initial call just avoids either object
     * sitting at LVGL's own default (0,0) for one frame before that. */
    remote_control_relayout_below_status();

    finalize_screen_navigation(scr);
    return scr;
}

static void open_remote_control_screen(void) {
    remote_control_refresh_address();
    nav_push(remote_control_screen);
}

static void remote_control_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!wifi_feature_guard()) return;
    open_remote_control_screen();
}

static void dlna_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!wifi_feature_guard()) return;
    open_dlna_screen();
}

/* Centralized Wi-Fi-disabled cleanup for all four Wi-Fi-dependent features
 * -- called by gui_shell.c's poll_wifi_toggle() only once a Wi-Fi disable is
 * authoritatively confirmed (see that call site's own comment). Stops
 * whichever of AirPlay/DLNA/Remote Control/Import via Wi-Fi is actually
 * running and corrects their persisted settings, refreshing any of their
 * screens/toggles already built so nothing shows stale "on" state. A single
 * settings_save() at the end covers every flag this pass touched, rather
 * than one write per feature. */
void gui_network_handle_wifi_disabled(void) {
    bool settings_changed = false;

    if (current_settings.wifi_dac_mode_enabled) {
        airplay_control_stop();
        current_settings.wifi_dac_mode_enabled = false;
        settings_changed = true;
        populate_airplay_screen();
    }

    if (current_settings.dlna_renderer_enabled) {
        dlna_control_stop();
        current_settings.dlna_renderer_enabled = false;
        settings_changed = true;
        populate_dlna_screen();
    }

    if (current_settings.remote_control_enabled) {
        remote_control_stop();
        current_settings.remote_control_enabled = false;
        settings_changed = true;
        lv_image_set_src(remote_control_toggle_img, asset_path("settings/off.png"));
        lv_obj_clear_state(remote_control_toggle_img, LV_STATE_CHECKED);
        remote_control_refresh_address();
    }

    /* Import via Wi-Fi has no persisted enabled flag (session-only, started
     * only when its screen opens) -- import_wifi_handle_wifi_disabled() is
     * a cheap no-op whenever it isn't currently running. */
    import_wifi_handle_wifi_disabled();

    if (settings_changed) settings_save(&current_settings);
}

static lv_obj_t * build_wireless_screen(void) {
    static icon_grid_item_t items[6];
    items[0] = (icon_grid_item_t){ "wireless/wifi.png", "wireless/wifi_s.png", "Wi-Fi", wifi_tile_cb, NULL };
    items[1] = (icon_grid_item_t){ "wireless/bt.png", "wireless/bt_s.png", "Bluetooth", bt_tile_cb, NULL };
    items[2] = (icon_grid_item_t){ "wireless/airplay.png", "wireless/airplay_s.png", "AirPlay", airplay_tile_cb, NULL };
    items[3] = (icon_grid_item_t){ "wireless/dlna.png", "wireless/dlna_s.png", "DLNA", dlna_tile_cb, NULL };
    /* Relabeled from the stock "HiBy Link" -- this project doesn't implement
     * that proprietary protocol (see the remote-control design doc/plan for
     * why: only partially recoverable from firmware strings, and only useful
     * against HiBy's own closed-source app). This is that plan's own Phase 1
     * (read-only Now Playing web page) now wired up -- see
     * build_remote_control_screen(). */
    /* Shortened from "Remote Control"/"Import via Wi-Fi" -- real-device
     * feedback: the full names overflowed this tile's caption width. The
     * screens these tiles open keep their own full titles (build_remote_
     * control_screen()/Import via Wi-Fi's own title label) -- only the grid
     * caption is shortened. */
    items[4] = (icon_grid_item_t){ "wireless/hibylink.png", "wireless/hibylink_s.png", "Remote", remote_control_tile_cb, NULL };
    items[5] = (icon_grid_item_t){ "wireless/via.png", "wireless/via_s.png", "Import", import_wifi_tile_cb, NULL };
    /* 160: bigger than the shared 100% default -- explicit user request.
     * Bumped again from an earlier 120 once label_inside_icon (see
     * build_icon_grid_screen()'s own comment) stopped reserving a separate
     * below-icon label row, freeing up the room to grow the icon itself
     * rather than just its surrounding whitespace -- lands at very close to
     * these assets' own native 212x190 resolution within this screen's
     * available cell height. */
    lv_obj_t * scr = build_icon_grid_screen("Wireless", generic_back_cb, items, 6, 160, true, 0);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_network_init(void) {
    wifi_screen = build_wifi_screen();
    wifi_info_screen = build_wifi_info_screen();
    wifi_dns_screen = build_wifi_dns_screen();
    bt_screen = build_bluetooth_screen();
    bt_dac_screen = build_bt_dac_screen();
    bt_dac_overlay_screen = build_bt_dac_overlay_screen();
    bt_codec_screen = build_bt_codec_screen();
    font_size_screen = build_font_size_screen();
    replaygain_mode_screen = build_replaygain_mode_screen();
    resume_mode_screen = build_resume_mode_screen();
    play_pause_button_mode_screen = build_play_pause_button_mode_screen();
    usb_mode_screen = build_usb_mode_screen();
    usb_dac_overlay_screen = build_usb_dac_overlay_screen();
    build_import_rescan_popup();
    import_wifi_screen = build_import_wifi_screen();
    airplay_screen = build_airplay_screen();
    airplay_overlay_screen = build_airplay_overlay_screen();
    dlna_screen = build_dlna_screen();
    remote_control_screen = build_remote_control_screen();
    wireless_screen = build_wireless_screen();
    build_bt_action_popup();
    build_wifi_action_popup();
    build_usb_dac_leave_popup();
    build_bt_dac_leave_popup();
    dac_stream_labels_timer_cb(NULL);
    lv_timer_create(dac_stream_labels_timer_cb, 500, NULL);
}

bool gui_network_has_background_work(void) {
    return wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active ||
           wifi_scan_active || wifi_forget_active || bt_connect_active || bt_forget_active ||
           bt_scan_active || usb_mode_switch_active || import_web_stop_active;
}

void gui_network_cancel_background_work(void) {
    if (wifi_connect_active) {
        pthread_join(wifi_connect_thread, NULL);
        wifi_connect_active = false;
        gui_busy_hide(wifi_connect_token);
    }
    if (wifi_connect_saved_active) {
        pthread_join(wifi_connect_saved_thread, NULL);
        wifi_connect_saved_active = false;
        gui_busy_hide(wifi_connect_saved_token);
    }
    if (wifi_disconnect_active) {
        pthread_join(wifi_disconnect_thread, NULL);
        wifi_disconnect_active = false;
    }
    if (wifi_scan_active) {
        pthread_join(wifi_scan_thread, NULL);
        wifi_scan_active = false;
    }
    if (wifi_forget_active) {
        pthread_join(wifi_forget_thread, NULL);
        wifi_forget_active = false;
    }
    if (bt_connect_active) {
        pthread_join(bt_connect_thread, NULL);
        bt_connect_active = false;
    }
    if (bt_forget_active) {
        pthread_join(bt_forget_thread, NULL);
        bt_forget_active = false;
    }
    if (bt_scan_active) {
        pthread_join(bt_scan_thread, NULL);
        bt_scan_active = false;
    }
    if (usb_mode_switch_active) {
        pthread_join(usb_mode_switch_thread, NULL);
        usb_mode_switch_active = false;
    }
    if (import_web_stop_active) {
        pthread_join(import_web_stop_thread, NULL);
        import_web_stop_active = false;
        gui_busy_hide(import_web_stop_token);
    }
}


lv_obj_t * gui_network_get_wifi_screen(void) { return wifi_screen; }
lv_obj_t * gui_network_get_bt_screen(void) { return bt_screen; }
lv_obj_t * gui_network_get_wireless_screen(void) { return wireless_screen; }
lv_obj_t * gui_network_get_bt_dac_overlay(void) { return bt_dac_overlay_screen; }
lv_obj_t * gui_network_get_usb_dac_overlay(void) { return usb_dac_overlay_screen; }
