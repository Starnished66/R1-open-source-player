#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

/* Third-party Lua plugin support. Every *.lua file under
 * <SD card>/.plugins/ is loaded into its own lua_State at startup, with a
 * small C API (the `plugin` global table, see plugin_manager.c's own
 * comment above register_plugin_api()) exposed for adding a row to an
 * existing list screen (see PLUGIN_MAX_BOOKS_LIST_ITEMS below), showing a
 * simple list screen, browsing the SD card, and driving playback -- so a
 * plugin is a plain text script, not compiled code. Each lua_State stays
 * open for the app's entire lifetime (not closed after the initial load)
 * since a plugin's registered callbacks (on_open, on_select, ...) are Lua
 * closures that need their owning state alive to be called later. */

/* Upper bound on how many rows all plugins combined may register into the
 * Books screen's list via plugin.register_list_item("books", ...) -- sizes
 * plugin_manager.c's own internal plugin_books_list_items[] array, and
 * gui.c's build_books_screen() sizes its own static items[] array off this
 * (2 built-in rows -- "Books", "Favorites" -- plus this many plugin rows).
 * Unlike the icon-grid tile registries below, build_pill_list_screen()'s
 * rows genuinely scroll (real-device confirmed, screen_builders.h's own
 * comment), so there's no hard "fills the screen" ceiling forcing this
 * number down the way PLUGIN_MAX_STREAM_TILES's own comment describes --
 * 8 is just a generous, arbitrary round number, not a rendering limit. */
#define PLUGIN_MAX_BOOKS_LIST_ITEMS 8

/* Same shape and same reasoning as PLUGIN_MAX_BOOKS_LIST_ITEMS above, for
 * plugin.register_list_item("settings", ...) -- sizes plugin_manager.c's
 * own internal plugin_settings_list_items[] array, and gui.c's
 * build_settings_screen() sizes its own static items[] array off this (5
 * built-in category rows -- "Playback", "Display", "Power", "System",
 * "About" -- plus this many plugin rows). A separate array from the Books
 * one, not shared storage, per plugin_manager.c's own comment on why a new
 * list_id gets its own array rather than a fully generic dispatch table. */
#define PLUGIN_MAX_SETTINGS_LIST_ITEMS 8

/* Same shape and reasoning as PLUGIN_MAX_SETTINGS_LIST_ITEMS above, for
 * plugin.register_list_item("display", ...) -- sizes plugin_manager.c's own
 * internal plugin_display_list_items[] array, and gui.c's build_settings_
 * display_screen() sizes its own static items[] array off this (4 built-in
 * rows -- "Accent Color", "Font Size", "Screen Timeout", "Swipe Up for
 * Home" -- plus this many plugin rows). Exists as its own list_id (not
 * folded into "settings") so a plugin whose row belongs specifically under
 * Settings -> Display (e.g. a Theme picker) can land there instead of the
 * top-level Settings list. */
#define PLUGIN_MAX_DISPLAY_LIST_ITEMS 8

/* Upper bound on how many tiles all plugins combined may register via
 * plugin.register_stream_media_tile() -- sizes plugin_manager.c's own
 * internal plugin_stream_tiles[] array. Stream Media has exactly one
 * built-in tile (Subsonic) after this session's Qobuz/Tidal/Net Radio
 * cleanup, so unlike Home (already full at 6, can't scroll) it has real
 * room -- capped at 5 to keep the total at 6, the same "fills exactly 3
 * rows, no scroll" ceiling proven out for Home. gui.c's
 * build_stream_media_screen() sizes its own static items[] array off this
 * (1 built-in + this many plugin tiles) -- icon_grid_item_t has no
 * runtime-append API, so that array has to be sized up front. */
#define PLUGIN_MAX_STREAM_TILES 5

/* Scans <SD card>/.plugins/ for .lua files and loads each one, discovering
 * the rows/tiles they register during load. Should run before anything
 * might dispatch a click (gui_init() calls it early, well before either
 * the Books or Stream Media screen could plausibly be reached). A missing
 * or empty .plugins folder is not an error; a script that fails to
 * load/run is skipped (logged to stderr) without aborting the others. */
void plugin_manager_init(void);

/* Rows registered via plugin.register_list_item("books", label, on_open) --
 * gui.c's build_books_screen() appends these after its own 2 built-in rows
 * ("Books", "Favorites"). No icon: pill-list rows (screen_builders.h's
 * pill_list_item_t) don't have an icon slot at all, unlike the tile
 * registries below. */
int plugin_manager_get_books_list_item_count(void);
const char * plugin_manager_get_books_list_item_label(int index);

/* Calls back into books-list-item `index`'s on_open Lua function -- gui.c's
 * shared row click handler is the caller, with `index` matching a row's
 * position among plugin_manager_get_books_list_item_* above (i.e. already
 * offset past the 2 built-in rows by the caller). */
void plugin_manager_books_list_item_clicked(int index);

/* Same shape as the plugin_manager_*_books_list_item_* family above, for
 * plugin.register_list_item("settings", ...) -- gui.c's build_settings_
 * screen() appends these after its own 5 built-in category rows. */
int plugin_manager_get_settings_list_item_count(void);
const char * plugin_manager_get_settings_list_item_label(int index);
void plugin_manager_settings_list_item_clicked(int index);

/* Same shape again, for plugin.register_list_item("display", ...) -- gui.c's
 * build_settings_display_screen() appends these after its own 4 built-in
 * rows. */
int plugin_manager_get_display_list_item_count(void);
const char * plugin_manager_get_display_list_item_label(int index);
void plugin_manager_display_list_item_clicked(int index);

/* Invoked by gui.c's plugin-list-screen row click handler when row `index`
 * (into whatever items table the most recent plugin.show_list() call
 * passed) is tapped -- calls back into that call's on_select Lua function
 * with a 1-based Lua index. No-op if no plugin.show_list() call is still
 * "current" (i.e. none has ever been made yet). */
void plugin_manager_list_item_selected(int index);

/* Same shape as the plugin_manager_get_tile_* / plugin_manager_tile_clicked
 * family above, but for the separate plugin.register_stream_media_tile()
 * registry -- gui.c's build_stream_media_screen() reads these to append
 * plugin-registered tiles after the built-in Subsonic one. */
int plugin_manager_get_stream_tile_count(void);
const char * plugin_manager_get_stream_tile_label(int index);
const char * plugin_manager_get_stream_tile_icon(int index);
const char * plugin_manager_get_stream_tile_icon_selected(int index);
void plugin_manager_stream_tile_clicked(int index);

#endif /* PLUGIN_MANAGER_H */

