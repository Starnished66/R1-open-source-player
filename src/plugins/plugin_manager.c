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
#include <sys/stat.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point -- see gui.c's own MUSIC_ROOT_DIR comment; each
   * file that needs it defines its own copy rather than sharing a header,
   * matching gui.c/remote_control.c/metadata_db.c already doing the same. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
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
 * ---- */

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
    if (icon) {
        char base[80];
        snprintf(base, sizeof(base), "%s", icon);
        char * dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        snprintf(t->icon, sizeof(t->icon), "%s", icon);
        snprintf(t->icon_selected, sizeof(t->icon_selected), "%s_s.png", base);
    } else {
        snprintf(t->icon, sizeof(t->icon), "launcher/book.png");
        snprintf(t->icon_selected, sizeof(t->icon_selected), "launcher/book_s.png");
    }
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

static const luaL_Reg plugin_funcs[] = {
    { "register_tile", l_plugin_register_tile },
    { "show_list",     l_plugin_show_list },
    { "list_dir",      l_plugin_list_dir },
    { "sd_root",       l_plugin_sd_root },
    { "play_file",     l_plugin_play_file },
    { "play_list",     l_plugin_play_list },
    { "show_toast",    l_plugin_show_toast },
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

void plugin_manager_tile_clicked(int index) {
    if (index < 0 || index >= plugin_tile_count) return;
    plugin_tile_t * t = &plugin_tiles[index];

    lua_rawgeti(t->L, LUA_REGISTRYINDEX, t->open_ref);
    if (lua_pcall(t->L, 0, 0, 0) != LUA_OK) {
        const char * err = lua_tostring(t->L, -1);
        fprintf(stderr, "[plugins] tile '%s' on_open error: %s\n", t->label, err ? err : "unknown error");
        lua_pop(t->L, 1);
    }
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
