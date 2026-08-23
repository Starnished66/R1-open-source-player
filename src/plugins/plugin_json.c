#include "plugin_json.h"

#include "lauxlib.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_INPUT_BYTES  (512U * 1024U)
#define JSON_MAX_OUTPUT_BYTES (512U * 1024U)
#define JSON_NESTING_DEFAULT  32
#define JSON_NESTING_CEILING  64
#define JSON_ENTRIES_MAX      10000

typedef struct {
    size_t max_input_bytes;
    size_t max_output_bytes;
    int max_nesting;
    int max_entries;
} json_limits_t;

static size_t clamp_size(lua_Integer v, size_t ceiling) {
    if (v <= 0) return ceiling;
    if ((uint64_t) v > (uint64_t) ceiling) return ceiling;
    return (size_t) v;
}

static int clamp_int(lua_Integer v, int fallback, int ceiling) {
    if (v <= 0) return fallback;
    if (v > ceiling) return ceiling;
    return (int) v;
}

static void parse_limits(lua_State * L, int index, json_limits_t * lim) {
    lim->max_input_bytes = JSON_MAX_INPUT_BYTES;
    lim->max_output_bytes = JSON_MAX_OUTPUT_BYTES;
    lim->max_nesting = JSON_NESTING_DEFAULT;
    lim->max_entries = JSON_ENTRIES_MAX;
    if (lua_isnoneornil(L, index)) return;
    luaL_checktype(L, index, LUA_TTABLE);

    lua_getfield(L, index, "max_input_bytes");
    if (lua_isnumber(L, -1)) lim->max_input_bytes = clamp_size(lua_tointeger(L, -1), JSON_MAX_INPUT_BYTES);
    lua_pop(L, 1);

    lua_getfield(L, index, "max_output_bytes");
    if (lua_isnumber(L, -1)) lim->max_output_bytes = clamp_size(lua_tointeger(L, -1), JSON_MAX_OUTPUT_BYTES);
    lua_pop(L, 1);

    lua_getfield(L, index, "max_nesting");
    if (lua_isnumber(L, -1)) lim->max_nesting = clamp_int(lua_tointeger(L, -1), JSON_NESTING_DEFAULT, JSON_NESTING_CEILING);
    lua_pop(L, 1);

    lua_getfield(L, index, "max_entries");
    if (lua_isnumber(L, -1)) lim->max_entries = clamp_int(lua_tointeger(L, -1), JSON_ENTRIES_MAX, JSON_ENTRIES_MAX);
    lua_pop(L, 1);
}

static bool utf8_ok(const char * s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char) s[i];
        uint32_t cp;
        int more;
        if (c <= 0x7F) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            more = 1;
            cp = c & 0x1F;
            if (c < 0xC2) return false;
        } else if ((c & 0xF0) == 0xE0) {
            more = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            more = 3;
            cp = c & 0x07;
            if (c > 0xF4) return false;
        } else {
            return false;
        }
        if (i + 1 + (size_t) more > n) return false;
        for (int k = 1; k <= more; k++) {
            unsigned char cc = (unsigned char) s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (more == 2 && cp < 0x800) return false;
        if (more == 3 && cp < 0x10000) return false;
        i += 1 + (size_t) more;
    }
    return true;
}

static bool json_nesting_ok(const char * s, size_t n, int max_nest) {
    int depth = 0;
    bool in_string = false, escape = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) s[i];
        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') {
            depth++;
            if (depth > max_nest) return false;
        } else if (c == '}' || c == ']') {
            if (depth > 0) depth--;
        }
    }
    return true;
}

static int fail(lua_State * L, const char * msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

static int ok_value(lua_State * L) {
    lua_pushnil(L);
    return 2;
}

static bool push_cjson(lua_State * L, const cJSON * node, int depth, const json_limits_t * lim, int * entries,
                       const char ** err) {
    if (depth > lim->max_nesting) {
        *err = "json too deeply nested";
        return false;
    }
    if (!node || cJSON_IsNull(node)) {
        lua_pushnil(L);
        return true;
    }
    if (cJSON_IsBool(node)) {
        lua_pushboolean(L, cJSON_IsTrue(node));
        return true;
    }
    if (cJSON_IsNumber(node)) {
        lua_pushnumber(L, node->valuedouble);
        return true;
    }
    if (cJSON_IsString(node)) {
        const char * s = node->valuestring ? node->valuestring : "";
        size_t n = strlen(s);
        if (!utf8_ok(s, n)) {
            *err = "invalid utf-8";
            return false;
        }
        lua_pushlstring(L, s, n);
        return true;
    }
    if (cJSON_IsArray(node) || cJSON_IsObject(node)) {
        lua_newtable(L);
        int i = 1;
        const cJSON * child;
        cJSON_ArrayForEach(child, node) {
            if (*entries >= lim->max_entries) {
                lua_pop(L, 1);
                *err = "too many entries";
                return false;
            }
            (*entries)++;
            if (cJSON_IsObject(node)) {
                if (!child->string || !utf8_ok(child->string, strlen(child->string))) {
                    lua_pop(L, 1);
                    *err = "invalid utf-8";
                    return false;
                }
            }
            if (!push_cjson(L, child, depth + 1, lim, entries, err)) {
                lua_pop(L, 1);
                return false;
            }
            if (cJSON_IsArray(node)) lua_rawseti(L, -2, i++);
            else lua_setfield(L, -2, child->string);
        }
        return true;
    }
    lua_pushnil(L);
    return true;
}

static cJSON * lua_to_cjson(lua_State * L, int index, int depth, const json_limits_t * lim, int * entries,
                            const char ** err) {
    if (depth > lim->max_nesting) {
        *err = "json too deeply nested";
        return NULL;
    }
    int type = lua_type(L, index);
    switch (type) {
        case LUA_TNIL:
            return cJSON_CreateNull();
        case LUA_TBOOLEAN:
            return cJSON_CreateBool(lua_toboolean(L, index));
        case LUA_TNUMBER:
            return cJSON_CreateNumber(lua_tonumber(L, index));
        case LUA_TSTRING: {
            size_t n = 0;
            const char * s = lua_tolstring(L, index, &n);
            if (!utf8_ok(s, n)) {
                *err = "invalid utf-8";
                return NULL;
            }
            return cJSON_CreateString(s);
        }
        case LUA_TTABLE: {
            if (index < 0) index = lua_gettop(L) + index + 1;
            lua_Integer len = lua_rawlen(L, index);
            bool is_array = len > 0;
            if (is_array) {
                lua_pushnil(L);
                while (lua_next(L, index) != 0) {
                    if (lua_type(L, -2) != LUA_TNUMBER) is_array = false;
                    lua_pop(L, 1);
                    if (!is_array) {
                        lua_pop(L, 1);
                        break;
                    }
                }
            }
            if (is_array) {
                cJSON * arr = cJSON_CreateArray();
                if (!arr) {
                    *err = "encode failed";
                    return NULL;
                }
                for (lua_Integer i = 1; i <= len; i++) {
                    if (*entries >= lim->max_entries) {
                        cJSON_Delete(arr);
                        *err = "too many entries";
                        return NULL;
                    }
                    (*entries)++;
                    lua_rawgeti(L, index, (int) i);
                    cJSON * child = lua_to_cjson(L, -1, depth + 1, lim, entries, err);
                    lua_pop(L, 1);
                    if (!child) {
                        cJSON_Delete(arr);
                        return NULL;
                    }
                    cJSON_AddItemToArray(arr, child);
                }
                return arr;
            }
            cJSON * obj = cJSON_CreateObject();
            if (!obj) {
                *err = "encode failed";
                return NULL;
            }
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                if (lua_type(L, -2) != LUA_TSTRING) {
                    lua_pop(L, 2);
                    cJSON_Delete(obj);
                    *err = "object keys must be strings";
                    return NULL;
                }
                size_t kn = 0;
                const char * key = lua_tolstring(L, -2, &kn);
                if (!utf8_ok(key, kn)) {
                    lua_pop(L, 2);
                    cJSON_Delete(obj);
                    *err = "invalid utf-8";
                    return NULL;
                }
                if (*entries >= lim->max_entries) {
                    lua_pop(L, 2);
                    cJSON_Delete(obj);
                    *err = "too many entries";
                    return NULL;
                }
                (*entries)++;
                cJSON * child = lua_to_cjson(L, -1, depth + 1, lim, entries, err);
                if (!child) {
                    lua_pop(L, 2);
                    cJSON_Delete(obj);
                    return NULL;
                }
                cJSON_AddItemToObject(obj, key, child);
                lua_pop(L, 1);
            }
            return obj;
        }
        default:
            *err = "cannot encode value";
            return NULL;
    }
}

int l_plugin_json_decode(lua_State * L) {
    size_t len = 0;
    const char * s = luaL_checklstring(L, 1, &len);
    json_limits_t lim;
    parse_limits(L, 2, &lim);
    if (len > lim.max_input_bytes) return fail(L, "input too large");
    if (!utf8_ok(s, len)) return fail(L, "invalid utf-8");
    if (!json_nesting_ok(s, len, lim.max_nesting)) return fail(L, "json too deeply nested");

    cJSON * json = cJSON_ParseWithLength(s, len);
    if (!json) return fail(L, "invalid json");

    int entries = 0;
    const char * err = "invalid json";
    if (!push_cjson(L, json, 0, &lim, &entries, &err)) {
        cJSON_Delete(json);
        return fail(L, err);
    }
    cJSON_Delete(json);
    return ok_value(L);
}

int l_plugin_json_encode(lua_State * L) {
    luaL_checkany(L, 1);
    json_limits_t lim;
    parse_limits(L, 2, &lim);
    const char * err = "cannot encode value";
    int entries = 0;
    cJSON * json = lua_to_cjson(L, 1, 0, &lim, &entries, &err);
    if (!json) return fail(L, err);
    char * printed = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!printed) return fail(L, "encode failed");
    size_t n = strlen(printed);
    if (n > lim.max_output_bytes) {
        cJSON_free(printed);
        return fail(L, "output too large");
    }
    if (!utf8_ok(printed, n)) {
        cJSON_free(printed);
        return fail(L, "invalid utf-8");
    }
    lua_pushlstring(L, printed, n);
    cJSON_free(printed);
    return ok_value(L);
}