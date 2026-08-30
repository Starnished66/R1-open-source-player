#pragma once
#include <lvgl/lvgl.h>
#include "usb_mode_control.h"

/* Screen accessors */
lv_obj_t * gui_network_get_wifi_screen(void);
lv_obj_t * gui_network_get_bt_screen(void);
lv_obj_t * gui_network_get_wireless_screen(void);
lv_obj_t * gui_network_get_bt_dac_overlay(void);
lv_obj_t * gui_network_get_usb_dac_overlay(void);

void gui_network_init(void);
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
void build_bt_action_popup(void);
void build_wifi_action_popup(void);
void build_usb_dac_leave_popup(void);
void build_bt_dac_leave_popup(void);

void open_wifi_screen(void);
void open_bluetooth_screen(void);
void populate_bt_screen(void);
void gui_network_show_wifi_toggle_pending(bool enabled);
void gui_network_wifi_toggle_completed(bool enabled);

void start_usb_mode_switch(usb_mode_t target);

bool gui_network_has_background_work(void);
void gui_network_cancel_background_work(void);

/* Called once Wi-Fi disable is authoritatively confirmed (gui_shell.c's
 * poll_wifi_toggle()) -- stops AirPlay/DLNA/Remote Control/Import via Wi-Fi
 * and clears their persisted enabled flags, since all four require Wi-Fi.
 * Cheap no-op for whichever of them wasn't running. */
void gui_network_handle_wifi_disabled(void);
