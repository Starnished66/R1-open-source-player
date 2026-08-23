#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#define PLUGIN_API_VERSION 1
#define PLUGIN_LIST_SCREEN_POOL_SIZE 4

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

/* Same shape and reasoning as PLUGIN_MAX_DISPLAY_LIST_ITEMS above, for
 * plugin.register_list_item("playback", ...) -- sizes plugin_manager.c's own
 * internal plugin_playback_list_items[] array, and gui.c's build_settings_
 * playback_screen() sizes its own static items[] array off this (6 built-in
 * rows -- "Car Mode", "Crossfade", "Equalizer", "Resume Last Track", "Sleep
 * Timer", "Startup Volume" -- plus this many plugin rows). */
#define PLUGIN_MAX_PLAYBACK_LIST_ITEMS 8

/* Same shape and reasoning as PLUGIN_MAX_DISPLAY_LIST_ITEMS above, for
 * plugin.register_list_item("power", ...) -- sizes plugin_manager.c's own
 * internal plugin_power_list_items[] array, and gui.c's build_settings_
 * power_screen() sizes its own static items[] array off this. */
#define PLUGIN_MAX_POWER_LIST_ITEMS 8

/* Same shape and reasoning as PLUGIN_MAX_DISPLAY_LIST_ITEMS above, for
 * plugin.register_list_item("system", ...) -- sizes plugin_manager.c's own
 * internal plugin_system_list_items[] array, and gui.c's build_settings_
 * system_screen() sizes its own static items[] array off this. */
#define PLUGIN_MAX_SYSTEM_LIST_ITEMS 8

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

/* ---- plugin.show_settings_list() -- a second, separate screen pool from
 * plugin.show_list() (PLUGIN_LIST_SCREEN_POOL_SIZE, gui.c), for a plugin's
 * OWN nested settings submenu with real toggle/slider rows, not plain
 * tappable text. Shared between plugin_manager.c (per-slot Lua callback ref
 * storage, so a reused pool slot's stale refs get released rather than
 * leaked) and gui.c (the pool itself + its per-slot row state) -- see
 * gui.c's gui_plugin_show_settings_list() and plugin_manager.c's
 * l_plugin_show_settings_list(). ---- */

/* Row type tags, passed as plain ints across the gui.c/plugin_manager.c
 * boundary (parallel-array style, matching every other plugin.* bridge in
 * this codebase -- no shared struct type needed across the two headers). */
#define PLUGIN_SETTINGS_ROW_TAP    0 /* plain tap row -- on_select(), no args */
#define PLUGIN_SETTINGS_ROW_TOGGLE 1 /* on/off switch -- on_change(new_value: bool) */
#define PLUGIN_SETTINGS_ROW_SLIDER 2 /* on_change(new_value: number), fires on release only */

/* Two pool slots -- settings submenus nest shallower in practice than plain
 * list browsing (plugin.show_list()'s own pool is 4), so smaller with real
 * headroom under NAV_STACK_MAX. Sizes both gui.c's screen pool and
 * plugin_manager.c's per-slot callback-ref storage below. */
#define PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE 2

/* Upper bound on rows per plugin.show_settings_list() call -- silently
 * truncates past this, same convention plugin.show_list()'s own
 * PLUGIN_MAX_LIST_ITEMS cap uses. Sizes both plugin_manager.c's per-slot ref
 * storage and gui.c's per-slot row-state storage. */
#define PLUGIN_SETTINGS_LIST_MAX_ROWS 24

/* Upper bound on "slider"-type rows per call specifically -- separate from
 * PLUGIN_SETTINGS_LIST_MAX_ROWS because each slider row needs a real,
 * bounded slot in gui.c's swipe_dead_zones[] (see SWIPE_DEAD_ZONE_MAX's own
 * comment there): PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE pool slots x this
 * many sliders each is real, accounted-for headroom, not unbounded growth.
 * A slider row past this cap is silently dropped (not rendered at all),
 * same truncate-don't-error convention as the row cap above. */
#define PLUGIN_SETTINGS_LIST_MAX_SLIDERS 4

/* ---- plugin.on(event, callback) -- lets a plugin react to a playback
 * lifecycle change it didn't itself cause (a scrobbler needs to know when a
 * track starts, for instance), rather than only ever running in response to
 * a direct tap. Unlike plugin.register_list_item() (each plugin's row
 * coexists as its own list entry), an event has no UI real estate to divide
 * up -- every subscriber to a given event fires, so storage is a small
 * per-event array of Lua refs rather than one ref per plugin. See gui.c's
 * own comment on where each event actually fires (play_track_at_from()/
 * on_track_auto_advanced()/toggle_play_pause()/the six audio_stop() call
 * sites). ---- */

/* Upper bound on how many plugin.on() subscriptions any ONE event may have,
 * across every loaded plugin combined -- luaL_error()s past this, same
 * cap-and-fail-loudly convention as PLUGIN_MAX_STREAM_TILES. Four events
 * (track_started/paused/resumed/stopped) each get their own array sized off
 * this, in plugin_manager.c. */
#define PLUGIN_MAX_EVENT_SUBSCRIBERS 8

/* ---- plugin.set_interval(seconds, callback) / plugin.clear_interval(handle)
 * -- a generic repeating timer, needed so a plugin can periodically poll
 * plugin.get_position() (e.g. a scrobbler checking Last.fm's own "50% or 4
 * minutes played" threshold) without a native event to hang that logic off
 * of. Backed by gui.c's lv_timer_create(), the same mechanism
 * update_timer_cb() already uses for its own 500ms polling loop. ---- */

/* Fixed pool of concurrently-active intervals, across every loaded plugin
 * combined -- luaL_error()s a set_interval() call past this rather than
 * silently overwriting or refusing quietly. Sizes gui.c's own
 * plugin_interval_timers[] array too. */
#define PLUGIN_MAX_INTERVALS 8

/* Minimum enforced period for plugin.set_interval() -- a request for less
 * than this is silently clamped up to it (not an error: asking for 100ms
 * isn't misusing the API, just asking for more than makes sense), so one
 * misbehaving plugin can't flood the main UI thread with timer callbacks. */
#define PLUGIN_INTERVAL_MIN_MS 1000

/* Scans <SD card>/.plugins/ for .lua files and loads each one, discovering
 * the rows/tiles they register during load. Should run before anything
 * might dispatch a click (gui_init() calls it early, well before either
 * the Books or Stream Media screen could plausibly be reached). A missing
 * or empty .plugins folder is not an error; a script that fails to
 * load/run is skipped (logged to stderr) without aborting the others. */
void plugin_manager_init(void);
void plugin_manager_poll(void);

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

/* Reads back row `index`'s optional icon/height/width/text_size (plugin.
 * register_list_item()'s 4th `options` arg, PLUGINS.md) -- *out_icon and
 * *out_text_size are set to NULL, and both numeric outputs to 0, when index is out of
 * range or that field was never set, matching "unset" all the way through
 * to screen_builders.c's pill_row_apply_icon()/pill_row_resolve_text_size()
 * (both already treat NULL/0 as "default, don't touch today's look"). One
 * combined accessor per list_id family (not three separate ones) -- same
 * data, just avoids tripling the already-6-way repetition of this family's
 * own count/label/clicked trio. */
void plugin_manager_get_books_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                 int32_t * out_width, const char ** out_text_size);

/* Same shape as the plugin_manager_*_books_list_item_* family above, for
 * plugin.register_list_item("settings", ...) -- gui.c's build_settings_
 * screen() appends these after its own 5 built-in category rows. */
int plugin_manager_get_settings_list_item_count(void);
const char * plugin_manager_get_settings_list_item_label(int index);
void plugin_manager_settings_list_item_clicked(int index);
void plugin_manager_get_settings_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                    int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("display", ...) -- gui.c's
 * build_settings_display_screen() appends these after its own 4 built-in
 * rows. */
int plugin_manager_get_display_list_item_count(void);
const char * plugin_manager_get_display_list_item_label(int index);
void plugin_manager_display_list_item_clicked(int index);
void plugin_manager_get_display_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                   int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("playback", ...) -- gui.c's
 * build_settings_playback_screen() appends these after its own 6 built-in
 * rows. */
int plugin_manager_get_playback_list_item_count(void);
const char * plugin_manager_get_playback_list_item_label(int index);
void plugin_manager_playback_list_item_clicked(int index);
void plugin_manager_get_playback_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                    int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("power", ...) -- gui.c's
 * build_settings_power_screen() appends these after its own built-in rows. */
int plugin_manager_get_power_list_item_count(void);
const char * plugin_manager_get_power_list_item_label(int index);
void plugin_manager_power_list_item_clicked(int index);
void plugin_manager_get_power_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                 int32_t * out_width, const char ** out_text_size);

/* Same shape again, for plugin.register_list_item("system", ...) -- gui.c's
 * build_settings_system_screen() appends these after its own built-in rows. */
int plugin_manager_get_system_list_item_count(void);
const char * plugin_manager_get_system_list_item_label(int index);
void plugin_manager_system_list_item_clicked(int index);
void plugin_manager_get_system_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                  int32_t * out_width, const char ** out_text_size);

/* Invoked by gui.c's plugin-list-screen row click handler when row `index`
 * (into whatever items table the most recent plugin.show_list() call
 * passed) is tapped -- calls back into that call's on_select Lua function
 * with a 1-based Lua index. No-op if no plugin.show_list() call is still
 * "current" (i.e. none has ever been made yet). */
void plugin_manager_list_item_selected(int slot, int index);

/* Same shape as the plugin_manager_get_tile_* / plugin_manager_tile_clicked
 * family above, but for the separate plugin.register_stream_media_tile()
 * registry -- gui.c's build_stream_media_screen() reads these to append
 * plugin-registered tiles after the built-in Subsonic one. */
int plugin_manager_get_stream_tile_count(void);
const char * plugin_manager_get_stream_tile_label(int index);
const char * plugin_manager_get_stream_tile_icon(int index);
const char * plugin_manager_get_stream_tile_icon_selected(int index);
void plugin_manager_stream_tile_clicked(int index);

/* Backing plugin.set_screen_layout(screen_id, ...) and (as pure sugar,
 * screen_id pinned to "home") plugin.set_home_layout() -- every top-level
 * native screen sharing Home's own shape (a small, fixed, string-keyed set
 * of nav tiles/rows) reads these through the same generic family, keyed by
 * its own screen_id ("home"/"music"/"wireless"/"stream_media"/"settings"/
 * "books"/"dac"). 0 from the count means no plugin called set_screen_layout()
 * for that screen_id, i.e. "use the native default order/set" -- see
 * gui.c's own per-screen native-defs tables for each screen_id's full key
 * list. An unknown screen_id (or one this file's own plugin_screen_defs[]
 * doesn't know) is treated the same as "0 items" by every accessor below,
 * never a crash. */
int plugin_manager_get_screen_item_count(const char * screen_id);
const char * plugin_manager_get_screen_item_key(const char * screen_id, int index);

/* true = show this screen as a plain vertical list (build_pill_list_screen())
 * instead of the icon grid (build_icon_grid_screen()) -- set_screen_layout()'s/
 * set_home_layout()'s optional `{ mode = "list" }`. Only meaningful for the
 * icon-grid-native screens (home/music/wireless/stream_media); always true
 * for the pill-list-native ones (settings/books/dac), which have no tile
 * grid to switch to. Defaults to false (today's icon grid) where relevant. */
bool plugin_manager_get_screen_list_mode(const char * screen_id);

/* Per-item style overrides from set_screen_layout()'s/set_home_layout()'s
 * optional per-item config table ({ key = "...", bg_color = 0xRRGGBB,
 * text_color = 0xRRGGBB, radius = n, bg_alpha = n }, PLUGINS.md) -- each
 * returns false (leaving *out untouched) if item `index` on screen_id has
 * no override for that property, matching every other "false means nothing
 * to apply" accessor in this file. bg_alpha (0-255, lv_opa_t scale) lets a
 * tile/row go semi-transparent over plugin.set_background_image()'s
 * whole-screen wallpaper. */
bool plugin_manager_get_screen_item_bg_color(const char * screen_id, int index, uint32_t * out_rgb);
bool plugin_manager_get_screen_item_text_color(const char * screen_id, int index, uint32_t * out_rgb);
bool plugin_manager_get_screen_item_radius(const char * screen_id, int index, int32_t * out_radius);
bool plugin_manager_get_screen_item_bg_alpha(const char * screen_id, int index, uint8_t * out_alpha);

/* Same "false means nothing to apply" shape, for an optional border
 * (`border_color = 0xRRGGBB, border_width = n`, px, clamped to
 * 0..PLUGIN_SCREEN_BORDER_WIDTH_MAX) -- works in both tile and list mode,
 * not list-mode-only like the block below. A border_width of 0 (or the
 * field left out) never sets has_border, since a 0-width border is
 * invisible either way. */
bool plugin_manager_get_screen_item_border(const char * screen_id, int index, uint32_t * out_color,
                                            int32_t * out_width);

/* options.item_keys[i].offset_x -- signed px, -150..150 (see
 * PLUGIN_SCREEN_OFFSET_X_MAX, plugin_manager.c). A pure visual reposition
 * of the tile/row from its normal centered position, e.g. for an
 * alternating zig-zag list -- see PLUGIN_STYLING.md. Returns false (no
 * override) if this item never set it. */
bool plugin_manager_get_screen_item_offset_x(const char * screen_id, int index, int32_t * out_offset_x);

/* List-mode-only per-item extensions (set_screen_layout()'s/set_home_
 * layout()'s per-item config table gains `height`, `width`, `align`,
 * `accessory`, `text_size`, `icon`, PLUGINS.md) -- meaningless in tile
 * mode, so an icon-grid-native screen only reads these once
 * plugin_manager_get_screen_list_mode() is true for it; a pill-list-native
 * screen (settings/books/dac) always reads them, since it's always in list
 * shape. Same "false/NULL means nothing to apply, caller keeps its own
 * default" shape as the four accessors above, except plugin_manager_get_
 * screen_item_accessory()/_show_icon() (no natural "unset" value -- both
 * default true, matching today's always-shown chevron/icon). */
bool plugin_manager_get_screen_item_row_height(const char * screen_id, int index, int32_t * out_height);
bool plugin_manager_get_screen_item_row_width(const char * screen_id, int index, int32_t * out_width);
const char * plugin_manager_get_screen_item_text_align(const char * screen_id, int index); /* NULL/"left" default */
bool plugin_manager_get_screen_item_accessory(const char * screen_id, int index);          /* true = chevron shown */
const char * plugin_manager_get_screen_item_text_size(const char * screen_id, int index);  /* NULL = native default */
bool plugin_manager_get_screen_item_show_icon(const char * screen_id, int index);          /* true = launcher icon shown
                                                                                              * (and its label indent reserved) */

/* options.row_gap -- vertical px between rows. Defaults to 6,
 * build_pill_list_screen()'s own pre-existing hardcoded value, so a plugin
 * that never sets this changes nothing. Applies whenever screen_id ends up
 * rendered as a list -- always for the pill-list-native screens, only in
 * `{ mode = "list" }` for the icon-grid-native ones. */
int32_t plugin_manager_get_screen_row_gap(const char * screen_id);

/* options.tile_gap (tile mode only) -- px of visible space between adjacent
 * tiles. Defaults to 0, build_icon_grid_screen()'s own pre-existing
 * flush-cell look, so a plugin that never sets this changes nothing.
 * Meaningless (silently unused) for the pill-list-native screens. */
int32_t plugin_manager_get_screen_tile_gap(const char * screen_id);

/* options.title_align/title_size/title_underline -- styles the screen's own
 * title label (build_title(), screen_builders.c), separate from any per-item
 * key in item_keys. NULL (align/size) or false (underline) = today's exact
 * default (left-aligned, ui_size_28, no underline) -- see
 * screen_title_style_t's own doc comment in screen_builders.h for how these
 * reach the label. */
const char * plugin_manager_get_screen_title_align(const char * screen_id);
const char * plugin_manager_get_screen_title_size(const char * screen_id);
bool plugin_manager_get_screen_title_underline(const char * screen_id);

/* options.title_color -- 0xRRGGBB, a per-screen override of the title
 * label's own text color, separate from set_text_color("primary", ...)'s
 * single app-wide slot. Returns false (no override) if this screen never
 * set it. */
bool plugin_manager_get_screen_title_color(const char * screen_id, uint32_t * out_color);

/* options.title_gap -- px between the title band and the first tile/row
 * below it. Returns false (no override) if this screen never set it. */
bool plugin_manager_get_screen_title_gap(const char * screen_id, int32_t * out_gap);

/* Invoked by gui.c's plugin-settings-list row widgets (see
 * gui_plugin_show_settings_list()) when a "row"-type row in pool slot `slot`
 * at position `row` is tapped -- calls that row's own on_select Lua function
 * with no arguments. Unlike plugin_manager_list_item_selected() above (one
 * shared "current" ref for plugin.show_list()), this looks up a ref stored
 * per (slot, row) -- see plugin_manager.c's own plugin_settings_list_rows[]
 * comment for why. No-op if slot/row is out of range or was never
 * populated. */
void plugin_manager_settings_list_row_selected(int slot, int row);

/* Same shape, for a "toggle"-type row -- calls its on_change(new_value)
 * with the toggle's new boolean state. */
void plugin_manager_settings_list_toggled(int slot, int row, bool new_value);

/* Same shape, for a "slider"-type row -- calls its on_change(new_value) with
 * the slider's new integer value. Fired once on release, not on every drag
 * tick -- see gui.c's plugin_settings_slider_event_cb()'s own comment. */
void plugin_manager_settings_list_slid(int slot, int row, int new_value);

/* ---- Dispatchers for plugin.on() event subscribers -- called from gui.c
 * at the exact points each event actually happens (see plugin_manager.h's
 * own PLUGIN_MAX_EVENT_SUBSCRIBERS comment above for the hook-point list).
 * Each loops every plugin currently subscribed to that event and
 * lua_pcall()s it -- a no-op if nothing is subscribed. ---- */
void plugin_manager_notify_track_started(const char * title, const char * artist, const char * album,
                                          double duration_seconds);
void plugin_manager_notify_paused(void);
void plugin_manager_notify_resumed(void);
void plugin_manager_notify_stopped(void);

/* Invoked by gui.c's shared plugin-interval lv_timer callback when the
 * timer for pool slot `slot` (plugin_interval_timers[], gui.c) fires --
 * looks up that slot's stored Lua ref and calls it with no arguments. */
void plugin_manager_interval_fired(int slot);

/* Invoked by gui.c's plugin_text_entry_done_cb() when the single pending
 * plugin.show_text_input() call is submitted (T9 keypad Enter) -- calls its
 * on_submit(text) once, then clears the pending ref (one-shot, matching
 * show_text_entry()'s own "never fires on cancel" semantics -- see
 * plugin_manager.c's l_plugin_show_text_input() for the caveat this
 * inherits from the native singleton screen). */
void plugin_manager_text_input_submitted(const char * text);
void plugin_manager_text_input_cancelled(void);

#endif /* PLUGIN_MANAGER_H */
