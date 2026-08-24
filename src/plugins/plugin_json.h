#ifndef PLUGIN_JSON_H
#define PLUGIN_JSON_H

#include "lua.h"

/* Lua bindings for plugin.json_decode(text [, limits]) and
 * plugin.json_encode(value [, limits]). Success: value, nil.
 * Failure: nil, error. See PLUGINS.md. */
int l_plugin_json_decode(lua_State * L);
int l_plugin_json_encode(lua_State * L);

#endif /* PLUGIN_JSON_H */