#include "gui_plugin_manage.h"
#include "screen_builders.h"
#include "gui_theme.h"
#include "gui_navigation.h"
#include "gui_reload.h"
#include "gui_notifications.h"
#include "plugin_manager.h"
#include "plugin_disabled_list.h"
#include "assets.h"

#include <stdint.h>
#include <stdbool.h>

/* A bound, not a promise -- plugin_manager_scan_available() truncates its
 * on-disk scan at this count. It guarantees every currently-LOADED plugin
 * (at most PLUGIN_MAX_FILES=16 of those) is always included even when
 * truncating, so only *disabled, never-loaded* files can ever be the ones
 * left off; still, no realistic .plugins folder approaches 64 files. */
#define PLUGIN_MANAGE_MAX_ROWS 64

static plugin_available_entry_t manage_entries[PLUGIN_MANAGE_MAX_ROWS];
static int manage_entry_count = 0;

static void plugin_manage_reload_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_reload_request();
}

/* Reuses the exact reload path plugin.reload_ui() already uses -- see
 * plugin_manager.c's own comments on why a full reload (rather than a
 * surgical single-plugin unload) is the correct default here: it's the
 * only mechanism that already knows how to reset the global "last plugin
 * wins" style/layout/EQ state cleanly. It does NOT undo a plugin's own
 * durable side effects made outside that state -- most notably
 * set_icon()'s files under /usr/data/theme_overrides/, which have no
 * per-plugin attribution and so no revert path (plugin_manager.c's own
 * comment on PLUGIN_THEME_OVERRIDE_ROOT). Disabling a theme plugin stops
 * its script from running again on the next reload; it does not, by
 * itself, remove icons that plugin already copied to disk. */
static void plugin_manage_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= manage_entry_count) return;
    lv_obj_t * toggle_img = lv_event_get_target(e);
    bool enabled_now = lv_obj_has_state(toggle_img, LV_STATE_CHECKED);

    if (plugin_disabled_list_set(manage_entries[index].filename, !enabled_now)) {
        gui_reload_request();
        return;
    }

    /* Persisting the change failed (full/removed SD card) -- don't reload
     * on a toggle that was never actually saved to disk, and put this
     * row's sprite/state back to the last known-good value rather than
     * leaving the UI showing a position that doesn't match what's on disk
     * (plugin_disabled_list_set() itself already rolled its own in-memory
     * state back on failure -- manage_entries[index].disabled still holds
     * that last known-good value). */
    bool restore_checked = !manage_entries[index].disabled;
    if (restore_checked) lv_obj_add_state(toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(toggle_img, LV_STATE_CHECKED);
    lv_image_set_src(toggle_img, asset_path(restore_checked ? "settings/on.png" : "settings/off.png"));
    show_info_toast("Couldn't save -- plugin change was not applied");
}

lv_obj_t * gui_plugin_manage_build_screen(void) {
    manage_entry_count = plugin_manager_scan_available(manage_entries, PLUGIN_MANAGE_MAX_ROWS);

    static pill_list_item_t items[1 + PLUGIN_MANAGE_MAX_ROWS];
    items[0] = (pill_list_item_t){ "Reload Plugins", PILL_ACCESSORY_NONE, false,
                                    plugin_manage_reload_row_cb, NULL, NULL };
    int count = 1;
    for (int i = 0; i < manage_entry_count; i++) {
        items[count++] = (pill_list_item_t){
            manage_entries[i].display_name, PILL_ACCESSORY_TOGGLE,
            !manage_entries[i].disabled, NULL, plugin_manage_toggle_cb,
            (void *) (intptr_t) i
        };
    }

    lv_obj_t * scr = build_pill_list_screen("Manage Plugins", generic_back_cb, items, count,
                                             gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * plugin_manage_screen;

void gui_plugin_manage_init(void) {
    plugin_manage_screen = gui_plugin_manage_build_screen();
}

void gui_plugin_manage_teardown(void) {
    if (plugin_manage_screen) {
        lv_obj_del(plugin_manage_screen);
        plugin_manage_screen = NULL;
    }
}

void gui_plugin_manage_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(plugin_manage_screen);
}
