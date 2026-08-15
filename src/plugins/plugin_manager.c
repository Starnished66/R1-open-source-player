#include "plugin_manager.h"
#include "gui.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
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

static lua_State * plugin_instances[PLUGIN_MAX_FILES];
static int plugin_instance_count = 0;

static plugin_tile_t plugin_tiles[PLUGIN_MAX_TILES];
static int plugin_tile_count = 0;

/* Separate registry for plugin.register_stream_media_tile() -- same
 * plugin_tile_t shape, different surface (gui.c's Stream Media screen,
 * not the Books -> "Audio Books" row). See PLUGIN_MAX_STREAM_TILES's own
 * comment in plugin_manager.h for why Stream Media gets real tiles when
 * Home doesn't. */
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
 *   plugin.register_tile(label, on_open [, icon])
 *     Registers the plugin's entry point. on_open(): lua_State function,
 *     called with no arguments when the plugin is opened -- currently: the
 *     first plugin to register is invoked from gui.c's Books -> "Audio
 *     Books" row (see audio_books_row_cb() and PLUGIN_MAX_TILES's own
 *     comment in plugin_manager.h for why it isn't its own Home-screen
 *     tile). label/icon are accepted (icon: optional theme2-relative asset
 *     path, e.g. "launcher/dac.png", "_s" variant derived automatically)
 *     for a future picker UI once more than one plugin is reachable;
 *     unused today.
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
 *     Same shape as register_tile() above, but appended as a real
 *     icon-grid tile in gui.c's Stream Media screen (after the built-in
 *     Subsonic tile) rather than routed through Books -> "Audio Books" --
 *     for plugins that thematically belong there (a Net Radio source, or
 *     similar). Up to PLUGIN_MAX_STREAM_TILES (plugin_manager.h) across
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
 * ---- */

/* Shared by l_plugin_register_tile()/l_plugin_register_stream_media_tile()
 * -- fills t->icon/icon_selected from an explicit icon string (deriving
 * the "_s" selected-state variant the same way every real icon pair in
 * this app is named), or from (default_icon, default_icon_selected) if
 * icon is NULL. */
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

static int l_plugin_register_tile(lua_State * L) {
    const char * label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char * icon = (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? luaL_checkstring(L, 3) : NULL;

    if (plugin_tile_count >= PLUGIN_MAX_TILES) {
        return luaL_error(L, "too many plugin tiles registered (max %d)", PLUGIN_MAX_TILES);
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    plugin_tile_t * t = &plugin_tiles[plugin_tile_count++];
    t->L = L;
    t->open_ref = ref;
    snprintf(t->label, sizeof(t->label), "%s", label);
    fill_tile_icon(t, icon, "launcher/book.png", "launcher/book_s.png");
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
 * to change what the Books tile looks like -- a different case from
 * plugin.register_tile()/register_stream_media_tile()'s own icon argument
 * (which points a BRAND NEW tile at whatever icon it likes with no special
 * handling needed). Copies source_path's bytes into
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

static const luaL_Reg plugin_funcs[] = {
    { "register_tile",             l_plugin_register_tile },
    { "register_stream_media_tile", l_plugin_register_stream_media_tile },
    { "show_list",                 l_plugin_show_list },
    { "list_dir",                  l_plugin_list_dir },
    { "sd_root",                   l_plugin_sd_root },
    { "play_file",                 l_plugin_play_file },
    { "play_list",                 l_plugin_play_list },
    { "show_toast",                l_plugin_show_toast },
    { "set_icon",                  l_plugin_set_icon },
    { "set_background_color",      l_plugin_set_background_color },
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

int plugin_manager_get_tile_count(void) {
    return plugin_tile_count;
}

const char * plugin_manager_get_tile_label(int index) {
    if (index < 0 || index >= plugin_tile_count) return "";
    return plugin_tiles[index].label;
}

const char * plugin_manager_get_tile_icon(int index) {
    if (index < 0 || index >= plugin_tile_count) return "launcher/book.png";
    return plugin_tiles[index].icon;
}

const char * plugin_manager_get_tile_icon_selected(int index) {
    if (index < 0 || index >= plugin_tile_count) return "launcher/book_s.png";
    return plugin_tiles[index].icon_selected;
}

/* Shared by plugin_manager_tile_clicked()/_stream_tile_clicked() below. */
static void dispatch_tile_open(plugin_tile_t * t) {
    lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->open_ref);
    if (lua_pcall(t->L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(t->L, -1);
        fprintf(stderr, "[plugins] tile '%s' on_open error: %s\n", t->label, err ? err : "unknown error");
        lua_pop(t->L, 1);
    }
}

void plugin_manager_tile_clicked(int index) {
    if (index < 0 || index >= plugin_tile_count) return;
    dispatch_tile_open(&plugin_tiles[index]);
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
