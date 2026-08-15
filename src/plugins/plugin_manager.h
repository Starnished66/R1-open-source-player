#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

/* Third-party Lua plugin support. Every *.lua file under
 * <SD card>/.plugins/ is loaded into its own lua_State at startup, with a
 * small C API (the `plugin` global table, see plugin_manager.c's own
 * comment above register_plugin_api()) exposed for registering a tile
 * (currently surfaced via the native Books -> "Audio Books" row, not its
 * own Home-screen slot -- see PLUGIN_MAX_TILES below), showing a simple
 * list screen, browsing the SD card, and driving playback -- so a plugin
 * is a plain text script, not compiled code. Each lua_State stays open
 * for the app's entire lifetime (not closed after the initial load) since
 * a plugin's registered callbacks (on_open, on_select, ...) are Lua
 * closures that need their owning state alive to be called later. */

/* Upper bound on how many tiles all plugins combined may register via
 * plugin.register_tile() -- sizes plugin_manager.c's own internal
 * plugin_tiles[] array. Registered tiles aren't rendered into their own
 * Home-screen grid slot (the Home icon grid can't be scrolled, so
 * anything past its fixed 6 built-in tiles would be unreachable) --
 * gui.c currently routes its native Books -> "Audio Books" row to tile 0
 * (see audio_books_row_cb()) rather than listing tiles anywhere itself. */
#define PLUGIN_MAX_TILES 16

/* Upper bound on how many tiles all plugins combined may register via
 * plugin.register_stream_media_tile() -- a SEPARATE registry from
 * plugin_tiles[]/PLUGIN_MAX_TILES above (different Lua function, different
 * surface). Stream Media has exactly one built-in tile (Subsonic) after
 * this session's Qobuz/Tidal/Net Radio cleanup, so unlike Home (already
 * full at 6, can't scroll) it has real room -- capped at 5 to keep the
 * total at 6, the same "fills exactly 3 rows, no scroll" ceiling proven
 * out for Home. gui.c's build_stream_media_screen() sizes its own static
 * items[] array off this (1 built-in + this many plugin tiles), same
 * reasoning as PLUGIN_MAX_TILES's own comment on icon_grid_item_t having
 * no runtime-append API. */
#define PLUGIN_MAX_STREAM_TILES 5

/* Scans <SD card>/.plugins/ for .lua files and loads each one, discovering
 * the tiles they register via plugin.register_tile() during load. Should
 * run before anything might dispatch a tile tap (gui_init() calls it early,
 * well before the Books screen -- the only current tile entry point --
 * could plausibly be reached). A missing or empty .plugins folder is not an
 * error; a script that fails to load/run is skipped (logged to stderr)
 * without aborting the others. */
void plugin_manager_init(void);

int plugin_manager_get_tile_count(void);
const char * plugin_manager_get_tile_label(int index);
const char * plugin_manager_get_tile_icon(int index);          /* theme2-relative, e.g. "launcher/book.png" */
const char * plugin_manager_get_tile_icon_selected(int index); /* the "_s" variant */

/* Calls back into tile `index`'s (into the plugin_manager_get_tile_*
 * arrays above) on_open Lua function -- gui.c's audio_books_row_cb() is
 * the current caller, invoking tile 0 when the Books -> "Audio Books" row
 * is tapped. */
void plugin_manager_tile_clicked(int index);

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
