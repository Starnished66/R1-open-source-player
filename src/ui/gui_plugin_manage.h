#ifndef GUI_PLUGIN_MANAGE_H
#define GUI_PLUGIN_MANAGE_H

#include <lvgl/lvgl.h>

/* "Manage Plugins" -- a native settings screen listing every *.lua file
 * under <SD card>/.plugins/ (plugin_manager_scan_available()) with a
 * per-file enable/disable toggle, plus a "Refresh Plugins" row. Each
 * toggle is persisted immediately; leaving the screen coalesces any
 * changed toggles into one full plugin/UI reload (the same path plugin.
 * reload_ui() uses) rather than rebuilding once per toggle, while
 * "Refresh Plugins" always triggers that same reload immediately (useful
 * even with nothing toggled, e.g. right after copying a new plugin file
 * onto the SD card -- plugin_manager_init() re-scans .plugins/ from disk
 * every time it runs). */
lv_obj_t * gui_plugin_manage_build_screen(void);

/* Builds/tears down this module's one screen -- called alongside
 * gui_settings_init()/gui_settings_teardown() (gui.c, gui_reload.c), same
 * lifecycle as every other settings sub-screen. */
void gui_plugin_manage_init(void);
void gui_plugin_manage_teardown(void);

/* Reached from a "Plugins" row on Settings -> System. */
void gui_plugin_manage_row_cb(lv_event_t * e);

#endif /* GUI_PLUGIN_MANAGE_H */
