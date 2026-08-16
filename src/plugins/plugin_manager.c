#include "plugin_manager.h"
#include "gui.h"
#include "peq.h"
#include "http_client.h"

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

typedef struct {
    lua_State * L;
    int open_ref; /* LUA_REGISTRYINDEX ref to the on_open function */
    char label[64];
    char icon[96];
    char icon_selected[96];
} plugin_tile_t;

/* Leaner sibling of plugin_tile_t for plugin.register_list_item() -- no
 * icon fields, since pill-list rows (screen_builders.h's pill_list_item_t)
 * have no icon slot at all to fill; storing one would just be dead data
 * again, the exact problem this whole plugin theming/list-item redesign
 * started from (icon/label fields captured but never rendered). */
typedef struct {
    lua_State * L;
    int open_ref;
    char label[64];
} plugin_list_item_t;

static lua_State * plugin_instances[PLUGIN_MAX_FILES];
static int plugin_instance_count = 0;

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

/* Separate registry for plugin.register_stream_media_tile() -- same
 * plugin_tile_t shape, different surface (gui.c's Stream Media screen).
 * See PLUGIN_MAX_STREAM_TILES's own comment in plugin_manager.h for why
 * Stream Media gets real tiles when Home doesn't. */
static plugin_tile_t plugin_stream_tiles[PLUGIN_MAX_STREAM_TILES];
static int plugin_stream_tile_count = 0;

/* The most recent plugin.show_list() call's on_select function -- only one
 * plugin list screen is ever the front-most one at a time (see gui.c's
 * gui_plugin_show_list() pool comment), so a single "current" ref is
 * enough; a later show_list() call replaces it and releases the old ref. */
static lua_State * current_list_L = NULL;
static int current_list_select_ref = LUA_NOREF;

/* ---- plugin.* C API, exposed as a global table `plugin` in every loaded
 * script's own lua_State (see register_plugin_api() below):
 *
 *   plugin.register_list_item(list_id, label, on_open)
 *     Adds a row labeled `label` to an existing native list screen,
 *     identified by `list_id` -- "books" (gui.c's Books screen, appended
 *     after its own "Books"/"Favorites" rows), "settings" (gui.c's
 *     Settings screen, appended after its own "Playback"/"Display"/
 *     "Power"/"System"/"About" rows), or "display" (gui.c's Settings ->
 *     Display sub-screen, appended after its own "Accent Color"/"Font
 *     Size"/"Screen Timeout"/"Swipe Up for Home" rows); see
 *     PLUGIN_MAX_BOOKS_LIST_ITEMS, PLUGIN_MAX_SETTINGS_LIST_ITEMS, and
 *     PLUGIN_MAX_DISPLAY_LIST_ITEMS in plugin_manager.h for the per-screen
 *     caps. Every plugin that calls this gets its own row (not just the
 *     first, unlike the old register_tile()/"Audio Books" row this
 *     replaced) -- tapping it calls on_open() with no arguments.
 *
 *   plugin.show_list(title, items, on_select)
 *     Opens a list screen titled `title` showing each string in the
 *     `items` array table as a row. on_select(index): called with the
 *     1-based Lua index of the tapped row.
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

/* Shared by l_plugin_register_list_item() below, once its capacity check
 * for the target array has already passed -- pushes on_open (Lua stack
 * index 3 in the caller) into the registry and appends {L, ref, label}. */
static void append_list_item(plugin_list_item_t * array, int * count, lua_State * L, const char * label) {
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    plugin_list_item_t * item = &array[(*count)++];
    item->L = L;
    item->open_ref = ref;
    snprintf(item->label, sizeof(item->label), "%s", label);
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
    } else {
        return luaL_error(L, "plugin.register_list_item: unknown list_id '%s' (expected \"books\", \"settings\", or \"display\")", list_id);
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

static int l_plugin_show_list(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    lua_Unsigned raw_n = lua_rawlen(L, 2);
    int n = (raw_n > (lua_Unsigned) PLUGIN_MAX_LIST_ITEMS) ? PLUGIN_MAX_LIST_ITEMS : (int) raw_n;

    static char label_bufs[PLUGIN_MAX_LIST_ITEMS][160];
    static const char * labels[PLUGIN_MAX_LIST_ITEMS];
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        const char * s = lua_tostring(L, -1);
        snprintf(label_bufs[i], sizeof(label_bufs[i]), "%s", s ? s : "");
        labels[i] = label_bufs[i];
        lua_pop(L, 1);
    }

    if (current_list_L && current_list_select_ref != LUA_NOREF) {
        luaL_unref(current_list_L, LUA_REGISTRYINDEX, current_list_select_ref);
    }
    lua_pushvalue(L, 3);
    current_list_select_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    current_list_L = L;

    gui_plugin_show_list(title, labels, n);
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

static const luaL_Reg plugin_funcs[] = {
    { "register_list_item",        l_plugin_register_list_item },
    { "register_stream_media_tile", l_plugin_register_stream_media_tile },
    { "show_list",                 l_plugin_show_list },
    { "list_dir",                  l_plugin_list_dir },
    { "sd_root",                   l_plugin_sd_root },
    { "play_file",                 l_plugin_play_file },
    { "play_list",                 l_plugin_play_list },
    { "show_toast",                l_plugin_show_toast },
    { "set_icon",                  l_plugin_set_icon },
    { "set_background_color",      l_plugin_set_background_color },
    { "set_text_color",            l_plugin_set_text_color },
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
    { NULL, NULL }
};

static void register_plugin_api(lua_State * L) {
    luaL_newlib(L, plugin_funcs);
    lua_setglobal(L, "plugin");
}

static void load_plugin_file(const char * path) {
    lua_State * L = luaL_newstate();
    if (!L) return;
    luaL_openlibs(L);
    register_plugin_api(L);

    if (luaL_dofile(L, path) != LUA_OK) {
        const char * err = lua_tostring(L, -1);
        fprintf(stderr, "[plugins] failed to load %s: %s\n", path, err ? err : "unknown error");
        lua_close(L);
        return;
    }

    plugin_instances[plugin_instance_count++] = L;
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

/* Shared by plugin_manager_books_list_item_clicked()/_settings_list_item_
 * clicked() below -- kind is just for the stderr message ("books list
 * item"/"settings list item"), so a load-time error in one plugin's row is
 * distinguishable from another's in the log. */
static void dispatch_list_item_open(plugin_list_item_t * item, const char * kind) {
    lua_rawgeti(item->L, LUA_REGISTRYINDEX, item->open_ref);
    if (lua_pcall(item->L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(item->L, -1);
        fprintf(stderr, "[plugins] %s '%s' on_open error: %s\n", kind, item->label, err ? err : "unknown error");
        lua_pop(item->L, 1);
    }
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

/* Shared by plugin_manager_stream_tile_clicked() below (the only remaining
 * caller now that plugin_manager_tile_clicked() -- the old, single-slot
 * "Audio Books" dispatch -- is gone, replaced by the books-list-item
 * registry above). */
static void dispatch_tile_open(plugin_tile_t * t) {
    lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->open_ref);
    if (lua_pcall(t->L, 0, 0, 0) != LUA_OK) {
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

void plugin_manager_list_item_selected(int index) {
    if (!current_list_L || current_list_select_ref == LUA_NOREF) return;

    lua_rawgeti(current_list_L, LUA_REGISTRYINDEX, current_list_select_ref);
    lua_pushinteger(current_list_L, index + 1);
    if (lua_pcall(current_list_L, 1, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(current_list_L, -1);
        fprintf(stderr, "[plugins] show_list on_select error: %s\n", err ? err : "unknown error");
        lua_pop(current_list_L, 1);
    }
}
