#include "plugin_manager.h"
#include "gui.h"
#include "peq.h"
#include "http_client.h"
#include "mbedtls/md5.h" /* plugin.md5() -- same primitive subsonic_client.c already uses for its own token auth */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <limits.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point -- see gui.c's own MUSIC_ROOT_DIR comment; each
   * file that needs it defines its own copy rather than sharing a header,
   * matching gui.c/remote_control.c/metadata_db.c already doing the same. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"

  /* Same override root assets.c's own asset_path() checks (see its own
   * comment there) -- duplicated here rather than shared via a header,
   * same convention as MUSIC_ROOT_DIR above. Only defined on target:
   * asset_path() never checks any override root on HOST_BUILD, so
   * plugin.set_icon() is a no-op there (see its own comment below) rather
   * than writing files nothing would ever read. */
  #define PLUGIN_THEME_OVERRIDE_ROOT "/usr/data/theme_overrides/"
#endif

#define PLUGIN_MAX_FILES 16
#define PLUGIN_MAX_LIST_ITEMS 500
#define PLUGIN_MAX_ASYNC_HTTP 4
#define PLUGIN_ASYNC_HTTP_DEFAULT_MAX (512U * 1024U)
#define PLUGIN_ASYNC_HTTP_MAX_RESPONSE (2U * 1024U * 1024U)
#define PLUGIN_ASYNC_HTTP_MAX_REQUEST (1024U * 1024U)

typedef struct {
    lua_State * L;
    int open_ref; /* LUA_REGISTRYINDEX ref to the on_open function */
    char label[64];
    char icon[96];
    char icon_selected[96];
} plugin_tile_t;

/* Leaner sibling of plugin_tile_t for plugin.register_list_item() -- pill-
 * list rows (screen_builders.h's pill_list_item_t) now DO have an icon/
 * height/width/text_size slot (this session's row-layout work),
 * populated from this call's optional 4th `options` table -- see
 * append_list_item()'s own comment. Empty string ("") means "unset" for
 * icon_path/text_size, matching pill_list_item_t's own NULL convention one
 * level up (plugin_manager_get_*_list_item_options() below translates
 * empty-string back to NULL for its callers). */
typedef struct {
    lua_State * L;
    int open_ref;
    char label[64];
    char icon_path[256];
    int32_t row_height;
    int32_t row_width;
    char text_size[8];
} plugin_list_item_t;

typedef struct {
    lua_State * L;
    char id[64];
    char name[96];
    char version[32];
    bool defined;
} plugin_instance_t;

static plugin_instance_t plugin_instances[PLUGIN_MAX_FILES];
static int plugin_instance_count = 0;
static int loading_plugin_slot = -1;

/* Registry for plugin.register_list_item("books", ...) -- gui.c's
 * build_books_screen() appends these after its own 2 built-in rows. See
 * PLUGIN_MAX_BOOKS_LIST_ITEMS's own comment in plugin_manager.h. list_id is
 * validated against a fixed, small set of recognized strings ("books",
 * "settings", "display") rather than driving any real per-list-id
 * storage/dispatch here -- each recognized list_id gets its own array +
 * validation branch (see plugin_settings_list_items[]/plugin_display_list_
 * items[] right below), not a fully generic dispatch table built ahead of
 * actually needing one. */
static plugin_list_item_t plugin_books_list_items[PLUGIN_MAX_BOOKS_LIST_ITEMS];
static int plugin_books_list_item_count = 0;

/* Registry for plugin.register_list_item("settings", ...) -- gui.c's
 * build_settings_screen() appends these after its own 5 built-in category
 * rows. See PLUGIN_MAX_SETTINGS_LIST_ITEMS's own comment in
 * plugin_manager.h. */
static plugin_list_item_t plugin_settings_list_items[PLUGIN_MAX_SETTINGS_LIST_ITEMS];
static int plugin_settings_list_item_count = 0;

/* Registry for plugin.register_list_item("display", ...) -- gui.c's
 * build_settings_display_screen() appends these after its own 4 built-in
 * rows. See PLUGIN_MAX_DISPLAY_LIST_ITEMS's own comment in
 * plugin_manager.h. */
static plugin_list_item_t plugin_display_list_items[PLUGIN_MAX_DISPLAY_LIST_ITEMS];
static int plugin_display_list_item_count = 0;

/* Registry for plugin.register_list_item("playback", ...) -- gui.c's
 * build_settings_playback_screen() appends these after its own 6 built-in
 * rows. See PLUGIN_MAX_PLAYBACK_LIST_ITEMS's own comment in
 * plugin_manager.h. */
static plugin_list_item_t plugin_playback_list_items[PLUGIN_MAX_PLAYBACK_LIST_ITEMS];
static int plugin_playback_list_item_count = 0;

/* Registry for plugin.register_list_item("power", ...) -- gui.c's
 * build_settings_power_screen() appends these after its own built-in rows.
 * See PLUGIN_MAX_POWER_LIST_ITEMS's own comment in plugin_manager.h. */
static plugin_list_item_t plugin_power_list_items[PLUGIN_MAX_POWER_LIST_ITEMS];
static int plugin_power_list_item_count = 0;

/* Registry for plugin.register_list_item("system", ...) -- gui.c's
 * build_settings_system_screen() appends these after its own built-in rows.
 * See PLUGIN_MAX_SYSTEM_LIST_ITEMS's own comment in plugin_manager.h. */
static plugin_list_item_t plugin_system_list_items[PLUGIN_MAX_SYSTEM_LIST_ITEMS];
static int plugin_system_list_item_count = 0;

/* Separate registry for plugin.register_stream_media_tile() -- same
 * plugin_tile_t shape, different surface (gui.c's Stream Media screen).
 * See PLUGIN_MAX_STREAM_TILES's own comment in plugin_manager.h for why
 * Stream Media gets real tiles when Home doesn't. */
static plugin_tile_t plugin_stream_tiles[PLUGIN_MAX_STREAM_TILES];
static int plugin_stream_tile_count = 0;

/* Each reusable plugin.show_list() screen owns its on_select callback. This
 * matters after Back returns from a nested list: the older screen must route
 * through its original callback, not whichever list was opened most recently. */
typedef struct {
    lua_State * L;
    int select_ref;
} plugin_list_callback_t;

static plugin_list_callback_t plugin_list_callbacks[PLUGIN_LIST_SCREEN_POOL_SIZE];

/* Per-(pool slot, row) Lua callback ref storage for plugin.show_settings_list()
 * -- a settings-list screen carries per-row toggle/slider state that
 * needs to survive being navigated away from and back to, so storage is
 * keyed by the actual gui.c pool slot instead of "whichever call was most
 * recent". L is NULL for a row that's never been populated (both the
 * static-array initial state, and after being unref'd when its slot is
 * reused for a shorter call) -- used as l_plugin_show_settings_list()'s own
 * guard for "does this row need unref'ing before I overwrite it". */
typedef struct {
    lua_State * L;
    int callback_ref; /* on_select for a "row" row, on_change for "toggle"/"slider" */
} plugin_settings_list_row_ref_t;

static plugin_settings_list_row_ref_t
    plugin_settings_list_rows[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE][PLUGIN_SETTINGS_LIST_MAX_ROWS];
static int plugin_settings_list_row_counts[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];

/* ---- plugin.* C API, exposed as a global table `plugin` in every loaded
 * script's own lua_State (see register_plugin_api() below):
 *
 *   plugin.register_list_item(list_id, label, on_open)
 *     Adds a row labeled `label` to an existing native list screen,
 *     identified by `list_id` -- "books" (gui.c's Books screen, appended
 *     after its own "Books"/"Favorites" rows), "settings" (gui.c's
 *     Settings screen, appended after its own "Playback"/"Display"/
 *     "Power"/"System"/"About" rows), "display" (gui.c's Settings ->
 *     Display sub-screen, appended after its own "Accent Color"/"Font
 *     Size"/"Screen Timeout"/"Swipe Up for Home" rows), "playback" (Settings
 *     -> Playback, after "Car Mode"/"Crossfade"/"Equalizer"/"Resume Last
 *     Track"/"Sleep Timer"/"Startup Volume"), "power" (Settings -> Power),
 *     or "system" (Settings -> System); see PLUGIN_MAX_BOOKS_LIST_ITEMS,
 *     PLUGIN_MAX_SETTINGS_LIST_ITEMS, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
 *     PLUGIN_MAX_PLAYBACK_LIST_ITEMS, PLUGIN_MAX_POWER_LIST_ITEMS, and
 *     PLUGIN_MAX_SYSTEM_LIST_ITEMS in plugin_manager.h for the per-screen
 *     caps. Every plugin that calls this gets its own row (not just the
 *     first, unlike the old register_tile()/"Audio Books" row this
 *     replaced) -- tapping it calls on_open() with no arguments.
 *
 *     Optional 4th arg `options`: { icon = "...", height = n, text_size =
 *     "small"/"medium"/"large" } -- icon is a raw absolute filesystem path
 *     (e.g. plugin.sd_root() .. "/icon.png", NOT a theme2-relative one);
 *     height (px) resizes the row (clamped, see PILL_ROW_HEIGHT_MIN/MAX,
 *     screen_builders.h); text_size picks the row's font. All three default
 *     to sensible values matching this row's own pre-existing look if
 *     omitted -- see PLUGINS.md for the full picture.
 *
 *   plugin.show_list(title, items, on_select [, options])
 *     Opens a list screen titled `title`. Each `items` entry is either a
 *     plain string (a row with just a label) or a table { label = "...",
 *     icon = "...", text_size = "..." } for a row with its own icon/text
 *     size (same `icon`/`text_size` meaning as register_list_item()'s
 *     `options` above). on_select(index): called with the 1-based Lua index
 *     of the tapped row. Optional 4th arg `options`: { height = n } --
 *     applies to every row in this call (not per-row).
 *
 *   plugin.show_settings_list(title, items)
 *     Opens a nested settings submenu titled `title`, indistinguishable
 *     from a native Settings screen -- unlike show_list() above, each row
 *     carries a real accessory and its own callback rather than one shared
 *     on_select(index). Each entry in `items` is a table: { type = "row",
 *     label = "...", on_select = function() ... end } for a plain tap (an
 *     on_select that itself calls show_settings_list() again is how a
 *     submenu nests inside a submenu); { type = "toggle", label = "...",
 *     value = true/false, on_change = function(new_value) ... end } for an
 *     on/off switch; or { type = "slider", label = "...", min = n, max = n,
 *     value = n, on_change = function(new_value) ... end } for a slider --
 *     on_change fires once when the drag is released, not on every
 *     intermediate tick. Every row type also accepts `icon`/`text_size`
 *     (same meaning as register_list_item()'s `options` above); `height`
 *     too, except for a "slider" row (its own fixed-layout card has no
 *     spare room to grow into). Capped at 24 rows and 4 slider rows per
 *     call, silently truncated past that (same convention as show_list()'s
 *     own item cap) -- an unknown/missing `type`, an unrecognized
 *     `text_size`, or a row missing its required callback, all raise a Lua
 *     error instead.
 *
 *   plugin.list_dir(path)
 *     Returns an array table of { name = "...", dir = true/false } for
 *     each non-hidden entry directly under the absolute path `path`.
 *
 *   plugin.sd_root()
 *     Returns the SD card's absolute mount path, so a script can build
 *     paths under it (e.g. plugin.sd_root() .. "/Audiobooks").
 *
 *   plugin.play_file(path)
 *     Plays a single file as a fresh one-song playlist.
 *
 *   plugin.play_list(paths [, start_index])
 *     Plays `paths` (an array table of file paths) as a fresh playlist,
 *     starting at the 1-based `start_index` (default 1).
 *
 *   plugin.show_toast(message)
 *     Shows the same transient toast used elsewhere in the app.
 *
 *   plugin.register_stream_media_tile(label, on_open [, icon])
 *     Appended as a real icon-grid tile in gui.c's Stream Media screen
 *     (after the built-in Subsonic tile) -- for plugins that thematically
 *     belong there (a Net Radio source, or similar) rather than in the
 *     Books list. Up to PLUGIN_MAX_STREAM_TILES (plugin_manager.h) across
 *     all loaded plugins combined. icon defaults to stream_media/radio.png
 *     if omitted.
 *
 *   plugin.set_icon(theme2_relative_path, source_file_path)
 *     Reskins an EXISTING tile's icon in place (e.g. "launcher/book.png"
 *     for Home's Books tile) by copying source_file_path's bytes into the
 *     app's writable theme-override directory. Must be called from a
 *     plugin's own top-level script code (i.e. during load) -- see
 *     l_plugin_set_icon()'s own comment for why a later, mid-session call
 *     silently won't update an already-shown icon.
 *
 *   plugin.set_home_layout(tile_keys [, options])
 *     Reorders/hides Home's 6 native tiles -- tile_keys is an array of any
 *     subset of "music", "stream_media", "wireless", "books", "system",
 *     "dac", in the order they should appear; any key left out is hidden.
 *     Each entry is a plain string, or a table { key = "...", bg_color =
 *     0xRRGGBB, text_color = 0xRRGGBB, radius = n } to also override that
 *     one tile's background color/text color/corner radius (radius
 *     clamped to 0..64). options.mode is "tile" (default, today's icon
 *     grid) or "list" (a plain vertical list instead). Same load-time-only
 *     constraint as set_icon() above. Raises a Lua error on an empty
 *     array, an unknown/duplicate tile key, or an invalid mode.
 *
 *   plugin.set_background_color(slot, rgb)
 *     Sets one of three background-color slots app-wide, live, no restart
 *     needed: "screen", "card", or "list_row" -- see
 *     l_plugin_set_background_color()'s own comment for exactly what each
 *     covers. rgb is a packed 0xRRGGBB integer, e.g. 0x1E1E22.
 *
 *   plugin.set_text_color(slot, rgb)
 *     Sets one of two text-color slots app-wide, live, no restart needed --
 *     "primary" (dominant near-white text) or "muted" (secondary/disabled-
 *     ish gray text). Destructive-red and accent-tinted text are not
 *     covered -- see l_plugin_set_text_color()'s own comment.
 *
 *   plugin.eq_load_profile(path) -> bool
 *     Loads and applies a .peq profile file (src/audio/peq.c's own
 *     save/load format) from an arbitrary path, then persists it as the
 *     new always-current EQ state. Returns false (not a Lua error) if path
 *     doesn't exist or isn't readable.
 *
 *   plugin.eq_save_profile(path) -> bool
 *     Saves the current EQ state (bypass, preamp, all 10 bands) to path in
 *     the same format. Returns false on write failure.
 *
 *   plugin.eq_reset()
 *     Restores every band, the preamp, and bypass to peq.c's own built-in
 *     defaults (same as the native EQ screen's Reset button).
 *
 *   plugin.eq_set_bypass(enabled)
 *   plugin.eq_set_preamp(db)
 *     Toggles/sets the whole-EQ bypass and pre-amp (dB).
 *
 *   plugin.eq_set_band(index, freq_hz, gain_db, q)
 *   plugin.eq_set_band_type(index, type)
 *   plugin.eq_set_band_enabled(index, enabled)
 *     Per-band controls. index is 1-based (1..10, matching every other
 *     1-based index in this API). type is "peaking", "low_shelf", or
 *     "high_shelf".
 *
 *   plugin.toggle_pause()
 *   plugin.stop()
 *   plugin.next_track()
 *   plugin.prev_track()
 *   plugin.seek(seconds)
 *   plugin.set_volume(percent)
 *   plugin.is_playing() -> bool
 *   plugin.is_paused() -> bool
 *   plugin.get_position() -> seconds
 *   plugin.get_duration() -> seconds
 *     Playback control/query, bridged through gui.c so a plugin-driven
 *     change stays in sync with the play/pause icon, volume slider/popup,
 *     and shuffle-aware next/prev stepping the native UI itself uses -- see
 *     gui.h's own comment on gui_plugin_toggle_pause() and neighbors.
 *     percent is 0..100, clamped.
 *
 *   plugin.http_get(url [, verify_tls]) -> status, body
 *     Synchronous GET request (src/network/http_client.c). verify_tls
 *     defaults to true. Runs on the calling thread -- i.e. whatever plugin
 *     callback invoked it, always the main UI thread -- so a slow/hanging
 *     server blocks the UI until the request completes; keep calls fast or
 *     user-initiated. Returns nil, "network error" on a DNS/connect/TLS
 *     failure; a real HTTP error status (404, 500, ...) still returns
 *     normally with that status and whatever body the server sent.
 *
 *   plugin.http_post(url, body [, content_type] [, verify_tls]) -> status, body
 *     Same contract/blocking caveat as http_get() above, for a POST.
 *     content_type defaults to "application/x-www-form-urlencoded" if
 *     omitted/nil.
 *
 *   plugin.md5(text) -> lowercase hex string
 *     MD5 of text -- for API signing schemes that need it (e.g. Last.fm's
 *     own api_sig).
 *
 *   plugin.show_text_input(title, initial_text, is_password, on_submit)
 *     Opens the app's own T9 keypad text-entry screen. on_submit(text) fires
 *     only on an actual submit (T9 Enter) -- backing out via the screen's
 *     own back button never calls it at all. This is a true SINGLETON
 *     screen (same one Wi-Fi password entry/Subsonic login use): calling it
 *     again before a pending call resolves silently replaces the pending
 *     callback -- fine for one plugin chaining calls (ask username, then
 *     password), not safe against two unrelated calls racing.
 *
 *   plugin.get_now_playing() -> title, artist, album, duration_seconds, or a
 *   single nil if nothing is loaded
 *     Pull accessor for whatever's currently loaded -- same metadata a
 *     "track_started" event handler already receives as arguments, for code
 *     that isn't reacting to that event directly.
 *
 *   plugin.on(event, callback)
 *     Subscribes to a playback lifecycle event -- every subscriber fires,
 *     across every loaded plugin (unlike register_list_item(), an event
 *     isn't UI real estate to divide up). Four events:
 *       "track_started" -- callback(title, artist, album, duration_seconds),
 *         fires whenever a new track begins playing, whatever the cause
 *         (manual tap, next/prev, gapless/crossfade auto-advance, a
 *         plugin's own play_file/play_list, remote control).
 *       "paused" / "resumed" -- callback(), no arguments.
 *       "stopped" -- callback(), no arguments, fires when playback stops
 *         outright (not paused). Does NOT cover "playlist reaches its end
 *         with repeat off" (that path doesn't route through this app's own
 *         explicit stop call sites) -- a known, documented gap, not
 *         something every plugin needs.
 *     Unknown event name raises a Lua error immediately, same convention as
 *     an unknown list_id.
 *
 *   plugin.set_interval(seconds, callback) -> handle
 *   plugin.clear_interval(handle)
 *     Generic repeating timer (LVGL-backed, same mechanism this app's own
 *     500ms UI polling loop uses) -- e.g. for periodically checking
 *     get_position() against some threshold. seconds is clamped up to a
 *     1-second minimum if lower, silently (not an error). Errors if more
 *     than PLUGIN_MAX_INTERVALS (plugin_manager.h) are active at once,
 *     across every loaded plugin combined. A callback registered this way
 *     runs on the main UI thread exactly like every other plugin callback --
 *     a slow http_get()/http_post() inside one will visibly stall the UI
 *     until it returns, same tradeoff http_get() itself already documents.
 * ---- */

/* Used by l_plugin_register_stream_media_tile() -- fills t->icon/icon_selected
 * from an explicit icon string (deriving the "_s" selected-state variant
 * the same way every real icon pair in this app is named), or from
 * (default_icon, default_icon_selected) if icon is NULL. */
static void fill_tile_icon(plugin_tile_t * t, const char * icon, const char * default_icon,
                            const char * default_icon_selected) {
    if (icon) {
        char base[80];
        snprintf(base, sizeof(base), "%s", icon);
        char * dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(t->icon, sizeof(t->icon), "%s", icon);
        snprintf(t->icon_selected, sizeof(t->icon_selected), "%s_s.png", base);
    } else {
        snprintf(t->icon, sizeof(t->icon), "%s", default_icon);
        snprintf(t->icon_selected, sizeof(t->icon_selected), "%s", default_icon_selected);
    }
}

/* Validates a text_size string against the three recognized tiers -- shared
 * by append_list_item() below and l_plugin_show_settings_list()/
 * l_plugin_show_list() (fail loudly at the Lua boundary, same convention
 * every other unrecognized-string case in this file already uses; the
 * resolved font itself is picked later, in screen_builders.c's
 * pill_row_resolve_text_size(), which trusts this was already validated). */
static bool is_valid_text_size(const char * text_size) {
    return strcmp(text_size, "small") == 0 || strcmp(text_size, "medium") == 0 || strcmp(text_size, "large") == 0;
}

/* Shared by l_plugin_register_list_item() below, once its capacity check
 * for the target array has already passed -- pushes on_open (Lua stack
 * index 3 in the caller) into the registry and appends {L, ref, label}, plus
 * this call's optional 4th `options` table ({ icon, height, width, text_size },
 * PLUGINS.md) if present. icon_path/text_size are left as empty strings
 * (not touched at all here, matching the struct's already-zero-initialized
 * static-array storage) when `options` is absent or doesn't set that field
 * -- plugin_manager_get_*_list_item_options() below is what translates that
 * back to NULL for its own callers. */
static void append_list_item(plugin_list_item_t * array, int * count, lua_State * L, const char * label) {
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    plugin_list_item_t * item = &array[(*count)++];
    item->L = L;
    item->open_ref = ref;
    snprintf(item->label, sizeof(item->label), "%s", label);
    item->icon_path[0] = '\0';
    item->row_height = 0;
    item->row_width = 0;
    item->text_size[0] = '\0';

    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "icon");
        const char * icon = lua_tostring(L, -1);
        if (icon) snprintf(item->icon_path, sizeof(item->icon_path), "%s", icon);
        lua_pop(L, 1);

        lua_getfield(L, 4, "height");
        item->row_height = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, 4, "width");
        item->row_width = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, 4, "text_size");
        const char * text_size = lua_tostring(L, -1);
        if (text_size) {
            if (!is_valid_text_size(text_size)) {
                lua_pop(L, 1);
                luaL_error(L, "plugin.register_list_item: unknown text_size '%s' (expected \"small\", \"medium\", or \"large\")",
                           text_size);
            }
            snprintf(item->text_size, sizeof(item->text_size), "%s", text_size);
        }
        lua_pop(L, 1);
    }
}

/* Adds a row to an existing native list screen -- see PLUGIN_MAX_BOOKS_
 * LIST_ITEMS/PLUGIN_MAX_SETTINGS_LIST_ITEMS's own comments in plugin_
 * manager.h. list_id is checked against a small, fixed set of recognized
 * strings ("books", "settings") rather than accepted as-is: a typo'd or
 * unsupported list_id should fail loudly at plugin load time, not silently
 * register into nothing. */
static int l_plugin_register_list_item(lua_State * L) {
    const char * list_id = luaL_checkstring(L, 1);
    const char * label = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    if (strcmp(list_id, "books") == 0) {
        if (plugin_books_list_item_count >= PLUGIN_MAX_BOOKS_LIST_ITEMS) {
            return luaL_error(L, "plugin.register_list_item: too many items registered for \"books\" (max %d)",
                               PLUGIN_MAX_BOOKS_LIST_ITEMS);
        }
        append_list_item(plugin_books_list_items, &plugin_books_list_item_count, L, label);
    } else if (strcmp(list_id, "settings") == 0) {
        if (plugin_settings_list_item_count >= PLUGIN_MAX_SETTINGS_LIST_ITEMS) {
            return luaL_error(L, "plugin.register_list_item: too many items registered for \"settings\" (max %d)",
                               PLUGIN_MAX_SETTINGS_LIST_ITEMS);
        }
        append_list_item(plugin_settings_list_items, &plugin_settings_list_item_count, L, label);
    } else if (strcmp(list_id, "display") == 0) {
        if (plugin_display_list_item_count >= PLUGIN_MAX_DISPLAY_LIST_ITEMS) {
            return luaL_error(L, "plugin.register_list_item: too many items registered for \"display\" (max %d)",
                               PLUGIN_MAX_DISPLAY_LIST_ITEMS);
        }
        append_list_item(plugin_display_list_items, &plugin_display_list_item_count, L, label);
    } else if (strcmp(list_id, "playback") == 0) {
        if (plugin_playback_list_item_count >= PLUGIN_MAX_PLAYBACK_LIST_ITEMS) {
            return luaL_error(L, "plugin.register_list_item: too many items registered for \"playback\" (max %d)",
                               PLUGIN_MAX_PLAYBACK_LIST_ITEMS);
        }
        append_list_item(plugin_playback_list_items, &plugin_playback_list_item_count, L, label);
    } else if (strcmp(list_id, "power") == 0) {
        if (plugin_power_list_item_count >= PLUGIN_MAX_POWER_LIST_ITEMS) {
            return luaL_error(L, "plugin.register_list_item: too many items registered for \"power\" (max %d)",
                               PLUGIN_MAX_POWER_LIST_ITEMS);
        }
        append_list_item(plugin_power_list_items, &plugin_power_list_item_count, L, label);
    } else if (strcmp(list_id, "system") == 0) {
        if (plugin_system_list_item_count >= PLUGIN_MAX_SYSTEM_LIST_ITEMS) {
            return luaL_error(L, "plugin.register_list_item: too many items registered for \"system\" (max %d)",
                               PLUGIN_MAX_SYSTEM_LIST_ITEMS);
        }
        append_list_item(plugin_system_list_items, &plugin_system_list_item_count, L, label);
    } else {
        return luaL_error(L, "plugin.register_list_item: unknown list_id '%s' (expected \"books\", \"settings\", \"display\", \"playback\", \"power\", or \"system\")", list_id);
    }
    return 0;
}

/* Registers a Stream Media tile, appended after the built-in Subsonic one
 * -- see PLUGIN_MAX_STREAM_TILES's own comment in plugin_manager.h.
 * Default icon is stream_media/radio.png -- a real stock theme2 asset
 * (confirmed still present on disk; this session's earlier Qobuz/Tidal/Net
 * Radio cleanup only removed the dead *code* referencing it, not the
 * asset itself, which isn't this project's own to delete anyway), a
 * sensible default for the kind of plugin (a Net Radio-style streaming
 * source) this registry is meant for. */
static int l_plugin_register_stream_media_tile(lua_State * L) {
    const char * label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char * icon = (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? luaL_checkstring(L, 3) : NULL;

    if (plugin_stream_tile_count >= PLUGIN_MAX_STREAM_TILES) {
        return luaL_error(L, "too many stream media tiles registered (max %d)", PLUGIN_MAX_STREAM_TILES);
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    plugin_tile_t * t = &plugin_stream_tiles[plugin_stream_tile_count++];
    t->L = L;
    t->open_ref = ref;
    snprintf(t->label, sizeof(t->label), "%s", label);
    fill_tile_icon(t, icon, "stream_media/radio.png", "stream_media/radio_s.png");
    return 0;
}

/* Each items[] entry is either a plain string (today's original behavior,
 * unchanged) or a table { label = "...", icon = "...", text_size = "..." }
 * for a row that wants its own icon/text size -- see this function's own
 * doc comment. An optional trailing `options` table (4th arg) carries
 * `height` and `width`, applying to every row in this call (call-level,
 * setting -- a plain browsing list mixing wildly different row heights
 * would look broken in a way an occasional taller settings-submenu row
 * doesn't, see PLUGINS.md). */
static int l_plugin_show_list(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_Unsigned raw_n = lua_rawlen(L, 2);
    int n = (raw_n > (lua_Unsigned) PLUGIN_MAX_LIST_ITEMS) ? PLUGIN_MAX_LIST_ITEMS : (int) raw_n;

    static char label_bufs[PLUGIN_MAX_LIST_ITEMS][160];
    static const char * labels[PLUGIN_MAX_LIST_ITEMS];
    static char icon_bufs[PLUGIN_MAX_LIST_ITEMS][256];
    static const char * icon_paths[PLUGIN_MAX_LIST_ITEMS];
    static char text_size_bufs[PLUGIN_MAX_LIST_ITEMS][8];
    static const char * text_sizes[PLUGIN_MAX_LIST_ITEMS];

    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        icon_bufs[i][0] = '\0';
        icon_paths[i] = NULL;
        text_size_bufs[i][0] = '\0';
        text_sizes[i] = NULL;

        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "label");
            const char * s = lua_tostring(L, -1);
            snprintf(label_bufs[i], sizeof(label_bufs[i]), "%s", s ? s : "");
            lua_pop(L, 1);

            lua_getfield(L, -1, "icon");
            const char * icon = lua_tostring(L, -1);
            if (icon) {
                snprintf(icon_bufs[i], sizeof(icon_bufs[i]), "%s", icon);
                icon_paths[i] = icon_bufs[i];
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "text_size");
            const char * text_size = lua_tostring(L, -1);
            if (text_size) {
                if (!is_valid_text_size(text_size)) {
                    return luaL_error(L, "plugin.show_list: row %d has an unknown text_size '%s' (expected \"small\", \"medium\", or \"large\")",
                                       i + 1, text_size);
                }
                snprintf(text_size_bufs[i], sizeof(text_size_bufs[i]), "%s", text_size);
                text_sizes[i] = text_size_bufs[i];
            }
            lua_pop(L, 1);
        } else {
            const char * s = lua_tostring(L, -1);
            snprintf(label_bufs[i], sizeof(label_bufs[i]), "%s", s ? s : "");
        }
        labels[i] = label_bufs[i];
        lua_pop(L, 1);
    }

    int32_t height = 0;
    int32_t width = 0;
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "height");
        height = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, 4, "width");
        width = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
    }

    lua_pushvalue(L, 3);
    int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int slot = gui_plugin_show_list(title, labels, icon_paths, text_sizes, height, width, n);
    plugin_list_callback_t * cb = &plugin_list_callbacks[slot];
    if (cb->L && cb->select_ref != LUA_NOREF) luaL_unref(cb->L, LUA_REGISTRYINDEX, cb->select_ref);
    cb->L = L;
    cb->select_ref = new_ref;
    return 0;
}

/* plugin.show_settings_list(title, items) -- a nested settings submenu with
 * real toggle/slider rows, see this file's own top-of-file doc comment.
 * Each Lua item is a table: { type = "row"|"toggle"|"slider", label = ...,
 * on_select = fn }  (type == "row") or  { ..., value = bool, on_change = fn }
 * (type == "toggle")  or  { ..., min = n, max = n, value = n, on_change = fn }
 * (type == "slider"). A "row" whose on_select calls show_settings_list()
 * again is how a plugin nests a submenu inside a submenu -- same chaining
 * convention plugin.show_list() already supports.
 *
 * Rows past PLUGIN_SETTINGS_LIST_MAX_ROWS, and "slider" rows past
 * PLUGIN_SETTINGS_LIST_MAX_SLIDERS within one call, are silently dropped --
 * same truncate-don't-error convention as l_plugin_show_list()'s own
 * PLUGIN_MAX_LIST_ITEMS cap. An item with an unknown/missing `type`, or
 * missing its required callback, raises a Lua error (fail loudly at call
 * time, same convention l_plugin_register_list_item() uses for a bad
 * list_id). */
static int l_plugin_show_settings_list(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_Unsigned raw_n = lua_rawlen(L, 2);
    int n = (raw_n > (lua_Unsigned) PLUGIN_SETTINGS_LIST_MAX_ROWS) ? PLUGIN_SETTINGS_LIST_MAX_ROWS : (int) raw_n;

    static char label_bufs[PLUGIN_SETTINGS_LIST_MAX_ROWS][96];
    static const char * labels[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int row_types[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static bool toggle_initial[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int slider_min[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int slider_max[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int slider_value[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static char icon_bufs[PLUGIN_SETTINGS_LIST_MAX_ROWS][256];
    static const char * icon_paths[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int32_t heights[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static int32_t widths[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    static char text_size_bufs[PLUGIN_SETTINGS_LIST_MAX_ROWS][8];
    static const char * text_sizes[PLUGIN_SETTINGS_LIST_MAX_ROWS];
    int new_refs[PLUGIN_SETTINGS_LIST_MAX_ROWS];

    int count = 0;
    int slider_count = 0;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1); /* row table -> stack top */
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        lua_getfield(L, -1, "type");
        const char * type = lua_tostring(L, -1);
        int row_type = -1;
        if (type) {
            if (strcmp(type, "row") == 0) row_type = PLUGIN_SETTINGS_ROW_TAP;
            else if (strcmp(type, "toggle") == 0) row_type = PLUGIN_SETTINGS_ROW_TOGGLE;
            else if (strcmp(type, "slider") == 0) row_type = PLUGIN_SETTINGS_ROW_SLIDER;
        }
        if (row_type < 0) {
            return luaL_error(L, "plugin.show_settings_list: row %d has an unknown or missing type (expected \"row\", \"toggle\", or \"slider\")", i + 1);
        }
        lua_pop(L, 1); /* type string */

        if (row_type == PLUGIN_SETTINGS_ROW_SLIDER && slider_count >= PLUGIN_SETTINGS_LIST_MAX_SLIDERS) {
            lua_pop(L, 1); /* row table */
            continue;
        }

        lua_getfield(L, -1, "label");
        const char * label = lua_tostring(L, -1);
        snprintf(label_bufs[count], sizeof(label_bufs[count]), "%s", label ? label : "");
        lua_pop(L, 1); /* label string */

        const char * cb_field = (row_type == PLUGIN_SETTINGS_ROW_TAP) ? "on_select" : "on_change";
        lua_getfield(L, -1, cb_field);
        if (!lua_isfunction(L, -1)) {
            return luaL_error(L, "plugin.show_settings_list: row %d ('%s') missing %s function", i + 1, label ? label : "", cb_field);
        }
        new_refs[count] = luaL_ref(L, LUA_REGISTRYINDEX); /* pops the function */

        toggle_initial[count] = false;
        slider_min[count] = 0;
        slider_max[count] = 100;
        slider_value[count] = 0;
        if (row_type == PLUGIN_SETTINGS_ROW_TOGGLE) {
            lua_getfield(L, -1, "value");
            toggle_initial[count] = lua_toboolean(L, -1);
            lua_pop(L, 1);
        } else if (row_type == PLUGIN_SETTINGS_ROW_SLIDER) {
            lua_getfield(L, -1, "min");
            slider_min[count] = (int) luaL_optinteger(L, -1, 0);
            lua_pop(L, 1);
            lua_getfield(L, -1, "max");
            slider_max[count] = (int) luaL_optinteger(L, -1, 100);
            lua_pop(L, 1);
            lua_getfield(L, -1, "value");
            slider_value[count] = (int) luaL_optinteger(L, -1, slider_min[count]);
            lua_pop(L, 1);
        }

        icon_bufs[count][0] = '\0';
        icon_paths[count] = NULL;
        lua_getfield(L, -1, "icon");
        const char * icon = lua_tostring(L, -1);
        if (icon) {
            snprintf(icon_bufs[count], sizeof(icon_bufs[count]), "%s", icon);
            icon_paths[count] = icon_bufs[count];
        }
        lua_pop(L, 1);

        lua_getfield(L, -1, "height");
        heights[count] = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "width");
        widths[count] = (int32_t) luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);

        text_size_bufs[count][0] = '\0';
        text_sizes[count] = NULL;
        lua_getfield(L, -1, "text_size");
        const char * text_size = lua_tostring(L, -1);
        if (text_size) {
            if (!is_valid_text_size(text_size)) {
                return luaL_error(L, "plugin.show_settings_list: row %d has an unknown text_size '%s' (expected \"small\", \"medium\", or \"large\")",
                                   i + 1, text_size);
            }
            snprintf(text_size_bufs[count], sizeof(text_size_bufs[count]), "%s", text_size);
            text_sizes[count] = text_size_bufs[count];
        }
        lua_pop(L, 1);

        labels[count] = label_bufs[count];
        row_types[count] = row_type;
        if (row_type == PLUGIN_SETTINGS_ROW_SLIDER) slider_count++;
        count++;

        lua_pop(L, 1); /* row table */
    }

    int slot = gui_plugin_show_settings_list(title, row_types, labels, toggle_initial, slider_min, slider_max,
                                              slider_value, icon_paths, heights, widths, text_sizes, count);
    if (slot < 0 || slot >= PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE) return 0; /* defensive -- shouldn't happen */

    /* Release whatever the reused slot held from its previous population
     * before overwriting -- see plugin_settings_list_rows[]'s own comment on
     * why this is per-slot storage rather than one shared "current" ref. */
    for (int i = 0; i < plugin_settings_list_row_counts[slot]; i++) {
        if (plugin_settings_list_rows[slot][i].L) {
            luaL_unref(plugin_settings_list_rows[slot][i].L, LUA_REGISTRYINDEX,
                       plugin_settings_list_rows[slot][i].callback_ref);
        }
    }
    for (int i = 0; i < count; i++) {
        plugin_settings_list_rows[slot][i].L = L;
        plugin_settings_list_rows[slot][i].callback_ref = new_refs[i];
    }
    plugin_settings_list_row_counts[slot] = count;

    return 0;
}

static int l_plugin_list_dir(lua_State * L) {
    const char * path = luaL_checkstring(L, 1);
    lua_newtable(L);

    DIR * d = opendir(path);
    if (!d) return 1;

    int idx = 1;
    struct dirent * ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        bool is_dir = false;
        if (stat(full, &st) == 0) is_dir = S_ISDIR(st.st_mode);

        lua_newtable(L);
        lua_pushstring(L, ent->d_name);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, is_dir);
        lua_setfield(L, -2, "dir");
        lua_rawseti(L, -2, idx++);
    }
    closedir(d);
    return 1;
}

static int l_plugin_sd_root(lua_State * L) {
    lua_pushstring(L, MUSIC_ROOT_DIR);
    return 1;
}

/* plugin.mkdir(path) -> true | nil, error. mkdir -p semantics: existing
 * directories are success, while an existing non-directory remains an
 * error. This intentionally follows Lua's existing unrestricted path-based
 * file operations; plugins are not confined to sd_root() today. */
static int l_plugin_mkdir(lua_State * L) {
    const char * path = luaL_checkstring(L, 1);
    size_t len = strlen(path);
    if (len == 0 || len >= PATH_MAX) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid path");
        return 2;
    }

    char buf[PATH_MAX];
    memcpy(buf, path, len + 1);
    while (len > 1 && buf[len - 1] == '/') buf[--len] = '\0';

    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        char saved = buf[i];
        buf[i] = '\0';
        if (buf[0] != '\0' && mkdir(buf, 0755) != 0) {
            int saved_errno = errno;
            struct stat st;
            if (saved_errno != EEXIST || stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
                buf[i] = saved;
                lua_pushnil(L);
                lua_pushstring(L, strerror(saved_errno));
                return 2;
            }
        }
        buf[i] = saved;
    }

    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_play_file(lua_State * L) {
    const char * path = luaL_checkstring(L, 1);
    gui_plugin_play_paths(&path, 1, 0);
    return 0;
}

static int l_plugin_play_list(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int start = (int) luaL_optinteger(L, 2, 1) - 1;

    lua_Unsigned raw_n = lua_rawlen(L, 1);
    int n = (raw_n > (lua_Unsigned) PLUGIN_MAX_LIST_ITEMS) ? PLUGIN_MAX_LIST_ITEMS : (int) raw_n;
    if (n <= 0) return 0;
    if (start < 0) start = 0;
    if (start >= n) start = n - 1;

    static char path_bufs[PLUGIN_MAX_LIST_ITEMS][512];
    static const char * paths[PLUGIN_MAX_LIST_ITEMS];
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1);
        const char * s = lua_tostring(L, -1);
        snprintf(path_bufs[i], sizeof(path_bufs[i]), "%s", s ? s : "");
        paths[i] = path_bufs[i];
        lua_pop(L, 1);
    }

    gui_plugin_play_paths(paths, n, start);
    return 0;
}

static int l_plugin_show_toast(lua_State * L) {
    const char * msg = luaL_checkstring(L, 1);
    gui_plugin_show_toast(msg);
    return 0;
}

#ifndef HOST_BUILD
/* Plain byte copy, same fread/fwrite-loop shape as metadata_db.c's own
 * migrate_old_db_if_needed() -- used here to copy a plugin-supplied
 * replacement icon into THEME_OVERRIDE_ROOT. */
static bool copy_file(const char * src_path, const char * dst_path) {
    FILE * in = fopen(src_path, "rb");
    if (!in) return false;
    FILE * out = fopen(dst_path, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    bool ok = true;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(in);
    fclose(in);
    fclose(out);

    if (!ok) remove(dst_path);
    return ok;
}
#endif

/* Reskins an EXISTING theme2 asset in place, e.g.
 * plugin.set_icon("launcher/book.png", plugin.sd_root() .. "/my_book.png")
 * to change what Home's Books tile looks like -- a different case from
 * plugin.register_stream_media_tile()'s own icon argument (which points a
 * BRAND NEW tile at whatever icon it likes with no special handling
 * needed). Copies source_path's bytes into
 * THEME_OVERRIDE_ROOT/relative_path, the same writable-override mechanism
 * assets.c's own asset_path() already checks first for every icon
 * resolution in the app (already how Subsonic's own non-stock icon works
 * today).
 *
 * Real constraint (confirmed by reading LVGL's own image decoder cache,
 * lv_image_decoder.c/lv_image_cache.c): LVGL caches decoded images keyed
 * by path *string*, not file content or mtime, and this codebase never
 * calls lv_image_cache_drop() anywhere. So this only works reliably when
 * called before the FIRST time relative_path is ever resolved+decoded
 * this boot -- i.e. from a plugin's top-level script code, since
 * plugin_manager_init() always runs before gui_init() builds any
 * icon-grid/pill-list screen. Calling this after the app has already
 * shown a tile using relative_path this session will silently NOT update
 * that already-decoded image. No mid-session swap support is attempted --
 * see PLUGINS.md. */
static int l_plugin_set_icon(lua_State * L) {
    const char * relative_path = luaL_checkstring(L, 1);
    const char * source_path = luaL_checkstring(L, 2);

#ifndef HOST_BUILD
    /* Audit finding: relative_path is fully plugin-controlled and used to
     * be concatenated straight onto PLUGIN_THEME_OVERRIDE_ROOT below with
     * no sanitization -- "../../../../data/some/file" walks the resulting
     * dst_path outside that root entirely, and copy_file() then overwrites
     * whatever it lands on with attacker-controlled bytes: an arbitrary
     * file WRITE, independent of (and not covered by) the Lua stdlib
     * sandboxing in sandbox_plugin_lua_state() above, since this is a
     * native C function reachable straight through the vetted plugin.*
     * API. Every real caller only ever needs a plain "dir/name.ext" shape
     * (see this function's own doc comment above) -- reject anything
     * containing a ".." component or starting with '/' outright, rather
     * than trying to canonicalize and re-check. */
    if (relative_path[0] == '/' || strstr(relative_path, "..") != NULL) {
        return luaL_error(L, "plugin.set_icon: relative_path must be a plain path under the theme root, got '%s'",
                           relative_path);
    }

    char dst_path[600];
    snprintf(dst_path, sizeof(dst_path), "%s%s", PLUGIN_THEME_OVERRIDE_ROOT, relative_path);

    /* Best-effort mkdir, ignore EEXIST -- same pattern
     * playlist_files_create() already uses. Every real theme2 asset in
     * this app is exactly one directory level deep (e.g. "launcher/",
     * "stream_media/"), so one mkdir beyond the root is enough; this
     * doesn't attempt a general recursive mkdir -p for deeper paths
     * nothing in this app's own asset layout actually needs. */
    mkdir(PLUGIN_THEME_OVERRIDE_ROOT, 0755);
    char dir_only[600];
    snprintf(dir_only, sizeof(dir_only), "%s", dst_path);
    char * slash = strrchr(dir_only, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir_only, 0755);
    }

    if (!copy_file(source_path, dst_path)) {
        return luaL_error(L, "plugin.set_icon: could not copy '%s' to '%s'", source_path, relative_path);
    }
#else
    (void) source_path; /* no override root on HOST_BUILD -- see assets.c's asset_path() */
#endif
    return 0;
}

/* Every top-level native screen sharing Home's own shape -- a small, fixed,
 * string-keyed set of nav tiles/rows, no toggles/sliders -- gets the same
 * reorder/hide/restyle mechanism Home originally had all to itself. Each
 * screen_id's own valid-key list mirrors gui.c's own per-screen native-defs
 * table 1:1 (gui.c stays the real source of truth for what a key looks
 * like/does -- icon paths, click callbacks -- this file only ever
 * validates key strings against its own mirrored list, same split the
 * original Home-only comment already documented). */
#define PLUGIN_SCREEN_MAX_ITEMS 6

typedef struct {
    const char * screen_id;
    const char * const * keys;
    int key_count;
} plugin_screen_def_t;

static const char * const plugin_home_keys[] = { "music", "stream_media", "wireless", "books", "system", "dac" };
static const char * const plugin_music_keys[] = { "files", "artists", "albums", "album_artist", "all_songs", "playlists" };
static const char * const plugin_wireless_keys[] = { "wifi", "bt", "airplay", "dlna", "remote", "import" };
static const char * const plugin_stream_media_keys[] = { "subsonic" };
static const char * const plugin_settings_keys[] = { "playback", "display", "power", "system", "about" };
static const char * const plugin_books_keys[] = { "books", "favorites" };
static const char * const plugin_dac_keys[] = { "usb_dac", "bluetooth_dac" };

static const plugin_screen_def_t plugin_screen_defs[] = {
    { "home", plugin_home_keys, 6 },
    { "music", plugin_music_keys, 6 },
    { "wireless", plugin_wireless_keys, 6 },
    { "stream_media", plugin_stream_media_keys, 1 },
    { "settings", plugin_settings_keys, 5 },
    { "books", plugin_books_keys, 2 },
    { "dac", plugin_dac_keys, 2 },
};
#define PLUGIN_SCREEN_COUNT (int) (sizeof(plugin_screen_defs) / sizeof(plugin_screen_defs[0]))

/* Reasonable ceiling on a plugin-chosen tile corner radius -- purely
 * cosmetic (unlike an unknown/duplicate/missing key), so this clamps
 * rather than erroring, same "clamp a cosmetic value, error on a
 * structural one" split plugin.register_list_item()'s own height option
 * already makes (PILL_ROW_HEIGHT_MIN/MAX). */
#define PLUGIN_SCREEN_RADIUS_MAX 64

/* Ceiling on options.row_gap -- cosmetic, clamped not errored, same split
 * PLUGIN_SCREEN_RADIUS_MAX already makes for radius. Raised from an
 * original 24 -- real feedback wanted noticeably more breathing room
 * than that allowed on a short list (e.g. Home's 6 rows), and even the
 * shortest native row height (PILL_ROW_HEIGHT_MIN, 48px) leaves enough
 * screen budget for a gap this size without every row no longer fitting
 * without scrolling. */
#define PLUGIN_SCREEN_ROW_GAP_MAX 48

/* Ceiling on options.tile_gap -- tile mode's own analogous spacing knob.
 * Higher ceiling than row_gap: tiles are much bigger than list rows (a
 * whole 2x3 grid cell vs. a thin row), so a proportionally bigger gap still
 * reads as intentional spacing rather than the tiles falling apart. */
#define PLUGIN_SCREEN_TILE_GAP_MAX 40

/* Ceiling on options.title_gap -- same headroom reasoning as
 * PLUGIN_SCREEN_ROW_GAP_MAX above, comes out of the same available
 * content area. */
#define PLUGIN_SCREEN_TITLE_GAP_MAX 64

/* Ceiling on a plugin-chosen border_width -- cosmetic, clamped not errored,
 * same split PLUGIN_SCREEN_RADIUS_MAX already makes for radius. A thin
 * accent border is the realistic use case here, not a thick outline. */
#define PLUGIN_SCREEN_BORDER_WIDTH_MAX 8

/* Ceiling on a plugin-chosen |offset_x| -- cosmetic, clamped not errored,
 * same split PLUGIN_SCREEN_RADIUS_MAX already makes for radius. Generous
 * enough for a real zig-zag/staggered list look (see PLUGIN_STYLING.md)
 * without a large enough shift routinely pushing a narrow row off-screen. */
#define PLUGIN_SCREEN_OFFSET_X_MAX 150

/* Shared by is_valid_screen_text_size() below -- "mono" (lv_font_unscii_16,
 * see lv_conf.h) is only offered through set_screen_layout()/set_home_
 * layout() for now (list mode), not through register_list_item()/
 * show_list(), so this stays a separate check rather than widening
 * is_valid_text_size() itself. */
static bool is_valid_screen_text_size(const char * text_size) {
    return strcmp(text_size, "small") == 0 || strcmp(text_size, "medium") == 0 ||
           strcmp(text_size, "large") == 0 || strcmp(text_size, "mono") == 0;
}

typedef struct {
    char key[16];
    bool has_bg_color;   uint32_t bg_color;   /* 0xRRGGBB */
    bool has_text_color; uint32_t text_color; /* 0xRRGGBB */
    bool has_radius;     int32_t radius;      /* px, clamped to 0..PLUGIN_SCREEN_RADIUS_MAX */
    bool has_bg_alpha;   uint8_t bg_alpha;    /* 0-255, lv_opa_t scale */
    bool has_border;     uint32_t border_color; int32_t border_width; /* px */
    bool has_offset_x;   int32_t offset_x;    /* px, signed, -PLUGIN_SCREEN_OFFSET_X_MAX..+MAX */

    /* ---- List-mode-only extensions (plugin.set_screen_layout()/set_home_
     * layout(), PLUGINS.md). Ignored in tile mode -- only read once
     * plugin_manager_get_screen_list_mode() is true for that screen_id
     * (always true for the pill-list-native screens: settings/books/dac). ---- */
    int32_t row_height; /* 0 = unset/native default (124px) */
    int32_t row_width;  /* 0 = unset/native default */
    char text_align[8]; /* "" = unset ("left"); "left"/"center"/"right" */
    bool accessory;      /* true (default) = chevron shown, false = hidden */
    char text_size[8];  /* "" = unset (native default); "small"/"medium"/"large"/"mono" */
    bool show_icon;      /* true (default) = tile's launcher icon shown, reserving
                           * PILL_ROW_ICON_PX_DEFAULT+12px of label indent for it
                           * (screen_builders.c) even where align="left" -- false
                           * drops the icon AND that reserved indent, so the label
                           * sits flush at the row's own 24px inset. */
} plugin_screen_item_config_t;

static plugin_screen_item_config_t plugin_screen_items[PLUGIN_SCREEN_COUNT][PLUGIN_SCREEN_MAX_ITEMS];
static int plugin_screen_item_count[PLUGIN_SCREEN_COUNT]; /* 0 == no plugin has called set_screen_layout() for this screen yet */
static bool plugin_screen_is_list[PLUGIN_SCREEN_COUNT];
/* options.row_gap -- default matches build_pill_list_screen()'s own
 * pre-existing hardcoded 6px, so a plugin that never sets this changes
 * nothing. */
static int32_t plugin_screen_row_gap[PLUGIN_SCREEN_COUNT] = { 6, 6, 6, 6, 6, 6, 6 };
/* options.tile_gap -- default 0 matches build_icon_grid_screen()'s own
 * pre-existing flush-cell look (every native caller of that function also
 * passes 0), so a plugin that never sets this changes nothing. */
static int32_t plugin_screen_tile_gap[PLUGIN_SCREEN_COUNT];
/* options.title_align/title_size/title_underline -- style overrides for
 * the screen's own title label (build_title(), gui.c), separate from any
 * per-item override since the title isn't one of the tile/row items.
 * "" / false = unset, native default (left-aligned, ui_size_28, no
 * underline) -- a plugin that never sets these changes nothing. */
static char plugin_screen_title_align[PLUGIN_SCREEN_COUNT][8];
static char plugin_screen_title_size[PLUGIN_SCREEN_COUNT][8];
static bool plugin_screen_title_underline[PLUGIN_SCREEN_COUNT];
/* options.title_color -- same idea, a per-screen override of the title
 * label's own text color, separate from set_text_color("primary", ...)'s
 * single app-wide slot (which the title also picks up by default, see
 * build_title()'s own style_theme_text_primary) -- lets a plugin make one
 * screen's title stand out from its own body text. */
static bool plugin_screen_has_title_color[PLUGIN_SCREEN_COUNT];
static uint32_t plugin_screen_title_color[PLUGIN_SCREEN_COUNT];
/* options.title_gap -- vertical px between the title band and the first
 * tile/row below it, today's exact default (4px list mode, 0px tile
 * mode -- see screen_title_style_t's own comment in screen_builders.h)
 * unless a plugin sets it. */
static bool plugin_screen_has_title_gap[PLUGIN_SCREEN_COUNT];
static int32_t plugin_screen_title_gap[PLUGIN_SCREEN_COUNT];

static int plugin_screen_slot(const char * screen_id) {
    for (int i = 0; i < PLUGIN_SCREEN_COUNT; i++) {
        if (strcmp(plugin_screen_defs[i].screen_id, screen_id) == 0) return i;
    }
    return -1;
}

/* Builds "\"a\", \"b\", or \"c\"" for an unknown-key error message -- each
 * screen_id now has its own key set, so this can't be a single literal
 * string the way the original Home-only error message was. */
static void plugin_screen_format_keys(const plugin_screen_def_t * def, char * buf, size_t buf_size) {
    size_t used = 0;
    for (int i = 0; i < def->key_count && used < buf_size; i++) {
        const char * sep = (i == 0) ? "" : (i == def->key_count - 1) ? (def->key_count > 2 ? ", or " : " or ") : ", ";
        int n = snprintf(buf + used, buf_size - used, "%s\"%s\"", sep, def->keys[i]);
        if (n < 0) break;
        used += (size_t) n;
    }
}

/* Shared body behind both plugin.set_screen_layout(screen_id, item_keys
 * [, options]) and plugin.set_home_layout(item_keys [, options]) -- the
 * latter is pure sugar for the former with screen_id pinned to "home", so
 * this is the only place the actual parsing/validation/storage logic
 * exists. keys_idx/options_idx are the Lua stack indices of item_keys/
 * options for whichever entry point called in (2/3 for set_screen_layout,
 * 1/2 for set_home_layout); fn_name is only used in error messages, so
 * callers see the name they actually called.
 *
 * Reorders/hides a screen's native items, optionally switching between the
 * icon grid and a plain vertical list (icon-grid-native screens only --
 * options.mode is silently ignored for a screen gui.c never gives a
 * tile-grid rendering to begin with), and optionally overriding an item's
 * background color/text color/corner radius/background alpha -- only takes
 * effect if called from a plugin's top-level code, since every screen this
 * reaches is built once, right after every plugin finishes loading
 * (plugin_manager_init() runs before gui_init()); calling this later from a
 * callback is a silent no-op until the next restart, same load-time-only
 * constraint plugin.set_icon() already documents. Last plugin to call wins
 * per screen_id if more than one does, same "last call wins" precedent
 * set_background_color()/set_text_color() already established for their
 * own single global slots -- one call always fully specifies both the
 * layout and every per-item style override together, never a partial
 * update layered onto an earlier call. A screen must never end up with
 * zero items, so an empty array, an unknown/duplicate item key, or an
 * invalid `mode` all raise a Lua error (leaving whatever layout -- native
 * default or an earlier successful call -- already in effect untouched)
 * rather than silently doing something wrong. */
static int do_set_screen_layout(lua_State * L, const char * screen_id, int keys_idx, int options_idx,
                                 const char * fn_name) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0) {
        return luaL_error(L, "%s: unknown screen_id '%s'", fn_name, screen_id);
    }
    const plugin_screen_def_t * def = &plugin_screen_defs[slot];

    luaL_checktype(L, keys_idx, LUA_TTABLE);
    lua_Unsigned raw_n = lua_rawlen(L, keys_idx);
    if (raw_n == 0) {
        return luaL_error(L, "%s: item_keys must be a non-empty array", fn_name);
    }
    if (raw_n > (lua_Unsigned) def->key_count) {
        return luaL_error(L, "%s: too many item keys (max %d for screen '%s')", fn_name, def->key_count, screen_id);
    }
    int n = (int) raw_n;

    bool is_list = false;
    int32_t row_gap = 6;  /* today's exact hardcoded build_pill_list_screen() default */
    int32_t tile_gap = 0; /* today's exact flush-cell build_icon_grid_screen() default */
    char title_align[8] = "";
    char title_size[8] = "";
    bool title_underline = false;
    bool has_title_color = false;
    uint32_t title_color = 0;
    bool has_title_gap = false;
    int32_t title_gap = 0;
    if (lua_gettop(L) >= options_idx && !lua_isnil(L, options_idx)) {
        luaL_checktype(L, options_idx, LUA_TTABLE);
        lua_getfield(L, options_idx, "mode");
        if (!lua_isnil(L, -1)) {
            const char * mode = lua_tostring(L, -1);
            if (mode && strcmp(mode, "list") == 0) {
                is_list = true;
            } else if (!mode || strcmp(mode, "tile") != 0) {
                const char * shown = mode ? mode : "?";
                lua_pop(L, 1);
                return luaL_error(L, "%s: options.mode must be \"tile\" or \"list\", got '%s'", fn_name, shown);
            }
        }
        lua_pop(L, 1);

        /* Cosmetic, not structural -- clamp rather than error, same
         * precedent radius's own PLUGIN_SCREEN_RADIUS_MAX clamp sets. */
        lua_getfield(L, options_idx, "row_gap");
        if (!lua_isnil(L, -1)) {
            lua_Integer g = lua_tointeger(L, -1);
            if (g < 0) g = 0;
            if (g > PLUGIN_SCREEN_ROW_GAP_MAX) g = PLUGIN_SCREEN_ROW_GAP_MAX;
            row_gap = (int32_t) g;
        }
        lua_pop(L, 1);

        /* Same cosmetic clamp-not-error precedent as row_gap above -- tile
         * mode's own analogous "space between tiles" knob. */
        lua_getfield(L, options_idx, "tile_gap");
        if (!lua_isnil(L, -1)) {
            lua_Integer g = lua_tointeger(L, -1);
            if (g < 0) g = 0;
            if (g > PLUGIN_SCREEN_TILE_GAP_MAX) g = PLUGIN_SCREEN_TILE_GAP_MAX;
            tile_gap = (int32_t) g;
        }
        lua_pop(L, 1);

        lua_getfield(L, options_idx, "title_align");
        const char * ta = lua_tostring(L, -1);
        if (ta) {
            if (strcmp(ta, "left") != 0 && strcmp(ta, "center") != 0 && strcmp(ta, "right") != 0) {
                const char * shown_ta = ta;
                lua_pop(L, 1);
                return luaL_error(L, "%s: options.title_align must be \"left\", \"center\", or \"right\", got '%s'",
                                   fn_name, shown_ta);
            }
            snprintf(title_align, sizeof(title_align), "%s", ta);
        }
        lua_pop(L, 1);

        lua_getfield(L, options_idx, "title_size");
        const char * ts = lua_tostring(L, -1);
        if (ts) {
            if (!is_valid_screen_text_size(ts)) {
                const char * shown_ts = ts;
                lua_pop(L, 1);
                return luaL_error(L,
                    "%s: unknown options.title_size '%s' (expected \"small\", \"medium\", \"large\", or \"mono\")",
                    fn_name, shown_ts);
            }
            snprintf(title_size, sizeof(title_size), "%s", ts);
        }
        lua_pop(L, 1);

        lua_getfield(L, options_idx, "title_underline");
        if (!lua_isnil(L, -1)) title_underline = lua_toboolean(L, -1);
        lua_pop(L, 1);

        /* title_color -- separate slot from set_text_color("primary", ...)'s
         * single app-wide one (which the title picks up by default), same
         * "own override on top of the global default" relationship
         * bg_color/text_color already have with set_background_color()/
         * set_text_color() at the per-item level. */
        lua_getfield(L, options_idx, "title_color");
        if (!lua_isnil(L, -1)) {
            has_title_color = true;
            title_color = (uint32_t) lua_tointeger(L, -1);
        }
        lua_pop(L, 1);

        /* title_gap -- cosmetic, clamp not error, same precedent row_gap/
         * tile_gap already set. */
        lua_getfield(L, options_idx, "title_gap");
        if (!lua_isnil(L, -1)) {
            has_title_gap = true;
            lua_Integer g = lua_tointeger(L, -1);
            if (g < 0) g = 0;
            if (g > PLUGIN_SCREEN_TITLE_GAP_MAX) g = PLUGIN_SCREEN_TITLE_GAP_MAX;
            title_gap = (int32_t) g;
        }
        lua_pop(L, 1);
    }

    plugin_screen_item_config_t new_items[PLUGIN_SCREEN_MAX_ITEMS];
    memset(new_items, 0, sizeof(new_items));
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, keys_idx, i + 1);

        const char * key;
        bool has_bg_color = false, has_text_color = false, has_radius = false, has_bg_alpha = false;
        bool has_border = false, has_offset_x = false;
        uint32_t bg_color = 0, text_color = 0, border_color = 0;
        int32_t radius = 0, border_width = 0, offset_x = 0;
        uint8_t bg_alpha = 0;
        /* List-mode-only extensions (PLUGINS.md) -- silently ignored in tile
         * mode, same as bg_color/text_color/radius/bg_alpha are never
         * structurally invalid there either, just cosmetically unused by
         * build_icon_grid_screen(). */
        int32_t row_height = 0, row_width = 0;
        char text_align[8] = "";
        bool accessory = true;
        char text_size[8] = "";
        bool show_icon = true;

        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "key");
            key = lua_tostring(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "bg_color");
            if (!lua_isnil(L, -1)) {
                has_bg_color = true;
                bg_color = (uint32_t) lua_tointeger(L, -1);
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "text_color");
            if (!lua_isnil(L, -1)) {
                has_text_color = true;
                text_color = (uint32_t) lua_tointeger(L, -1);
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "radius");
            if (!lua_isnil(L, -1)) {
                has_radius = true;
                lua_Integer r = lua_tointeger(L, -1);
                if (r < 0) r = 0;
                if (r > PLUGIN_SCREEN_RADIUS_MAX) r = PLUGIN_SCREEN_RADIUS_MAX;
                radius = (int32_t) r;
            }
            lua_pop(L, 1);

            /* Cosmetic, matches lv_opa_t's own 0-255 scale -- clamp rather
             * than error, same precedent radius's own clamp sets. Lets a
             * tile/row go semi-transparent over plugin.set_background_image()'s
             * whole-screen wallpaper. */
            lua_getfield(L, -1, "bg_alpha");
            if (!lua_isnil(L, -1)) {
                has_bg_alpha = true;
                lua_Integer a = lua_tointeger(L, -1);
                if (a < 0) a = 0;
                if (a > 255) a = 255;
                bg_alpha = (uint8_t) a;
            }
            lua_pop(L, 1);

            /* border_color/border_width -- a plain 0-width border (the
             * default) is invisible regardless of border_color, so only
             * border_width actually flips has_border on; border_color
             * alone with no width set is silently inert, same "harmless if
             * incomplete" tolerance every other pair-of-fields option here
             * already has. */
            lua_getfield(L, -1, "border_color");
            if (!lua_isnil(L, -1)) border_color = (uint32_t) lua_tointeger(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "border_width");
            if (!lua_isnil(L, -1)) {
                lua_Integer w = lua_tointeger(L, -1);
                if (w < 0) w = 0;
                if (w > PLUGIN_SCREEN_BORDER_WIDTH_MAX) w = PLUGIN_SCREEN_BORDER_WIDTH_MAX;
                border_width = (int32_t) w;
                has_border = border_width > 0;
            }
            lua_pop(L, 1);

            /* offset_x -- a pure visual reposition (lv_obj_set_style_translate_x(),
             * screen_builders.c), not a resize/refill like height/width/bg_color
             * above -- shifts the tile/row that many px left (negative) or right
             * (positive) from its normal centered position, independent of every
             * other property here. The realistic use case is an alternating
             * zig-zag list (a plugin computing +/-N by row index -- see
             * PLUGIN_STYLING.md), not a one-off nudge, hence the generous-but-
             * bounded clamp rather than PLUGIN_SCREEN_BORDER_WIDTH_MAX's tiny one. */
            lua_getfield(L, -1, "offset_x");
            if (!lua_isnil(L, -1)) {
                has_offset_x = true;
                lua_Integer ox = lua_tointeger(L, -1);
                if (ox < -PLUGIN_SCREEN_OFFSET_X_MAX) ox = -PLUGIN_SCREEN_OFFSET_X_MAX;
                if (ox > PLUGIN_SCREEN_OFFSET_X_MAX) ox = PLUGIN_SCREEN_OFFSET_X_MAX;
                offset_x = (int32_t) ox;
            }
            lua_pop(L, 1);

            /* height/width: same "clamp downstream, don't validate here"
             * story as register_list_item()'s own height/width options --
             * build_pill_list_screen() already clamps any nonzero row_height/
             * row_width to PILL_ROW_HEIGHT_MIN/MAX and PILL_ROW_WIDTH_MIN/MAX
             * (screen_builders.c), so 0 here (unset) reaches it unchanged and
             * still means "native default". */
            lua_getfield(L, -1, "height");
            row_height = (int32_t) luaL_optinteger(L, -1, 0);
            lua_pop(L, 1);

            lua_getfield(L, -1, "width");
            row_width = (int32_t) luaL_optinteger(L, -1, 0);
            lua_pop(L, 1);

            lua_getfield(L, -1, "align");
            const char * align = lua_tostring(L, -1);
            if (align) {
                if (strcmp(align, "left") != 0 && strcmp(align, "center") != 0 && strcmp(align, "right") != 0) {
                    const char * shown_align = align;
                    lua_pop(L, 1);
                    return luaL_error(L, "%s: align must be \"left\", \"center\", or \"right\", got '%s'", fn_name,
                                       shown_align);
                }
                snprintf(text_align, sizeof(text_align), "%s", align);
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "accessory");
            if (!lua_isnil(L, -1)) {
                accessory = lua_toboolean(L, -1);
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "icon");
            if (!lua_isnil(L, -1)) {
                show_icon = lua_toboolean(L, -1);
            }
            lua_pop(L, 1);

            lua_getfield(L, -1, "text_size");
            const char * ts = lua_tostring(L, -1);
            if (ts) {
                if (!is_valid_screen_text_size(ts)) {
                    const char * shown_ts = ts;
                    lua_pop(L, 1);
                    return luaL_error(L,
                        "%s: unknown text_size '%s' (expected \"small\", \"medium\", \"large\", or \"mono\")",
                        fn_name, shown_ts);
                }
                snprintf(text_size, sizeof(text_size), "%s", ts);
            }
            lua_pop(L, 1);
        } else {
            key = lua_tostring(L, -1);
        }

        bool known = false;
        for (int k = 0; k < def->key_count; k++) {
            if (key && strcmp(key, def->keys[k]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            const char * shown = key ? key : "?";
            char expected[160];
            plugin_screen_format_keys(def, expected, sizeof(expected));
            lua_pop(L, 1);
            return luaL_error(L, "%s: unknown item key '%s' (expected %s)", fn_name, shown, expected);
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(new_items[j].key, key) == 0) {
                lua_pop(L, 1);
                return luaL_error(L, "%s: duplicate item key '%s'", fn_name, key);
            }
        }

        snprintf(new_items[i].key, sizeof(new_items[i].key), "%s", key);
        new_items[i].has_bg_color = has_bg_color;
        new_items[i].bg_color = bg_color;
        new_items[i].has_text_color = has_text_color;
        new_items[i].text_color = text_color;
        new_items[i].has_radius = has_radius;
        new_items[i].radius = radius;
        new_items[i].has_bg_alpha = has_bg_alpha;
        new_items[i].bg_alpha = bg_alpha;
        new_items[i].has_border = has_border;
        new_items[i].border_color = border_color;
        new_items[i].border_width = border_width;
        new_items[i].has_offset_x = has_offset_x;
        new_items[i].offset_x = offset_x;
        new_items[i].row_height = row_height;
        new_items[i].row_width = row_width;
        snprintf(new_items[i].text_align, sizeof(new_items[i].text_align), "%s", text_align);
        new_items[i].accessory = accessory;
        snprintf(new_items[i].text_size, sizeof(new_items[i].text_size), "%s", text_size);
        new_items[i].show_icon = show_icon;
        lua_pop(L, 1);
    }

    memcpy(plugin_screen_items[slot], new_items, sizeof(new_items[0]) * (size_t) n);
    plugin_screen_item_count[slot] = n;
    plugin_screen_is_list[slot] = is_list;
    plugin_screen_row_gap[slot] = row_gap;
    plugin_screen_tile_gap[slot] = tile_gap;
    snprintf(plugin_screen_title_align[slot], sizeof(plugin_screen_title_align[slot]), "%s", title_align);
    snprintf(plugin_screen_title_size[slot], sizeof(plugin_screen_title_size[slot]), "%s", title_size);
    plugin_screen_title_underline[slot] = title_underline;
    plugin_screen_has_title_color[slot] = has_title_color;
    plugin_screen_title_color[slot] = title_color;
    plugin_screen_has_title_gap[slot] = has_title_gap;
    plugin_screen_title_gap[slot] = title_gap;
    return 0;
}

static int l_plugin_set_screen_layout(lua_State * L) {
    const char * screen_id = luaL_checkstring(L, 1);
    return do_set_screen_layout(L, screen_id, 2, 3, "plugin.set_screen_layout");
}

/* Pure sugar over do_set_screen_layout() with screen_id pinned to "home" --
 * plugin.set_home_layout() already shipped with this exact 2-arg signature
 * before set_screen_layout() existed, so it keeps working unchanged rather
 * than becoming a second, parallel implementation. */
static int l_plugin_set_home_layout(lua_State * L) {
    return do_set_screen_layout(L, "home", 1, 2, "plugin.set_home_layout");
}

int plugin_manager_get_screen_item_count(const char * screen_id) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0) return 0;
    return plugin_screen_item_count[slot];
}

const char * plugin_manager_get_screen_item_key(const char * screen_id, int index) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot]) return NULL;
    return plugin_screen_items[slot][index].key;
}

bool plugin_manager_get_screen_list_mode(const char * screen_id) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0) return false;
    return plugin_screen_is_list[slot];
}

bool plugin_manager_get_screen_item_bg_color(const char * screen_id, int index, uint32_t * out_rgb) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        !plugin_screen_items[slot][index].has_bg_color)
        return false;
    *out_rgb = plugin_screen_items[slot][index].bg_color;
    return true;
}

bool plugin_manager_get_screen_item_text_color(const char * screen_id, int index, uint32_t * out_rgb) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        !plugin_screen_items[slot][index].has_text_color)
        return false;
    *out_rgb = plugin_screen_items[slot][index].text_color;
    return true;
}

bool plugin_manager_get_screen_item_radius(const char * screen_id, int index, int32_t * out_radius) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        !plugin_screen_items[slot][index].has_radius)
        return false;
    *out_radius = plugin_screen_items[slot][index].radius;
    return true;
}

bool plugin_manager_get_screen_item_bg_alpha(const char * screen_id, int index, uint8_t * out_alpha) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        !plugin_screen_items[slot][index].has_bg_alpha)
        return false;
    *out_alpha = plugin_screen_items[slot][index].bg_alpha;
    return true;
}

bool plugin_manager_get_screen_item_border(const char * screen_id, int index, uint32_t * out_color,
                                            int32_t * out_width) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        !plugin_screen_items[slot][index].has_border)
        return false;
    *out_color = plugin_screen_items[slot][index].border_color;
    *out_width = plugin_screen_items[slot][index].border_width;
    return true;
}

bool plugin_manager_get_screen_item_offset_x(const char * screen_id, int index, int32_t * out_offset_x) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        !plugin_screen_items[slot][index].has_offset_x)
        return false;
    *out_offset_x = plugin_screen_items[slot][index].offset_x;
    return true;
}

bool plugin_manager_get_screen_item_row_height(const char * screen_id, int index, int32_t * out_height) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        plugin_screen_items[slot][index].row_height == 0)
        return false;
    *out_height = plugin_screen_items[slot][index].row_height;
    return true;
}

bool plugin_manager_get_screen_item_row_width(const char * screen_id, int index, int32_t * out_width) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        plugin_screen_items[slot][index].row_width == 0)
        return false;
    *out_width = plugin_screen_items[slot][index].row_width;
    return true;
}

const char * plugin_manager_get_screen_item_text_align(const char * screen_id, int index) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        plugin_screen_items[slot][index].text_align[0] == '\0')
        return NULL;
    return plugin_screen_items[slot][index].text_align;
}

bool plugin_manager_get_screen_item_accessory(const char * screen_id, int index) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot]) return true;
    return plugin_screen_items[slot][index].accessory;
}

bool plugin_manager_get_screen_item_show_icon(const char * screen_id, int index) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot]) return true;
    return plugin_screen_items[slot][index].show_icon;
}

const char * plugin_manager_get_screen_item_text_size(const char * screen_id, int index) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || index < 0 || index >= plugin_screen_item_count[slot] ||
        plugin_screen_items[slot][index].text_size[0] == '\0')
        return NULL;
    return plugin_screen_items[slot][index].text_size;
}

int32_t plugin_manager_get_screen_row_gap(const char * screen_id) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0) return 6;
    return plugin_screen_row_gap[slot];
}

int32_t plugin_manager_get_screen_tile_gap(const char * screen_id) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0) return 0;
    return plugin_screen_tile_gap[slot];
}

/* options.title_align/title_size -- "" means unset (native default), same
 * NULL-means-unset convention plugin_manager_get_screen_item_text_align()/
 * _text_size() already use. */
const char * plugin_manager_get_screen_title_align(const char * screen_id) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || plugin_screen_title_align[slot][0] == '\0') return NULL;
    return plugin_screen_title_align[slot];
}

const char * plugin_manager_get_screen_title_size(const char * screen_id) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || plugin_screen_title_size[slot][0] == '\0') return NULL;
    return plugin_screen_title_size[slot];
}

bool plugin_manager_get_screen_title_underline(const char * screen_id) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0) return false;
    return plugin_screen_title_underline[slot];
}

bool plugin_manager_get_screen_title_color(const char * screen_id, uint32_t * out_color) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || !plugin_screen_has_title_color[slot]) return false;
    *out_color = plugin_screen_title_color[slot];
    return true;
}

bool plugin_manager_get_screen_title_gap(const char * screen_id, int32_t * out_gap) {
    int slot = plugin_screen_slot(screen_id);
    if (slot < 0 || !plugin_screen_has_title_gap[slot]) return false;
    *out_gap = plugin_screen_title_gap[slot];
    return true;
}

/* plugin.lerp_color(from, to, t) -- linear-interpolates each RGB channel
 * independently between two 0xRRGGBB colors at t in 0..1. Pure math, no
 * error path needed -- t is cosmetic (clamped, not validated), same
 * "clamp a cosmetic value" precedent PLUGIN_SCREEN_RADIUS_MAX already
 * sets. Lets a plugin build an N-stop gradient across a screen's resolved
 * items from just two endpoint colors instead of hand-picking every
 * stop's own hex value -- see plugins_examples/HomeThemes.lua. */
static int l_plugin_lerp_color(lua_State * L) {
    lua_Integer from = luaL_checkinteger(L, 1);
    lua_Integer to = luaL_checkinteger(L, 2);
    double t = luaL_checknumber(L, 3);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    int fr = (int) ((from >> 16) & 0xFF), fg = (int) ((from >> 8) & 0xFF), fb = (int) (from & 0xFF);
    int tr = (int) ((to >> 16) & 0xFF), tg = (int) ((to >> 8) & 0xFF), tb = (int) (to & 0xFF);
    int r = fr + (int) ((tr - fr) * t);
    int g = fg + (int) ((tg - fg) * t);
    int b = fb + (int) ((tb - fb) * t);

    lua_pushinteger(L, ((lua_Integer) r << 16) | ((lua_Integer) g << 8) | b);
    return 1;
}

/* plugin.set_background_image(image_path) -- a whole-screen wallpaper, not
 * a per-tile one. Mutates the same shared style_theme_screen_bg style
 * plugin.set_background_color("screen", ...) already mutates (gui.c's
 * gui_plugin_set_background_image()) -- every screen root already has this
 * style attached, so one call updates every screen live, no restart and no
 * per-screen plumbing needed. Same path convention every other
 * plugin-supplied-file option already uses (raw absolute, or relative to
 * the SD card's .plugins/ dir, see pill_row_apply_icon()'s own comment) --
 * gui.c resolves it and falls back gracefully to whatever "screen" color is
 * set if the file is missing or fails to decode, same graceful-degrade
 * precedent register_list_item()'s own icon option already sets. */
static int l_plugin_set_background_image(lua_State * L) {
    const char * image_path = luaL_checkstring(L, 1);
    gui_plugin_set_background_image(image_path);
    return 0;
}

/* Sets one of three fixed background-color slots, live, app-wide, no
 * restart needed -- see gui_plugin_set_background_color()'s own comment
 * in gui.c for the shared-style mechanism (same one apply_accent_color()
 * already uses successfully for the existing accent color feature).
 * "screen": every screen's own background. "card": popups, EQ cards,
 * settings slider cards -- every neutral dark surface that isn't a plain
 * screen or a list row. "list_row": every row in every list screen (All
 * Songs, Artists, Playlists, Files, Queue, ...). */
static int l_plugin_set_background_color(lua_State * L) {
    const char * slot = luaL_checkstring(L, 1);
    lua_Integer rgb = luaL_checkinteger(L, 2);

    if (strcmp(slot, "screen") != 0 && strcmp(slot, "card") != 0 && strcmp(slot, "list_row") != 0) {
        return luaL_error(L, "plugin.set_background_color: unknown slot '%s' (expected \"screen\", \"card\", or \"list_row\")",
                           slot);
    }

    gui_plugin_set_background_color(slot, (uint32_t) rgb);
    return 0;
}

/* Sets one of two fixed text-color slots, live, app-wide, no restart needed
 * -- see gui_plugin_set_text_color()'s own comment in gui.c. "primary": the
 * app's dominant near-white text (labels, titles, list rows). "muted":
 * secondary/disabled-ish gray text (chevrons, timestamps, subtitles).
 * Destructive-red and accent-tinted text are NOT covered by either slot --
 * they're semantically fixed, not part of the light/dark background split
 * plugin.set_background_color() drives. */
static int l_plugin_set_text_color(lua_State * L) {
    const char * slot = luaL_checkstring(L, 1);
    lua_Integer rgb = luaL_checkinteger(L, 2);

    if (strcmp(slot, "primary") != 0 && strcmp(slot, "muted") != 0) {
        return luaL_error(L, "plugin.set_text_color: unknown slot '%s' (expected \"primary\" or \"muted\")", slot);
    }

    gui_plugin_set_text_color(slot, (uint32_t) rgb);
    return 0;
}

/* ---- plugin.eq_*() -- bridge straight into peq.c's public API, no gui.c
 * layer needed (peq.c has no LVGL dependency, matching l_plugin_list_dir()/
 * l_plugin_sd_root() already calling plain C functions directly). Every
 * setter ends with peq_save(), the same "persist on every change" pattern
 * every native EQ-screen event handler already follows (see e.g. gui.c's
 * eq_bypass_switch_event_cb()), so a plugin-driven EQ change survives
 * reboot with no new persistence code. ---- */

static int l_plugin_eq_load_profile(lua_State * L) {
    const char * path = luaL_checkstring(L, 1);
    bool ok = peq_load_from_path(path);
    if (ok) peq_save();
    lua_pushboolean(L, ok);
    return 1;
}

static int l_plugin_eq_save_profile(lua_State * L) {
    const char * path = luaL_checkstring(L, 1);
    lua_pushboolean(L, peq_save_to_path(path));
    return 1;
}

static int l_plugin_eq_reset(lua_State * L) {
    (void) L;
    peq_reset_to_defaults();
    peq_save();
    return 0;
}

static int l_plugin_eq_set_bypass(lua_State * L) {
    bool enabled = lua_toboolean(L, 1);
    peq_set_bypass(enabled);
    peq_save();
    return 0;
}

static int l_plugin_eq_set_preamp(lua_State * L) {
    double db = luaL_checknumber(L, 1);
    peq_set_preamp_db(db);
    peq_save();
    return 0;
}

/* Converts a 1-based Lua band index to peq.c's 0-based one, luaL_error()ing
 * if out of range -- same "fail loudly at call time" convention
 * l_plugin_register_list_item() already uses for a bad list_id. */
static int check_eq_band_index(lua_State * L, int arg) {
    lua_Integer index = luaL_checkinteger(L, arg);
    if (index < 1 || index > PEQ_NUM_BANDS) {
        luaL_error(L, "band index %d out of range (expected 1..%d)", (int) index, PEQ_NUM_BANDS);
    }
    return (int) index - 1;
}

static int l_plugin_eq_set_band(lua_State * L) {
    int index = check_eq_band_index(L, 1);
    double freq_hz = luaL_checknumber(L, 2);
    double gain_db = luaL_checknumber(L, 3);
    double q = luaL_checknumber(L, 4);
    peq_set_band(index, freq_hz, gain_db, q);
    peq_save();
    return 0;
}

static int l_plugin_eq_set_band_type(lua_State * L) {
    int index = check_eq_band_index(L, 1);
    const char * type = luaL_checkstring(L, 2);

    peq_band_type_t t;
    if (strcmp(type, "peaking") == 0) t = PEQ_TYPE_PEAKING;
    else if (strcmp(type, "low_shelf") == 0) t = PEQ_TYPE_LOW_SHELF;
    else if (strcmp(type, "high_shelf") == 0) t = PEQ_TYPE_HIGH_SHELF;
    else return luaL_error(L, "plugin.eq_set_band_type: unknown type '%s' (expected \"peaking\", \"low_shelf\", or \"high_shelf\")", type);

    peq_set_band_type(index, t);
    peq_save();
    return 0;
}

static int l_plugin_eq_set_band_enabled(lua_State * L) {
    int index = check_eq_band_index(L, 1);
    bool enabled = lua_toboolean(L, 2);
    peq_set_band_enabled(index, enabled);
    peq_save();
    return 0;
}

/* ---- plugin.toggle_pause()/stop()/next_track()/prev_track()/seek()/
 * set_volume()/is_playing()/is_paused()/get_position()/get_duration() --
 * thin wrappers over gui.h's gui_plugin_*() bridges, needed here (unlike
 * plugin.eq_*() above) because audio.c's own functions must be paired with
 * gui.c-local UI state (play/pause icon, volume slider/popup, shuffle-aware
 * stepping) -- see gui.h's own comment on gui_plugin_toggle_pause() and
 * neighbors for why. ---- */

static int l_plugin_toggle_pause(lua_State * L) {
    (void) L;
    gui_plugin_toggle_pause();
    return 0;
}

static int l_plugin_stop(lua_State * L) {
    (void) L;
    gui_plugin_stop();
    return 0;
}

static int l_plugin_next_track(lua_State * L) {
    (void) L;
    gui_plugin_next_track();
    return 0;
}

static int l_plugin_prev_track(lua_State * L) {
    (void) L;
    gui_plugin_prev_track();
    return 0;
}

static int l_plugin_seek(lua_State * L) {
    double seconds = luaL_checknumber(L, 1);
    gui_plugin_seek(seconds);
    return 0;
}

static int l_plugin_set_volume(lua_State * L) {
    lua_Integer percent = luaL_checkinteger(L, 1);
    gui_plugin_set_volume((int) percent);
    return 0;
}

static int l_plugin_is_playing(lua_State * L) {
    lua_pushboolean(L, gui_plugin_is_playing());
    return 1;
}

static int l_plugin_is_paused(lua_State * L) {
    lua_pushboolean(L, gui_plugin_is_paused());
    return 1;
}

static int l_plugin_get_position(lua_State * L) {
    lua_pushnumber(L, gui_plugin_get_position_seconds());
    return 1;
}

static int l_plugin_get_duration(lua_State * L) {
    lua_pushnumber(L, gui_plugin_get_duration_seconds());
    return 1;
}

/* plugin.http_get(url [, verify_tls]) -> status, body | nil, "network error"
 * -- bridges http_client.c's http_get_to_buffer() directly (no gui.c layer,
 * same reasoning as plugin.eq_*() above: no LVGL/gui-state dependency).
 * body is pushed with lua_pushlstring() (exact byte count), not
 * lua_pushstring(), since a response body isn't guaranteed NUL-free. Runs
 * synchronously on the calling (main UI) thread -- see this file's own
 * top-of-file doc comment for the blocking caveat. */
static int l_plugin_http_get(lua_State * L) {
    const char * url = luaL_checkstring(L, 1);
    bool verify_tls = lua_gettop(L) >= 2 ? lua_toboolean(L, 2) : true;

    int status = 0;
    uint8_t * body = NULL;
    size_t body_size = 0;
    bool ok = http_get_to_buffer(url, verify_tls, &status, &body, &body_size);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "network error");
        return 2;
    }

    lua_pushinteger(L, status);
    lua_pushlstring(L, (const char *) body, body_size);
    free(body);
    return 2;
}

/* plugin.http_post(url, body [, content_type] [, verify_tls]) -> status,
 * body | nil, "network error" -- same contract/shape as l_plugin_http_get()
 * above, bridging http_client.c's new http_post_to_buffer(). body is read
 * with luaL_checklstring() (not luaL_checkstring()) since a POST body isn't
 * guaranteed NUL-free either (e.g. a binary payload), same reasoning as the
 * response body's own lua_pushlstring() below. content_type defaults to
 * "application/x-www-form-urlencoded" (what most simple API POSTs,
 * including Last.fm's, expect) when omitted or nil. */
static int l_plugin_http_post(lua_State * L) {
    const char * url = luaL_checkstring(L, 1);
    size_t body_len = 0;
    const char * body = luaL_checklstring(L, 2, &body_len);
    const char * content_type =
        (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? luaL_checkstring(L, 3) : "application/x-www-form-urlencoded";
    bool verify_tls = lua_gettop(L) >= 4 ? lua_toboolean(L, 4) : true;

    int status = 0;
    uint8_t * resp_body = NULL;
    size_t resp_body_size = 0;
    bool ok = http_post_to_buffer(url, verify_tls, content_type, (const uint8_t *) body, body_len, &status,
                                   &resp_body, &resp_body_size);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "network error");
        return 2;
    }

    lua_pushinteger(L, status);
    lua_pushlstring(L, (const char *) resp_body, resp_body_size);
    free(resp_body);
    return 2;
}

typedef struct {
    bool active;
    atomic_bool done;
    atomic_bool cancelled;
    uint16_t generation;
    pthread_t thread;
    lua_State * L;
    int callback_ref;
    bool is_post;
    bool is_download;
    bool verify_tls;
    char url[2048];
    char content_type[128];
    char dest_path[PATH_MAX];
    uint8_t * request_body;
    size_t request_body_size;
    size_t max_response_size;
    bool ok;
    int status;
    uint8_t * response_body;
    size_t response_body_size;
} plugin_async_http_t;

static plugin_async_http_t plugin_async_http[PLUGIN_MAX_ASYNC_HTTP];

static bool plugin_async_download_progress(uint64_t downloaded, uint64_t total, void * user_data) {
    (void) downloaded;
    (void) total;
    plugin_async_http_t * req = (plugin_async_http_t *) user_data;
    return !atomic_load(&req->cancelled);
}

static void * plugin_async_http_thread_func(void * arg) {
    plugin_async_http_t * req = (plugin_async_http_t *) arg;
    if (req->is_download) {
        char temp_path[PATH_MAX];
        int n = snprintf(temp_path, sizeof(temp_path), "%s.part.XXXXXX", req->dest_path);
        if (n <= 0 || (size_t) n >= sizeof(temp_path)) {
            req->ok = false;
        } else {
            int fd = mkstemp(temp_path);
            if (fd < 0) {
                req->ok = false;
            } else {
                close(fd);
                req->ok = http_get_to_file(req->url, req->verify_tls, temp_path,
                                            plugin_async_download_progress, req);
                if (req->ok && !atomic_load(&req->cancelled)) {
                    req->ok = rename(temp_path, req->dest_path) == 0;
                }
                if (!req->ok || atomic_load(&req->cancelled)) remove(temp_path);
            }
        }
    } else if (req->is_post) {
        req->ok = http_post_to_buffer_limited(req->url, req->verify_tls, req->content_type, req->request_body,
                                               req->request_body_size, req->max_response_size, &req->status,
                                               &req->response_body, &req->response_body_size);
    } else {
        req->ok = http_get_to_buffer_limited(req->url, req->verify_tls, req->max_response_size, &req->status,
                                              &req->response_body, &req->response_body_size);
    }
    free(req->request_body);
    req->request_body = NULL;
    req->request_body_size = 0;
    atomic_store(&req->done, true);
    return NULL;
}

/* plugin.http_request(options, callback) -> handle | nil, error. Network
 * work happens on a native worker and the callback is invoked only later by
 * plugin_manager_poll() on the UI/Lua thread. callback(status, body, error):
 * status/body are nil on failure; error is nil on success. */
static int l_plugin_http_request(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        if (!plugin_async_http[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "too many active HTTP requests");
        return 2;
    }

    lua_getfield(L, 1, "url");
    const char * url = luaL_checkstring(L, -1);
    if (strlen(url) >= sizeof(plugin_async_http[slot].url)) {
        return luaL_error(L, "plugin.http_request: URL is too long");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "method");
    const char * method = luaL_optstring(L, -1, "GET");
    bool is_post;
    if (strcasecmp(method, "GET") == 0) is_post = false;
    else if (strcasecmp(method, "POST") == 0) is_post = true;
    else return luaL_error(L, "plugin.http_request: method must be GET or POST");
    lua_pop(L, 1);

    lua_getfield(L, 1, "body");
    size_t body_size = 0;
    const char * body = lua_isnil(L, -1) ? NULL : luaL_checklstring(L, -1, &body_size);
    if (body_size > PLUGIN_ASYNC_HTTP_MAX_REQUEST) {
        return luaL_error(L, "plugin.http_request: request body exceeds %u bytes", PLUGIN_ASYNC_HTTP_MAX_REQUEST);
    }
    uint8_t * body_copy = NULL;
    if (body_size > 0) {
        body_copy = malloc(body_size);
        if (!body_copy) return luaL_error(L, "plugin.http_request: out of memory");
        memcpy(body_copy, body, body_size);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "content_type");
    const char * content_type = luaL_optstring(L, -1, "application/x-www-form-urlencoded");
    if (strlen(content_type) >= sizeof(plugin_async_http[slot].content_type)) {
        free(body_copy);
        return luaL_error(L, "plugin.http_request: content_type is too long");
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "verify_tls");
    bool verify_tls = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "max_response_bytes");
    lua_Integer requested_max = luaL_optinteger(L, -1, PLUGIN_ASYNC_HTTP_DEFAULT_MAX);
    lua_pop(L, 1);
    if (requested_max < 1 || requested_max > PLUGIN_ASYNC_HTTP_MAX_RESPONSE) {
        free(body_copy);
        return luaL_error(L, "plugin.http_request: max_response_bytes must be 1..%u",
                          PLUGIN_ASYNC_HTTP_MAX_RESPONSE);
    }

    plugin_async_http_t * req = &plugin_async_http[slot];
    uint16_t generation = (uint16_t) (req->generation + 1);
    if (generation == 0) generation = 1;
    memset(req, 0, sizeof(*req));
    atomic_init(&req->done, false);
    atomic_init(&req->cancelled, false);
    req->generation = generation;
    req->active = true;
    req->L = L;
    req->is_post = is_post;
    req->verify_tls = verify_tls;
    req->request_body = body_copy;
    req->request_body_size = body_size;
    req->max_response_size = (size_t) requested_max;
    snprintf(req->url, sizeof(req->url), "%s", url);
    snprintf(req->content_type, sizeof(req->content_type), "%s", content_type);
    lua_pushvalue(L, 2);
    req->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (pthread_create(&req->thread, NULL, plugin_async_http_thread_func, req) != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
        free(req->request_body);
        req->request_body = NULL;
        req->active = false;
        lua_pushnil(L);
        lua_pushstring(L, "could not start HTTP worker");
        return 2;
    }

    int handle = ((int) generation << 8) | (slot + 1);
    lua_pushinteger(L, handle);
    return 1;
}

/* plugin.download_file_async(url, dest_path [, verify_tls], callback)
 * -> handle | nil, error. Streams into a unique sibling temporary file on
 * the native HTTP worker, then atomically renames it over dest_path only
 * after a complete 2xx response. callback(dest_path, error) runs later on
 * the Lua/UI thread, matching http_request's callback discipline. */
static int l_plugin_download_file_async(lua_State * L) {
    const char * url = luaL_checkstring(L, 1);
    const char * dest_path = luaL_checkstring(L, 2);
    int callback_index = lua_isfunction(L, 3) ? 3 : 4;
    bool verify_tls = callback_index == 3 ? true : lua_toboolean(L, 3);
    luaL_checktype(L, callback_index, LUA_TFUNCTION);

    if (strlen(url) >= sizeof(plugin_async_http[0].url))
        return luaL_error(L, "plugin.download_file_async: URL is too long");
    if (dest_path[0] == '\0' || strlen(dest_path) >= sizeof(plugin_async_http[0].dest_path) - 12)
        return luaL_error(L, "plugin.download_file_async: destination path is invalid or too long");

    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        if (!plugin_async_http[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "too many active HTTP requests");
        return 2;
    }

    plugin_async_http_t * req = &plugin_async_http[slot];
    uint16_t generation = (uint16_t) (req->generation + 1);
    if (generation == 0) generation = 1;
    memset(req, 0, sizeof(*req));
    atomic_init(&req->done, false);
    atomic_init(&req->cancelled, false);
    req->generation = generation;
    req->active = true;
    req->L = L;
    req->is_download = true;
    req->verify_tls = verify_tls;
    snprintf(req->url, sizeof(req->url), "%s", url);
    snprintf(req->dest_path, sizeof(req->dest_path), "%s", dest_path);
    lua_pushvalue(L, callback_index);
    req->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (pthread_create(&req->thread, NULL, plugin_async_http_thread_func, req) != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
        req->active = false;
        lua_pushnil(L);
        lua_pushstring(L, "could not start HTTP worker");
        return 2;
    }

    lua_pushinteger(L, ((int) generation << 8) | (slot + 1));
    return 1;
}

static int l_plugin_cancel(lua_State * L) {
    int handle = (int) luaL_checkinteger(L, 1);
    int slot = (handle & 0xFF) - 1;
    uint16_t generation = (uint16_t) ((unsigned int) handle >> 8);
    bool cancelled = false;
    if (slot >= 0 && slot < PLUGIN_MAX_ASYNC_HTTP) {
        plugin_async_http_t * req = &plugin_async_http[slot];
        if (req->active && req->generation == generation) {
            atomic_store(&req->cancelled, true);
            cancelled = true;
        }
    }
    lua_pushboolean(L, cancelled);
    return 1;
}

/* plugin.md5(text) -> lowercase hex string -- thin wrapper around
 * mbedtls_md5(), already vendored and already used exactly this way in
 * subsonic_client.c's own token-auth code, so no new dependency. Needed for
 * Last.fm's api_sig signing scheme (md5(sorted params + shared secret)),
 * and generically useful beyond that. */
static int l_plugin_md5(lua_State * L) {
    size_t len = 0;
    const char * data = luaL_checklstring(L, 1, &len);

    unsigned char digest[16];
    mbedtls_md5((const unsigned char *) data, len, digest);

    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    lua_pushstring(L, hex);
    return 1;
}

/* The single pending plugin.show_text_input() call's on_submit function --
 * show_text_entry() (gui.c) is itself a true singleton screen. A second
 * plugin request now returns "text input busy" rather than overwriting this
 * callback, and Back explicitly releases it. on_submit only fires on a T9
 * keypad Enter, never on the screen's own back button. Documented in
 * PLUGINS.md rather than engineered away, matching this codebase's existing
 * tolerance for show_list()'s own analogous single-ref caveat. */
static lua_State * pending_text_input_L = NULL;
static int pending_text_input_ref = LUA_NOREF;

static int l_plugin_show_text_input(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    const char * initial_text = (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) ? luaL_checkstring(L, 2) : NULL;
    bool is_password = lua_toboolean(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);

    if (pending_text_input_L && pending_text_input_ref != LUA_NOREF) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "text input busy");
        return 2;
    }
    lua_pushvalue(L, 4);
    pending_text_input_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    pending_text_input_L = L;

    gui_plugin_show_text_input(title, initial_text, is_password);
    lua_pushboolean(L, true);
    return 1;
}

/* plugin.get_now_playing() -> title, artist, album, duration_seconds, or a
 * single nil if nothing is loaded -- pull accessor for anything that isn't
 * reacting to the "track_started" event directly (e.g. a set_interval()
 * tick double-checking what's still playing). Backed by gui.c's own cached
 * now-playing globals, populated at the same two call sites that fire
 * "track_started" -- see gui_plugin_get_now_playing()'s own comment. */
static int l_plugin_get_now_playing(lua_State * L) {
    char title[128], artist[128], album[128];
    double duration_seconds = 0.0;
    if (!gui_plugin_get_now_playing(title, sizeof(title), artist, sizeof(artist), album, sizeof(album),
                                     &duration_seconds)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, title);
    lua_pushstring(L, artist);
    lua_pushstring(L, album);
    lua_pushnumber(L, duration_seconds);
    return 4;
}

/* plugin.get_play_mode() -> "sequential" | "repeat_all" | "repeat_one" |
 * "shuffle" -- thin wrap of gui_plugin_get_play_mode(), same "no gui-state
 * code in this file" boundary as is_playing()/get_position() above. */
static int l_plugin_get_play_mode(lua_State * L) {
    lua_pushstring(L, gui_plugin_get_play_mode());
    return 1;
}

/* plugin.get_current_track_path() -> path | nil, if nothing is loaded. */
static int l_plugin_get_current_track_path(lua_State * L) {
    const char * path = gui_plugin_get_current_track_path();
    if (!path) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, path);
    return 1;
}

/* Shared tail for get_artist_albums()/get_album_tracks()/
 * get_next_album_tracks() below -- pushes a malloc'd gui_plugin_* string
 * array as a 1-indexed Lua table (or nil if it came back empty/NULL) and
 * frees the C array, since nothing on the Lua side needs it once copied in. */
static int push_string_array_result(lua_State * L, char ** items, int count) {
    if (!items || count <= 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        lua_pushstring(L, items[i]);
        lua_rawseti(L, -2, i + 1);
    }
    gui_plugin_free_string_array(items, count);
    return 1;
}

/* plugin.get_artist_albums(artist) -> { album_name, ... } | nil */
static int l_plugin_get_artist_albums(lua_State * L) {
    const char * artist = luaL_checkstring(L, 1);
    int count = 0;
    char ** albums = gui_plugin_get_artist_albums(artist, &count);
    return push_string_array_result(L, albums, count);
}

/* plugin.get_album_tracks(artist, album) -> { track_path, ... } | nil */
static int l_plugin_get_album_tracks(lua_State * L) {
    const char * artist = luaL_checkstring(L, 1);
    const char * album = luaL_checkstring(L, 2);
    int count = 0;
    char ** tracks = gui_plugin_get_album_tracks(artist, album, &count);
    return push_string_array_result(L, tracks, count);
}

/* plugin.get_next_album_tracks(artist, current_album) -> { track_path, ... }
 * | nil -- nil both when current_album isn't found and when it's the
 * artist's last album (see gui_plugin_get_next_album_tracks()'s own
 * comment: no wraparound). */
static int l_plugin_get_next_album_tracks(lua_State * L) {
    const char * artist = luaL_checkstring(L, 1);
    const char * current_album = luaL_checkstring(L, 2);
    int count = 0;
    char ** tracks = gui_plugin_get_next_album_tracks(artist, current_album, &count);
    return push_string_array_result(L, tracks, count);
}

/* ---- plugin.library_* -- paged, DB-backed library access (see gui.h's own
 * gui_plugin_library_* comment for the design intent: bounded per call,
 * never a whole-library dump). Every row pushes the same {id, path, title,
 * artist, album, album_artist} shape for a song, or {name, count,
 * first_song_id} for an artist/album group. ---- */

static void push_song_row(lua_State * L, const song_row_t * row) {
    /* Real bug caught in review: pushing row->tags.title verbatim left
     * "title" blank for any untagged file (confirmed against this device's
     * own library). metadata_db_song_display_title() falls back to the
     * file's own basename, same as gui.c's own on-device list screens
     * already do for the exact same case. */
    char display_title[128];
    metadata_db_song_display_title(row, display_title, sizeof(display_title));

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer) row->id); lua_setfield(L, -2, "id");
    lua_pushstring(L, row->path); lua_setfield(L, -2, "path");
    lua_pushstring(L, display_title); lua_setfield(L, -2, "title");
    lua_pushstring(L, row->tags.artist); lua_setfield(L, -2, "artist");
    lua_pushstring(L, row->tags.album); lua_setfield(L, -2, "album");
    lua_pushstring(L, row->tags.album_artist); lua_setfield(L, -2, "album_artist");
}

static void push_group_row(lua_State * L, const group_row_t * row) {
    lua_newtable(L);
    lua_pushstring(L, row->name); lua_setfield(L, -2, "name");
    lua_pushinteger(L, row->song_count); lua_setfield(L, -2, "count");
    lua_pushinteger(L, (lua_Integer) row->first_song_id); lua_setfield(L, -2, "first_song_id");
    /* Empty ("") for artists/album_artists -- only ever populated by
     * metadata_db_get_albums_page_filtered() (see that function's own
     * comment) -- disambiguates two different artists' same-titled albums,
     * which now show as separate rows here instead of silently merging. */
    lua_pushstring(L, row->album_artist); lua_setfield(L, -2, "album_artist");
}

static int push_song_rows_result(lua_State * L, const song_row_t * rows, int count) {
    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        push_song_row(L, &rows[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* plugin.library_song_count() -> integer */
static int l_plugin_library_song_count(lua_State * L) {
    lua_pushinteger(L, (lua_Integer) gui_plugin_library_song_count());
    return 1;
}

/* plugin.library_get_songs(offset, limit, filters) -> { song, ... }, total
 * -- offset/limit default to 0/GUI_PLUGIN_LIBRARY_MAX_PAGE; filters is an
 * optional table with any of query/artist/album_artist/album (all optional
 * substring-or-exact filters, see gui_plugin_library_get_songs()'s own
 * comment). total is the match count across every page, for a plugin's own
 * "page N of M" UI. */
static int l_plugin_library_get_songs(lua_State * L) {
    int offset = (int) luaL_optinteger(L, 1, 0);
    int limit = (int) luaL_optinteger(L, 2, GUI_PLUGIN_LIBRARY_MAX_PAGE);
    const char * query = NULL, * artist = NULL, * album_artist = NULL, * album = NULL;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "query"); query = lua_tostring(L, -1);
        lua_getfield(L, 3, "artist"); artist = lua_tostring(L, -1);
        lua_getfield(L, 3, "album_artist"); album_artist = lua_tostring(L, -1);
        lua_getfield(L, 3, "album"); album = lua_tostring(L, -1);
        /* query/artist/album_artist/album above all point into the Lua
         * stack slots pushed by lua_getfield() -- valid until something
         * else touches the stack, which gui_plugin_library_get_songs()
         * below doesn't do, so reading them after all four is safe. */
    }

    /* Heap-allocated, not a stack array -- song_row_t is ~1.25KB (a 600-
     * byte path plus a 640-byte cached_tags_t), and GUI_PLUGIN_LIBRARY_MAX_
     * PAGE (200) of those is ~250KB. remote_control.c hit exactly this as a
     * real on-device stack-overflow crash (see build_library_json()'s own
     * comment) before it was fixed the same way; not worth trusting that
     * this callback's own thread stack happens to be big enough just
     * because it's the main thread rather than a bare pthread_create(). */
    song_row_t * rows = malloc(sizeof(song_row_t) * GUI_PLUGIN_LIBRARY_MAX_PAGE);
    int64_t total = 0;
    int n = rows ? gui_plugin_library_get_songs(query, artist, album_artist, album, offset, limit, rows, &total) : 0;
    push_song_rows_result(L, rows, n);
    free(rows);
    lua_pushinteger(L, (lua_Integer) total);
    return 2;
}

/* plugin.library_search(query, limit) -> { song, ... } */
static int l_plugin_library_search(lua_State * L) {
    const char * query = luaL_checkstring(L, 1);
    int limit = (int) luaL_optinteger(L, 2, GUI_PLUGIN_LIBRARY_MAX_PAGE);

    song_row_t * rows = malloc(sizeof(song_row_t) * GUI_PLUGIN_LIBRARY_MAX_PAGE); /* see get_songs()'s own comment */
    int n = rows ? gui_plugin_library_search(query, limit, rows) : 0;
    int result = push_song_rows_result(L, rows, n);
    free(rows);
    return result;
}

/* plugin.library_get_song(id) -> song | nil */
static int l_plugin_library_get_song(lua_State * L) {
    int64_t id = (int64_t) luaL_checkinteger(L, 1);
    song_row_t row;
    if (!gui_plugin_library_get_song(id, &row)) {
        lua_pushnil(L);
        return 1;
    }
    push_song_row(L, &row);
    return 1;
}

/* plugin.library_get_artists(offset, limit) -> { {name, count,
 * first_song_id}, ... } */
static int l_plugin_library_get_artists(lua_State * L) {
    int offset = (int) luaL_optinteger(L, 1, 0);
    int limit = (int) luaL_optinteger(L, 2, GUI_PLUGIN_LIBRARY_MAX_PAGE);

    group_row_t * rows = malloc(sizeof(group_row_t) * GUI_PLUGIN_LIBRARY_MAX_PAGE); /* see get_songs()'s own comment */
    int n = rows ? gui_plugin_library_get_artists(offset, limit, rows) : 0;
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        push_group_row(L, &rows[i]);
        lua_rawseti(L, -2, i + 1);
    }
    free(rows);
    return 1;
}

/* plugin.library_get_albums(offset, limit, artist) -> { {name, count,
 * first_song_id}, ... } -- artist optional (nil/omitted means every album,
 * unfiltered), matches either that name's artist or album_artist tag. */
static int l_plugin_library_get_albums(lua_State * L) {
    int offset = (int) luaL_optinteger(L, 1, 0);
    int limit = (int) luaL_optinteger(L, 2, GUI_PLUGIN_LIBRARY_MAX_PAGE);
    const char * artist_filter = luaL_optstring(L, 3, NULL);

    group_row_t * rows = malloc(sizeof(group_row_t) * GUI_PLUGIN_LIBRARY_MAX_PAGE); /* see get_songs()'s own comment */
    int n = rows ? gui_plugin_library_get_albums(offset, limit, artist_filter, rows) : 0;
    lua_newtable(L);
    for (int i = 0; i < n; i++) {
        push_group_row(L, &rows[i]);
        lua_rawseti(L, -2, i + 1);
    }
    free(rows);
    return 1;
}

/* ---- plugin.on(event, callback) -- see plugin_manager.h's own
 * PLUGIN_MAX_EVENT_SUBSCRIBERS comment for the design rationale (multiple
 * plugins can subscribe to the same event, unlike register_list_item()'s
 * "each plugin's row coexists as its own entry" model). Four events, each
 * with its own small subscriber array -- storage kept separate per event
 * (not a single generic dispatch table keyed by string) for the same reason
 * l_plugin_register_list_item() gives each list_id its own array: known,
 * small, fixed set, not something built ahead of actually needing genericity. ---- */

typedef enum {
    PLUGIN_EVENT_TRACK_STARTED = 0,
    PLUGIN_EVENT_PAUSED,
    PLUGIN_EVENT_RESUMED,
    PLUGIN_EVENT_STOPPED,
    PLUGIN_EVENT_COUNT,
} plugin_event_t;

typedef struct {
    lua_State * L;
    int ref;
} plugin_event_subscriber_t;

static plugin_event_subscriber_t plugin_event_subscribers[PLUGIN_EVENT_COUNT][PLUGIN_MAX_EVENT_SUBSCRIBERS];
static int plugin_event_subscriber_count[PLUGIN_EVENT_COUNT];

static int l_plugin_on(lua_State * L) {
    const char * event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    plugin_event_t idx;
    if (strcmp(event, "track_started") == 0) idx = PLUGIN_EVENT_TRACK_STARTED;
    else if (strcmp(event, "paused") == 0) idx = PLUGIN_EVENT_PAUSED;
    else if (strcmp(event, "resumed") == 0) idx = PLUGIN_EVENT_RESUMED;
    else if (strcmp(event, "stopped") == 0) idx = PLUGIN_EVENT_STOPPED;
    else return luaL_error(L, "plugin.on: unknown event '%s' (expected \"track_started\", \"paused\", \"resumed\", or \"stopped\")", event);

    if (plugin_event_subscriber_count[idx] >= PLUGIN_MAX_EVENT_SUBSCRIBERS) {
        return luaL_error(L, "plugin.on: too many subscribers registered for \"%s\" (max %d)", event,
                           PLUGIN_MAX_EVENT_SUBSCRIBERS);
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    plugin_event_subscriber_t * sub = &plugin_event_subscribers[idx][plugin_event_subscriber_count[idx]++];
    sub->L = L;
    sub->ref = ref;
    return 0;
}

/* ---- plugin.set_interval(seconds, callback) -> handle /
 * plugin.clear_interval(handle) -- see plugin_manager.h's own
 * PLUGIN_MAX_INTERVALS/PLUGIN_INTERVAL_MIN_MS comments. handle is the pool
 * slot + 1 (0 reserved as "invalid"/never returned). ---- */

typedef struct {
    lua_State * L;
    int ref;
    bool active;
} plugin_interval_t;

static plugin_interval_t plugin_intervals[PLUGIN_MAX_INTERVALS];

static int l_plugin_set_interval(lua_State * L) {
    double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_INTERVALS; i++) {
        if (!plugin_intervals[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        return luaL_error(L, "plugin.set_interval: too many active intervals (max %d)", PLUGIN_MAX_INTERVALS);
    }

    uint32_t period_ms = (uint32_t) (seconds * 1000.0);
    if (period_ms < PLUGIN_INTERVAL_MIN_MS) period_ms = PLUGIN_INTERVAL_MIN_MS;

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    plugin_intervals[slot].L = L;
    plugin_intervals[slot].ref = ref;
    plugin_intervals[slot].active = true;

    gui_plugin_set_interval(slot, period_ms);
    lua_pushinteger(L, slot + 1);
    return 1;
}

static int l_plugin_clear_interval(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    int slot = (int) handle - 1;
    if (slot < 0 || slot >= PLUGIN_MAX_INTERVALS || !plugin_intervals[slot].active) return 0;

    luaL_unref(plugin_intervals[slot].L, LUA_REGISTRYINDEX, plugin_intervals[slot].ref);
    plugin_intervals[slot].active = false;
    plugin_intervals[slot].L = NULL;
    gui_plugin_clear_interval(slot);
    return 0;
}

static bool plugin_id_is_valid(const char * id) {
    if (!id || !id[0]) return false;
    for (const unsigned char * p = (const unsigned char *) id; *p; p++) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return false;
    }
    return true;
}

/* plugin.define({ id=..., name=..., version=..., api_min=... }) establishes
 * stable identity before future storage/permission APIs are added. It is
 * deliberately legal only while this file is executing its top-level code:
 * identity cannot change after callbacks and resources have been registered. */
static int l_plugin_define(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (loading_plugin_slot < 0 || loading_plugin_slot >= PLUGIN_MAX_FILES ||
        plugin_instances[loading_plugin_slot].L != L) {
        return luaL_error(L, "plugin.define may only be called once during plugin loading");
    }
    plugin_instance_t * inst = &plugin_instances[loading_plugin_slot];
    if (inst->defined) return luaL_error(L, "plugin.define may only be called once");

    lua_getfield(L, 1, "id");
    const char * id = luaL_checkstring(L, -1);
    if (!plugin_id_is_valid(id) || strlen(id) >= sizeof(inst->id)) {
        return luaL_error(L, "plugin.define: id must be 1-%zu characters using letters, digits, '.', '_' or '-'",
                          sizeof(inst->id) - 1);
    }
    for (int i = 0; i < plugin_instance_count; i++) {
        if (i != loading_plugin_slot && plugin_instances[i].defined && strcmp(plugin_instances[i].id, id) == 0) {
            return luaL_error(L, "plugin.define: duplicate plugin id '%s'", id);
        }
    }
    snprintf(inst->id, sizeof(inst->id), "%s", id);
    lua_pop(L, 1);

    lua_getfield(L, 1, "name");
    const char * name = luaL_optstring(L, -1, id);
    snprintf(inst->name, sizeof(inst->name), "%s", name);
    lua_pop(L, 1);
    lua_getfield(L, 1, "version");
    const char * version = luaL_optstring(L, -1, "0");
    snprintf(inst->version, sizeof(inst->version), "%s", version);
    lua_pop(L, 1);
    lua_getfield(L, 1, "api_min");
    lua_Integer api_min = luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);
    if (api_min > PLUGIN_API_VERSION) {
        return luaL_error(L, "plugin '%s' requires API %lld, player provides API %d", id,
                          (long long) api_min, PLUGIN_API_VERSION);
    }
    inst->defined = true;
    return 0;
}

static int l_plugin_api_version(lua_State * L) {
    lua_pushinteger(L, PLUGIN_API_VERSION);
    return 1;
}

static const char * const plugin_capabilities[] = {
    "ui.list", "ui.settings", "ui.row_width", "ui.text_input", "ui.toast", "ui.theme",
    "filesystem.sd", "playback.control", "playback.state", "playback.events",
    "library.artist_albums", "library.paged", "network.http.sync", "network.http.async",
    "network.http.download", "filesystem.mkdir", "crypto.md5", "audio.peq"
};

static int l_plugin_has_capability(lua_State * L) {
    const char * requested = luaL_checkstring(L, 1);
    bool found = false;
    for (size_t i = 0; i < sizeof(plugin_capabilities) / sizeof(plugin_capabilities[0]); i++) {
        if (strcmp(requested, plugin_capabilities[i]) == 0) { found = true; break; }
    }
    lua_pushboolean(L, found);
    return 1;
}

static int l_plugin_get_app_info(lua_State * L) {
    lua_newtable(L);
    lua_pushstring(L, "Beta 1"); lua_setfield(L, -2, "version");
#if defined(TEST_BUILD_TAG)
    lua_pushstring(L, TEST_BUILD_TAG); lua_setfield(L, -2, "build");
#elif defined(BUILD_STAMP)
    lua_pushstring(L, BUILD_STAMP); lua_setfield(L, -2, "build");
#else
    lua_pushstring(L, "unknown"); lua_setfield(L, -2, "build");
#endif
#ifdef HOST_BUILD
    lua_pushstring(L, "host");
#else
    lua_pushstring(L, "hiby-r1");
#endif
    lua_setfield(L, -2, "platform");
    lua_pushinteger(L, PLUGIN_API_VERSION); lua_setfield(L, -2, "plugin_api");
    return 1;
}

static const luaL_Reg plugin_funcs[] = {
    { "define",                    l_plugin_define },
    { "api_version",               l_plugin_api_version },
    { "has_capability",            l_plugin_has_capability },
    { "get_app_info",              l_plugin_get_app_info },
    { "register_list_item",        l_plugin_register_list_item },
    { "register_stream_media_tile", l_plugin_register_stream_media_tile },
    { "show_list",                 l_plugin_show_list },
    { "show_settings_list",        l_plugin_show_settings_list },
    { "list_dir",                  l_plugin_list_dir },
    { "sd_root",                   l_plugin_sd_root },
    { "mkdir",                     l_plugin_mkdir },
    { "play_file",                 l_plugin_play_file },
    { "play_list",                 l_plugin_play_list },
    { "show_toast",                l_plugin_show_toast },
    { "set_icon",                  l_plugin_set_icon },
    { "set_home_layout",           l_plugin_set_home_layout },
    { "set_screen_layout",         l_plugin_set_screen_layout },
    { "set_background_color",      l_plugin_set_background_color },
    { "set_text_color",            l_plugin_set_text_color },
    { "set_background_image",      l_plugin_set_background_image },
    { "lerp_color",                l_plugin_lerp_color },
    { "eq_load_profile",           l_plugin_eq_load_profile },
    { "eq_save_profile",           l_plugin_eq_save_profile },
    { "eq_reset",                  l_plugin_eq_reset },
    { "eq_set_bypass",             l_plugin_eq_set_bypass },
    { "eq_set_preamp",             l_plugin_eq_set_preamp },
    { "eq_set_band",               l_plugin_eq_set_band },
    { "eq_set_band_type",          l_plugin_eq_set_band_type },
    { "eq_set_band_enabled",       l_plugin_eq_set_band_enabled },
    { "toggle_pause",              l_plugin_toggle_pause },
    { "stop",                      l_plugin_stop },
    { "next_track",                l_plugin_next_track },
    { "prev_track",                l_plugin_prev_track },
    { "seek",                      l_plugin_seek },
    { "set_volume",                l_plugin_set_volume },
    { "is_playing",                l_plugin_is_playing },
    { "is_paused",                 l_plugin_is_paused },
    { "get_position",              l_plugin_get_position },
    { "get_duration",              l_plugin_get_duration },
    { "http_get",                  l_plugin_http_get },
    { "http_post",                 l_plugin_http_post },
    { "http_request",              l_plugin_http_request },
    { "download_file_async",       l_plugin_download_file_async },
    { "cancel",                    l_plugin_cancel },
    { "md5",                       l_plugin_md5 },
    { "show_text_input",           l_plugin_show_text_input },
    { "get_now_playing",           l_plugin_get_now_playing },
    { "get_play_mode",             l_plugin_get_play_mode },
    { "get_current_track_path",    l_plugin_get_current_track_path },
    { "get_artist_albums",         l_plugin_get_artist_albums },
    { "get_album_tracks",          l_plugin_get_album_tracks },
    { "get_next_album_tracks",     l_plugin_get_next_album_tracks },
    { "library_song_count",        l_plugin_library_song_count },
    { "library_get_songs",         l_plugin_library_get_songs },
    { "library_search",            l_plugin_library_search },
    { "library_get_song",          l_plugin_library_get_song },
    { "library_get_artists",       l_plugin_library_get_artists },
    { "library_get_albums",        l_plugin_library_get_albums },
    { "on",                        l_plugin_on },
    { "set_interval",              l_plugin_set_interval },
    { "clear_interval",            l_plugin_clear_interval },
    { NULL, NULL }
};

static void register_plugin_api(lua_State * L) {
    luaL_newlib(L, plugin_funcs);
    lua_setglobal(L, "plugin");
}

/* Audit finding: luaL_openlibs() grants every plugin's lua_State the full,
 * unrestricted standard library -- os.execute()/io.popen() for arbitrary
 * shell commands, load()/loadstring()/dofile()/require() to pull in and
 * run further arbitrary code, and debug.* (powerful enough to defeat any
 * of the other restrictions below via metatable manipulation). A plugin is
 * just a .lua file dropped into <MUSIC_ROOT_DIR>/.plugins/, no different in
 * trust level from any other file an SD card author could place there, and
 * this codebase already treats SD-card-supplied content as untrusted
 * everywhere else (see the ID3v2/M4A parser hardening and file_browser.c's
 * symlink rejection elsewhere in this same audit).
 *
 * Real-device correction: an earlier version of this function also removed
 * the whole `io` table and os.remove()/os.rename(), reasoning that
 * plugin_funcs' own filesystem API (list_dir()/sd_root()) covered
 * everything a plugin should need -- live testing against the actual
 * installed plugins proved that wrong: all 7 (Audiobooks, Themes,
 * SoundProfiles, PlaybackExtras, LastFmScrobbler, ExtendedSleepTimer,
 * PlayThrough) use io.open() to persist their own small state file under
 * plugin.sd_root() .. "/.plugins/...", and 3 of them use os.remove()+
 * os.rename() for an atomic write-to-.tmp-then-rename -- the exact same
 * pattern this project's own C code uses (see playlist_files.c). Zero
 * installed plugins use os.execute(), io.popen(), load()/dofile()/
 * require(), or debug.* -- only the genuinely dangerous, actually-unused
 * primitives are removed below now: arbitrary shell execution and
 * arbitrary code loading. Plain file I/O and os.remove()/os.rename() stay,
 * since real plugins depend on them and they can't run a shell command or
 * load further code, just read/write/delete/rename whatever path the
 * plugin already had permission to name (same risk class as this app's own
 * file I/O, not a privilege escalation). */
static void sandbox_plugin_lua_state(lua_State * L) {
    lua_pushnil(L); lua_setglobal(L, "load");
    lua_pushnil(L); lua_setglobal(L, "loadstring");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "require");
    lua_pushnil(L); lua_setglobal(L, "package");
    lua_pushnil(L); lua_setglobal(L, "debug");

    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        static const char * dangerous_os[] = { "execute", "getenv", "exit", "tmpname" };
        for (size_t i = 0; i < sizeof(dangerous_os) / sizeof(dangerous_os[0]); i++) {
            lua_pushnil(L);
            lua_setfield(L, -2, dangerous_os[i]);
        }
    }
    lua_pop(L, 1);

    lua_getglobal(L, "io");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "popen"); /* the one io.* function that's a shell-exec primitive, not file I/O */
    }
    lua_pop(L, 1);
}

#define PLUGIN_CALL_MAX_MS 2000 /* generous margin above any legitimate plugin operation observed; still short enough that a runaway loop doesn't hang the UI for long */

static struct timespec plugin_call_deadline_start;

static void plugin_call_timeout_hook(lua_State * L, lua_Debug * ar) {
    (void) ar;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - plugin_call_deadline_start.tv_sec) * 1000L +
                      (now.tv_nsec - plugin_call_deadline_start.tv_nsec) / 1000000L;
    if (elapsed_ms > PLUGIN_CALL_MAX_MS) {
        luaL_error(L, "plugin call exceeded %dms time budget -- aborted to keep the UI responsive", PLUGIN_CALL_MAX_MS);
    }
}

/* Audit finding: no execution-time limit was ever installed on any plugin
 * lua_State, and every plugin callback (on_open/on_select/timers/event
 * handlers, all 11 lua_pcall() call sites in this file) runs synchronously
 * on the single UI thread -- a plugin containing `while true do end`
 * freezes the whole player indefinitely, needing a hard power-cycle.
 * Wraps lua_pcall() with a wall-clock budget checked every LUA_MASKCOUNT
 * instructions via a debug hook, installed/torn down around each call.
 * This whole plugin system only ever runs on the UI thread (every
 * lua_State is only ever touched from here), so a single file-static
 * deadline-start variable, reset immediately before each call, is safe --
 * no locking needed. Same signature as lua_pcall() itself, so every
 * existing call site needed only a mechanical rename. */
static int plugin_call(lua_State * L, int nargs, int nresults, int errfunc) {
    clock_gettime(CLOCK_MONOTONIC, &plugin_call_deadline_start);
    lua_sethook(L, plugin_call_timeout_hook, LUA_MASKCOUNT, 10000);
    int result = lua_pcall(L, nargs, nresults, errfunc);
    lua_sethook(L, NULL, 0, 0); /* stop checking between plugin calls -- no cost while idle */
    return result;
}

static void load_plugin_file(const char * path) {
    lua_State * L = luaL_newstate();
    if (!L) return;
    int slot = plugin_instance_count;
    plugin_instance_t * inst = &plugin_instances[slot];
    memset(inst, 0, sizeof(*inst));
    inst->L = L;
    loading_plugin_slot = slot;
    luaL_openlibs(L);
    sandbox_plugin_lua_state(L);
    register_plugin_api(L);

    /* luaL_dofile(L, path)'s own expansion, with plugin_call() in place of
     * a bare lua_pcall() -- top-level plugin code runs once here too, on
     * the same UI thread as every other plugin_call() site, so it needs
     * the same time budget (see plugin_call()'s own comment). */
    if ((luaL_loadfile(L, path) || plugin_call(L, 0, LUA_MULTRET, 0)) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] failed to load %s: %s\n", path, err ? err : "unknown error");
        lua_close(L);
        memset(inst, 0, sizeof(*inst));
        loading_plugin_slot = -1;
        return;
    }

    if (!inst->defined) {
        const char * base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(inst->id, sizeof(inst->id), "legacy.%.*s", (int) sizeof(inst->id) - 8, base);
        char * dot = strrchr(inst->id, '.');
        if (dot && strcasecmp(dot, ".lua") == 0) *dot = '\0';
        snprintf(inst->name, sizeof(inst->name), "%.*s", (int) sizeof(inst->name) - 1, base);
        snprintf(inst->version, sizeof(inst->version), "0");
        inst->defined = true;
    }
    loading_plugin_slot = -1;
    plugin_instance_count++;
}

void plugin_manager_init(void) {
    char dir_path[600];
    snprintf(dir_path, sizeof(dir_path), "%s/.plugins", MUSIC_ROOT_DIR);

    DIR * d = opendir(dir_path);
    if (!d) return; /* no .plugins folder -- nothing to do, not an error */

    struct dirent * ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".lua") != 0) continue;
        if (plugin_instance_count >= PLUGIN_MAX_FILES) break;

        char full_path[700];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
        load_plugin_file(full_path);
    }
    closedir(d);
}

void plugin_manager_poll(void) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        plugin_async_http_t * req = &plugin_async_http[i];
        if (!req->active || !atomic_load(&req->done)) continue;
        pthread_join(req->thread, NULL);

        if (!atomic_load(&req->cancelled)) {
            lua_rawgeti(req->L, LUA_REGISTRYINDEX, req->callback_ref);
            int callback_args;
            if (req->is_download) {
                if (req->ok) {
                    lua_pushstring(req->L, req->dest_path);
                    lua_pushnil(req->L);
                } else {
                    lua_pushnil(req->L);
                    lua_pushstring(req->L, "download failed");
                }
                callback_args = 2;
            } else if (req->ok) {
                lua_pushinteger(req->L, req->status);
                lua_pushlstring(req->L, req->response_body ? (const char *) req->response_body : "",
                                req->response_body_size);
                lua_pushnil(req->L);
                callback_args = 3;
            } else {
                lua_pushnil(req->L);
                lua_pushnil(req->L);
                lua_pushstring(req->L, "network error or response limit exceeded");
                callback_args = 3;
            }
            if (plugin_call(req->L, callback_args, 0, 0) != LUA_OK) {
                const char * err = lua_tostring(req->L, -1);
                fprintf(stderr, "[plugins] HTTP callback error: %s\n", err ? err : "unknown error");
                lua_pop(req->L, 1);
            }
        }

        luaL_unref(req->L, LUA_REGISTRYINDEX, req->callback_ref);
        free(req->response_body);
        req->response_body = NULL;
        req->response_body_size = 0;
        req->active = false;
        atomic_store(&req->done, false);
        atomic_store(&req->cancelled, false);
        req->L = NULL;
        req->callback_ref = LUA_NOREF;
    }
}

/* Shared by plugin_manager_books_list_item_clicked()/_settings_list_item_
 * clicked() below -- kind is just for the stderr message ("books list
 * item"/"settings list item"), so a load-time error in one plugin's row is
 * distinguishable from another's in the log. */
static void dispatch_list_item_open(plugin_list_item_t * item, const char * kind) {
    lua_rawgeti(item->L, LUA_REGISTRYINDEX, item->open_ref);
    if (plugin_call(item->L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(item->L, -1);
        fprintf(stderr, "[plugins] %s '%s' on_open error: %s\n", kind, item->label, err ? err : "unknown error");
        lua_pop(item->L, 1);
    }
}

/* Shared by every plugin_manager_get_*_list_item_options() below --
 * translates a plugin_list_item_t's empty-string "unset" convention back to
 * NULL, matching what screen_builders.c's pill_row_apply_icon()/
 * pill_row_resolve_text_size() already expect. Out-of-range index leaves
 * every out param at its "unset" value, same tolerance every other
 * out-of-range accessor in this family already has. */
static void get_list_item_options(const plugin_list_item_t * array, int count, int index, const char ** out_icon,
                                   int32_t * out_height, int32_t * out_width, const char ** out_text_size) {
    *out_icon = NULL;
    *out_height = 0;
    *out_width = 0;
    *out_text_size = NULL;
    if (index < 0 || index >= count) return;

    const plugin_list_item_t * item = &array[index];
    if (item->icon_path[0]) *out_icon = item->icon_path;
    *out_height = item->row_height;
    *out_width = item->row_width;
    if (item->text_size[0]) *out_text_size = item->text_size;
}

int plugin_manager_get_books_list_item_count(void) {
    return plugin_books_list_item_count;
}

const char * plugin_manager_get_books_list_item_label(int index) {
    if (index < 0 || index >= plugin_books_list_item_count) return "";
    return plugin_books_list_items[index].label;
}

void plugin_manager_books_list_item_clicked(int index) {
    if (index < 0 || index >= plugin_books_list_item_count) return;
    dispatch_list_item_open(&plugin_books_list_items[index], "books list item");
}

void plugin_manager_get_books_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                 int32_t * out_width, const char ** out_text_size) {
    get_list_item_options(plugin_books_list_items, plugin_books_list_item_count, index, out_icon, out_height,
                           out_width, out_text_size);
}

int plugin_manager_get_settings_list_item_count(void) {
    return plugin_settings_list_item_count;
}

const char * plugin_manager_get_settings_list_item_label(int index) {
    if (index < 0 || index >= plugin_settings_list_item_count) return "";
    return plugin_settings_list_items[index].label;
}

void plugin_manager_settings_list_item_clicked(int index) {
    if (index < 0 || index >= plugin_settings_list_item_count) return;
    dispatch_list_item_open(&plugin_settings_list_items[index], "settings list item");
}

void plugin_manager_get_settings_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                    int32_t * out_width, const char ** out_text_size) {
    get_list_item_options(plugin_settings_list_items, plugin_settings_list_item_count, index, out_icon, out_height,
                           out_width, out_text_size);
}

int plugin_manager_get_display_list_item_count(void) {
    return plugin_display_list_item_count;
}

const char * plugin_manager_get_display_list_item_label(int index) {
    if (index < 0 || index >= plugin_display_list_item_count) return "";
    return plugin_display_list_items[index].label;
}

void plugin_manager_display_list_item_clicked(int index) {
    if (index < 0 || index >= plugin_display_list_item_count) return;
    dispatch_list_item_open(&plugin_display_list_items[index], "display list item");
}

void plugin_manager_get_display_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                   int32_t * out_width, const char ** out_text_size) {
    get_list_item_options(plugin_display_list_items, plugin_display_list_item_count, index, out_icon, out_height,
                           out_width, out_text_size);
}

int plugin_manager_get_playback_list_item_count(void) {
    return plugin_playback_list_item_count;
}

const char * plugin_manager_get_playback_list_item_label(int index) {
    if (index < 0 || index >= plugin_playback_list_item_count) return "";
    return plugin_playback_list_items[index].label;
}

void plugin_manager_playback_list_item_clicked(int index) {
    if (index < 0 || index >= plugin_playback_list_item_count) return;
    dispatch_list_item_open(&plugin_playback_list_items[index], "playback list item");
}

void plugin_manager_get_playback_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                    int32_t * out_width, const char ** out_text_size) {
    get_list_item_options(plugin_playback_list_items, plugin_playback_list_item_count, index, out_icon, out_height,
                           out_width, out_text_size);
}

int plugin_manager_get_power_list_item_count(void) {
    return plugin_power_list_item_count;
}

const char * plugin_manager_get_power_list_item_label(int index) {
    if (index < 0 || index >= plugin_power_list_item_count) return "";
    return plugin_power_list_items[index].label;
}

void plugin_manager_power_list_item_clicked(int index) {
    if (index < 0 || index >= plugin_power_list_item_count) return;
    dispatch_list_item_open(&plugin_power_list_items[index], "power list item");
}

void plugin_manager_get_power_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                 int32_t * out_width, const char ** out_text_size) {
    get_list_item_options(plugin_power_list_items, plugin_power_list_item_count, index, out_icon, out_height,
                           out_width, out_text_size);
}

int plugin_manager_get_system_list_item_count(void) {
    return plugin_system_list_item_count;
}

const char * plugin_manager_get_system_list_item_label(int index) {
    if (index < 0 || index >= plugin_system_list_item_count) return "";
    return plugin_system_list_items[index].label;
}

void plugin_manager_system_list_item_clicked(int index) {
    if (index < 0 || index >= plugin_system_list_item_count) return;
    dispatch_list_item_open(&plugin_system_list_items[index], "system list item");
}

void plugin_manager_get_system_list_item_options(int index, const char ** out_icon, int32_t * out_height,
                                                  int32_t * out_width, const char ** out_text_size) {
    get_list_item_options(plugin_system_list_items, plugin_system_list_item_count, index, out_icon, out_height,
                           out_width, out_text_size);
}

/* Shared by plugin_manager_stream_tile_clicked() below (the only remaining
 * caller now that plugin_manager_tile_clicked() -- the old, single-slot
 * "Audio Books" dispatch -- is gone, replaced by the books-list-item
 * registry above). */
static void dispatch_tile_open(plugin_tile_t * t) {
    lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->open_ref);
    if (plugin_call(t->L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(t->L, -1);
        fprintf(stderr, "[plugins] tile '%s' on_open error: %s\n", t->label, err ? err : "unknown error");
        lua_pop(t->L, 1);
    }
}

int plugin_manager_get_stream_tile_count(void) {
    return plugin_stream_tile_count;
}

const char * plugin_manager_get_stream_tile_label(int index) {
    if (index < 0 || index >= plugin_stream_tile_count) return "";
    return plugin_stream_tiles[index].label;
}

const char * plugin_manager_get_stream_tile_icon(int index) {
    if (index < 0 || index >= plugin_stream_tile_count) return "stream_media/radio.png";
    return plugin_stream_tiles[index].icon;
}

const char * plugin_manager_get_stream_tile_icon_selected(int index) {
    if (index < 0 || index >= plugin_stream_tile_count) return "stream_media/radio_s.png";
    return plugin_stream_tiles[index].icon_selected;
}

void plugin_manager_stream_tile_clicked(int index) {
    if (index < 0 || index >= plugin_stream_tile_count) return;
    dispatch_tile_open(&plugin_stream_tiles[index]);
}

void plugin_manager_list_item_selected(int slot, int index) {
    if (slot < 0 || slot >= PLUGIN_LIST_SCREEN_POOL_SIZE) return;
    plugin_list_callback_t * cb = &plugin_list_callbacks[slot];
    if (!cb->L || cb->select_ref == LUA_NOREF) return;

    lua_rawgeti(cb->L, LUA_REGISTRYINDEX, cb->select_ref);
    lua_pushinteger(cb->L, index + 1);
    if (plugin_call(cb->L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(cb->L, -1);
        fprintf(stderr, "[plugins] show_list on_select error: %s\n", err ? err : "unknown error");
        lua_pop(cb->L, 1);
    }
}

/* Shared bounds check for the three plugin_manager_settings_list_*() below --
 * out-of-range or never-populated is a silent no-op, same tolerance
 * plugin_manager_list_item_selected() above has for "no show_list() call is
 * current yet". */
static bool settings_list_row_ref(int slot, int row, lua_State ** out_L, int * out_ref) {
    if (slot < 0 || slot >= PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE) return false;
    if (row < 0 || row >= plugin_settings_list_row_counts[slot]) return false;
    plugin_settings_list_row_ref_t * r = &plugin_settings_list_rows[slot][row];
    if (!r->L) return false;
    *out_L = r->L;
    *out_ref = r->callback_ref;
    return true;
}

void plugin_manager_settings_list_row_selected(int slot, int row) {
    lua_State * L;
    int ref;
    if (!settings_list_row_ref(slot, row, &L, &ref)) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (plugin_call(L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_settings_list on_select error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_settings_list_toggled(int slot, int row, bool new_value) {
    lua_State * L;
    int ref;
    if (!settings_list_row_ref(slot, row, &L, &ref)) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushboolean(L, new_value);
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_settings_list on_change error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_settings_list_slid(int slot, int row, int new_value) {
    lua_State * L;
    int ref;
    if (!settings_list_row_ref(slot, row, &L, &ref)) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, new_value);
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_settings_list on_change error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_notify_track_started(const char * title, const char * artist, const char * album,
                                          double duration_seconds) {
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_TRACK_STARTED]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[PLUGIN_EVENT_TRACK_STARTED][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        lua_pushstring(sub->L, title ? title : "");
        lua_pushstring(sub->L, artist ? artist : "");
        lua_pushstring(sub->L, album ? album : "");
        lua_pushnumber(sub->L, duration_seconds);
        if (plugin_call(sub->L, 4, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] track_started handler error: %s\n", err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

/* Shared by plugin_manager_notify_paused()/_resumed()/_stopped() below --
 * the three zero-argument events. */
static void notify_event_no_args(plugin_event_t idx, const char * kind) {
    for (int i = 0; i < plugin_event_subscriber_count[idx]; i++) {
        plugin_event_subscriber_t * sub = &plugin_event_subscribers[idx][i];
        lua_rawgeti(sub->L, LUA_REGISTRYINDEX, sub->ref);
        if (plugin_call(sub->L, 0, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(sub->L, -1);
            fprintf(stderr, "[plugins] %s handler error: %s\n", kind, err ? err : "unknown error");
            lua_pop(sub->L, 1);
        }
    }
}

void plugin_manager_notify_paused(void) {
    notify_event_no_args(PLUGIN_EVENT_PAUSED, "paused");
}

void plugin_manager_notify_resumed(void) {
    notify_event_no_args(PLUGIN_EVENT_RESUMED, "resumed");
}

void plugin_manager_notify_stopped(void) {
    notify_event_no_args(PLUGIN_EVENT_STOPPED, "stopped");
}

void plugin_manager_interval_fired(int slot) {
    if (slot < 0 || slot >= PLUGIN_MAX_INTERVALS || !plugin_intervals[slot].active) return;

    lua_State * L = plugin_intervals[slot].L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, plugin_intervals[slot].ref);
    if (plugin_call(L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] set_interval callback error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
}

void plugin_manager_text_input_submitted(const char * text) {
    if (!pending_text_input_L || pending_text_input_ref == LUA_NOREF) return;

    lua_State * L = pending_text_input_L;
    int ref = pending_text_input_ref;
    pending_text_input_L = NULL;
    pending_text_input_ref = LUA_NOREF;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushstring(L, text ? text : "");
    if (plugin_call(L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] show_text_input on_submit error: %s\n", err ? err : "unknown error");
        lua_pop(L, 1);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

void plugin_manager_text_input_cancelled(void) {
    if (!pending_text_input_L || pending_text_input_ref == LUA_NOREF) return;
    luaL_unref(pending_text_input_L, LUA_REGISTRYINDEX, pending_text_input_ref);
    pending_text_input_L = NULL;
    pending_text_input_ref = LUA_NOREF;
}
