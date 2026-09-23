#pragma once
#include <lvgl/lvgl.h>
#include "usb_mode_control.h"

/* Screen accessors */
lv_obj_t * gui_network_get_wifi_screen(void);
lv_obj_t * gui_network_get_bt_screen(void);
lv_obj_t * gui_network_get_wireless_screen(void);
lv_obj_t * gui_network_get_bt_dac_overlay(void);
lv_obj_t * gui_network_get_usb_dac_overlay(void);

/* True while USB DAC mode owns the UI -- i.e. the device is presenting
 * itself to a PC as a sound card and its overlay has taken the screen.
 * Deliberately NOT current_settings.usb_mode, which is only a persisted UI
 * hint and can still read DAC after a restart with no cable attached. */
bool gui_network_usb_dac_mode_active(void);
lv_obj_t * gui_network_get_import_wifi_screen(void);

void gui_network_init(void);
#ifdef TEST_BOOT_RC
/* Test build only: power Bluetooth and start the remote-control transports
 * asynchronously after the UI has completed its normal session-mode reset. */
void gui_network_start_test_boot_services(void);
#endif
/* Deletes every screen/popup this module owns so gui_reload.c's in-process
 * UI reload can call gui_network_init() again from a clean slate. */
void gui_network_teardown(void);
void gui_network_refresh_wireless_screen(void);
void gui_network_poll_airplay_overlay(void);
void poll_wifi_connect(void);
void poll_wifi_disconnect(void);
void poll_wifi_scan(void);
void poll_bt_action(void);
void poll_bt_dac(void);
void poll_usb_dac_overlay(void);
void poll_import_web(void);
void get_device_name(char * out, size_t out_size);

void populate_bt_dac_screen(void);
void poll_import_web_stop(void);
void poll_usb_mode_switch(void);
void poll_usb_storage_hotplug(void);
/* UI-thread snapshot: a real Storage host was observed on this cable session. */
bool gui_network_usb_storage_session_active(void);
void build_bt_action_popup(void);
void build_wifi_action_popup(void);
void build_usb_dac_leave_popup(void);
void build_bt_dac_leave_popup(void);

void open_wifi_screen(void);
void open_bluetooth_screen(void);

/* The Sample Rate screen shows one accessory's rates and edits that
 * accessory's stored entry, so it must redraw when the connected device
 * changes underneath it. No-op unless that screen is on top. */
void gui_network_notify_bt_device_changed(void);
void populate_bt_screen(void);
void gui_network_show_wifi_toggle_pending(bool enabled);
void gui_network_wifi_toggle_completed(bool enabled);

bool start_usb_mode_switch(usb_mode_t target);
bool gui_network_adb_active(void);

bool gui_network_has_background_work(void);
void gui_network_cancel_background_work(void);

/* Called once Wi-Fi disable is authoritatively confirmed (gui_shell.c's
 * poll_wifi_toggle()) -- stops AirPlay/DLNA/Remote Control/Import via Wi-Fi
 * and clears their persisted enabled flags, since all four require Wi-Fi.
 * Cheap no-op for whichever of them wasn't running. */
void gui_network_handle_wifi_disabled(void);

/* Flip AirPlay / DLNA / the phone remote-control server, and return the state
 * it actually ended up in. Each is the same function the matching settings
 * screen's own toggle row runs, so the quick drawer's tiles inherit every
 * Wi-Fi guard, busy-service check, "Service is busy" toast, transactional
 * start and Bluetooth-DAC mutual exclusion rather than duplicating any of it.
 * The returned state is not always the requested one: a rejected enable (no
 * Wi-Fi, or the service still settling) comes back still-off, which is what
 * the caller should draw. Safe to call before the corresponding settings
 * screen has ever been built. */
bool gui_network_toggle_airplay(void);
bool gui_network_toggle_dlna(void);
bool gui_network_toggle_remote_control(void);
