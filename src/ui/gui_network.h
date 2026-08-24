#pragma once
#include <lvgl/lvgl.h>

extern lv_obj_t * wifi_screen;
extern lv_obj_t * wifi_info_screen;
extern lv_obj_t * wifi_dns_screen;
extern lv_obj_t * bt_screen;
extern lv_obj_t * bt_dac_screen;
extern lv_obj_t * bt_dac_overlay_screen;
extern lv_obj_t * bt_codec_screen;
extern lv_obj_t * usb_mode_screen;
extern lv_obj_t * usb_dac_overlay_screen;
extern lv_obj_t * import_wifi_screen;
extern lv_obj_t * airplay_screen;
extern lv_obj_t * dlna_screen;
extern lv_obj_t * remote_control_screen;
extern lv_obj_t * wireless_screen;
extern lv_obj_t * resume_mode_screen;
extern lv_obj_t * resume_mode_list;
extern lv_obj_t * play_pause_button_mode_screen;
extern lv_obj_t * font_size_screen;
extern lv_obj_t * replaygain_mode_screen;

void gui_network_init(void);
void poll_wifi_connect(void);
void poll_wifi_disconnect(void);
void poll_wifi_scan(void);
void poll_bt_action(void);
void poll_bt_dac(void);
void poll_usb_dac_overlay(void);
void poll_import_web(void);
void show_font_size_reboot_popup(void);
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

#include "usb_mode_control.h"
void start_usb_mode_switch(usb_mode_t target);
