# `plugin.set_home_layout()` — Implementation Plan

## Why this is buildable now, and what's actually missing

`plugin.set_home_layout()` is referenced throughout `src/ui/screen_builders.c`/
`.h` (comments only) and used by the already-committed
`plugins_examples/HomeThemes.lua`, but it has no Lua binding anywhere in
`src/plugins/plugin_manager.c` — calling it today raises "attempt to call a
nil value". The rendering plumbing it needs, however, is already built and
sitting unused:

- `icon_grid_item_t` (`screen_builders.h:132-148`) already carries
  `has_bg_color/bg_color`, `has_text_color/text_color`, `has_radius/radius`
  per-tile overrides, and `build_icon_grid_screen()` already takes a
  `tile_gap` parameter (`screen_builders.h:178-180`) — both explicitly
  commented as existing "only" for this function's future use.
- `pill_list_item_t` (`screen_builders.h:188-260`) already carries the same
  three overrides plus `text_align`, and `build_pill_list_screen()` already
  takes a `row_gap` parameter — same "only `set_home_layout()` passes this"
  comments.
- `asset_path()` (`src/ui/assets.c:42`) already resolves a theme2-relative
  path (e.g. `"launcher/music.png"`) to a real absolute file, with any
  plugin `set_icon()` override already applied — reusable as-is to put a
  Home tile's real icon onto a list-mode row.

So this is wiring work, not new rendering infrastructure: a Lua binding, a
small config struct, and changes to one existing function
(`build_home_screen()`, `src/ui/gui_settings.c:1772`).

## Current state to change

`build_home_screen()` today (`gui_settings.c:1772-1786`) hardcodes a static
6-tile array and always calls
`build_icon_grid_screen(NULL, NULL, items, 6, 100, false, 0)` — no
per-tile overrides, `tile_gap` always `0`, no list-mode path at all. It's
called exactly once, from `gui_shell.c:3259` during startup, and (like every
other screen builder in this codebase) is never rebuilt afterward.

That "built once, at startup, never rebuilt" fact is what makes the rest of
this plan simple: `set_home_layout()` doesn't need to touch any live LVGL
object. It only needs to run *before* `build_home_screen()` does — which
`plugin_manager_init()` (called from `gui_init()`, before any screen is
built — see PLUGINS.md's "How Plugins Load") already guarantees for every
plugin call made from a plugin's own top-level script code. Exactly the same
ordering guarantee `plugin.set_icon()` already relies on, and exactly why
`HomeThemes.lua`'s own header comment says a call made later, from inside a
callback, only takes effect after a restart — not an error, just inert until
the next boot re-runs the plugin's top-level code and `build_home_screen()`
after it.

## Target Lua surface (already implied by `HomeThemes.lua` + the
`screen_builders.h` comments — this isn't a new design, just formalizing
what's already assumed)

```lua
plugin.set_home_layout(tiles, options)
```

`tiles` — array of tables, one per tile being overridden (omit a key to
leave that tile fully native):

```lua
{
    key = "music",        -- one of: music, stream_media, wireless, books, system, dac
    bg_color = 0xRRGGBB,  -- optional
    text_color = 0xRRGGBB,-- optional
    radius = 0,           -- optional, px
    -- list mode only, ignored in tile mode:
    height = 68, width = 440, align = "left", -- "left"/"center"/"right"
    accessory = true,      -- show the chevron
    text_size = "medium",  -- "small"/"medium"/"large" (see open question below)
    icon = true,            -- show the tile's native icon on its list row
}
```

`options`:

```lua
{
    mode = "list",   -- "tile" (default, today's grid) or "list"
    tile_gap = 6,    -- tile mode only
    row_gap = 10,    -- list mode only
}
```

An unrecognized `key`, an unrecognized `mode`, or an unrecognized `align`/
`text_size` should raise a Lua error immediately — same "fail loudly at the
Lua boundary" convention every other enum-like `plugin.*` argument already
uses (`list_id`, event names, EQ band type, row `text_size`).

## Data model

Add a small file-scope struct in `gui_settings.c` (where `build_home_screen()`
already lives) rather than `plugin_manager.c` — same split every other
`gui_plugin_*` bridge already follows (LVGL/screen-construction state stays
in the `gui.c`-family file that owns the screen; `plugin_manager.c` only
validates and hands off):

```c
typedef struct {
    bool set;
    bool has_bg_color;   uint32_t bg_color;
    bool has_text_color; uint32_t text_color;
    bool has_radius;     int32_t radius;
    /* list-mode-only fields */
    int32_t height, width;          /* 0 = unset */
    const char * align;             /* NULL = unset; owned copy, see below */
    bool has_accessory; bool accessory;
    const char * text_size;         /* NULL = unset; owned copy */
    bool has_icon; bool icon;
} home_tile_override_t;

typedef struct {
    bool configured;               /* false = plugin.set_home_layout() never called; use today's exact hardcoded build */
    bool list_mode;
    int32_t tile_gap;               /* tile mode */
    int32_t row_gap;                /* list mode, 0 = keep native default of 6 */
    home_tile_override_t tiles[6];  /* indexed by the same fixed KEYS order build_home_screen() already uses */
} home_layout_config_t;

static home_layout_config_t home_layout_config = { 0 };
```

String fields (`align`, `text_size`) need owned copies (small fixed buffers,
e.g. `char align[8]`) since the Lua strings backing them are only valid for
the duration of the call — same reasoning `plugin_storage`/`playlist_*`
already apply when they persist a Lua string past the call that produced it.

## `gui_plugin_set_home_layout()` bridge (new, in `gui_settings.c`, declared
in `gui.h` beside the other `gui_plugin_*` prototypes)

```c
bool gui_plugin_set_home_layout(const home_layout_config_t * config);
```

Just copies `*config` into the static `home_layout_config` above. Returns
`bool` only for symmetry with `eq_load_profile`-style bridges; realistically
this can't fail. No LVGL call in it at all — it never touches `home_screen`
directly, which is exactly why no "screen already built" edge case needs
handling.

## `l_plugin_set_home_layout()` (new, in `plugin_manager.c`, following the
`l_plugin_register_list_item()`/`l_plugin_show_settings_list()` pattern for
parsing an array of option tables)

1. `luaL_checktype(L, 1, LUA_TTABLE)` for `tiles`, `luaL_opttable`-style
   handling for `options` (table or absent).
2. For each entry in `tiles`: `luaL_checkstring` on `key`, match against the
   fixed `{"music","stream_media","wireless","books","system","dac"}` array
   `build_home_screen()` already uses — `luaL_error` on no match (same
   convention `list_id` validation uses).
3. Pull optional fields with `luaL_opt*`/manual `lua_getfield` + type checks:
   - `bg_color`/`text_color`: `luaL_checkinteger`, no range validation needed
     (masked to 24 bits the same way `set_background_color` already does).
   - `radius`: `luaL_checkinteger`, clamp into a sane range (reuse
     `PILL_ROW_HEIGHT_MIN/2` as an informal ceiling, or just leave
     unclamped since a corner radius larger than the row silently just
     looks fully rounded — LVGL itself clamps visually).
   - `height`/`width`: clamp to `PILL_ROW_HEIGHT_MIN..MAX` /
     `PILL_ROW_WIDTH_MIN..MAX` — the exact same constants
     `register_list_item()`'s own `options.height`/`width` already clamp to
     (`screen_builders.h:275-278`), so a Home list row behaves identically
     to a plugin-appended settings row of the same size.
   - `align`: must be `"left"`, `"center"`, or `"right"` — `luaL_error`
     otherwise.
   - `accessory`, `icon`: `lua_toboolean`.
   - `text_size`: validate with the same `is_valid_text_size()`
     (`plugin_manager.c:472`) every other `text_size` field already uses —
     see the open question below about `"mono"` before doing this.
4. Parse `options.mode` (`"tile"`/`"list"`/absent→`"tile"`), `options.tile_gap`,
   `options.row_gap` similarly.
5. Build a `home_layout_config_t` on the C stack and call
   `gui_plugin_set_home_layout(&config)`.
6. Add `{ "set_home_layout", l_plugin_set_home_layout }` to `plugin_funcs[]`
   (`plugin_manager.c:2742` block).

## `build_home_screen()` changes (`gui_settings.c:1772`)

```c
lv_obj_t * build_home_screen(void) {
    const home_layout_config_t * cfg = gui_settings_get_home_layout_config(); /* new getter */

    if (cfg->configured && cfg->list_mode) {
        static pill_list_item_t items[6];
        /* one entry per native tile, in the existing KEYS order, using
         * each tile's existing on_click callback (music_tile_cb, ...),
         * existing label, and cfg->tiles[i]'s overrides. icon=true resolves
         * via asset_path("launcher/music.png") etc. -- see below. */
        ...
        lv_obj_t * scr = build_pill_list_screen(NULL, NULL, items, 6,
                                                 gui_theme_accent_style(),
                                                 cfg->row_gap ? cfg->row_gap : 6);
        finalize_screen_navigation(scr);
        return scr;
    }

    static icon_grid_item_t items[6];
    /* existing 6 literals, unchanged, then apply cfg->tiles[i]'s
     * bg_color/text_color/radius overrides on top if cfg->configured */
    ...
    lv_obj_t * scr = build_icon_grid_screen(NULL, NULL, items, 6, 100, false,
                                             cfg->configured ? cfg->tile_gap : 0);
    finalize_screen_navigation(scr);
    return scr;
}
```

For the list-mode `icon = true` case: call
`pill_row_apply_icon`'s existing icon-path input with
`asset_path("launcher/music.png")` (and the matching path per tile) rather
than a raw SD-card path — `pill_list_item_t.icon_asset`'s doc comment
(`screen_builders.h:215-219`) says this field is meant for a raw filesystem
path, and `asset_path()` returns exactly that (already resolved through any
`set_icon()` override), so no new resolution code is needed, just the right
argument.

Each of the six `on_click` handlers (`music_tile_cb`, `stream_media_tile_cb`,
`wireless_tile_cb`, `gui_books_home_tile_cb`, `system_tile_cb`,
`dac_home_tile_cb`) already exists and is a plain
`lv_event_cb_t` — they plug directly into `pill_list_item_t.on_click`
unchanged.

## Open questions to settle before implementing

1. **`text_size = "mono"`.** `HomeThemes.lua`'s Game Boy/Terminal presets
   pass `text_size = "mono"`, but `is_valid_text_size()`
   (`plugin_manager.c:472-474`) only accepts `"small"/"medium"/"large"`, and
   `fallback_font.h`'s `app_font_*` family has no monospace member — adding
   real mono support means sourcing a monospace font file, generating an
   `app_font_mono` (or similar) LVGL font asset with the same non-Latin
   fallback treatment every other tier gets, and widening
   `pill_row_resolve_text_size()`/`is_valid_text_size()`. That's a real
   font-asset addition, not a wiring change — recommend shipping
   `set_home_layout()` first with only `small`/`medium`/`large`, and either
   (a) making `HomeThemes.lua` fall back `"mono"` → `"small"` until a real
   mono font exists, or (b) treating mono support as a fast-follow phase 2.
   Don't silently accept `"mono"` today and have it resolve to the wrong
   font — validate against exactly the tiers that exist.
2. **Missing keys.** A `tiles` array that omits a key (e.g. only overrides
   `"music"`) should leave the other five fully native for whichever fields
   weren't set — confirmed as the intent by `HomeThemes.lua` always passing
   all six, but a plugin that wants a partial override should get a
   sane partial result, not an error.
3. **Duplicate keys in one call.** Last-one-wins is the simplest rule and
   matches this codebase's general "don't error on redundant input, just
   take the final value" convention (e.g. a repeated HTTP response header).
4. **`radius` in tile mode.** `icon_grid_item_t.has_radius` exists but
   nothing today constrains what "radius on an icon tile" visually means —
   worth a quick real-device check once implemented (Retro/Vaporwave/Earthy
   presets rely on this) before calling it done.
5. **Capability token + API version.** This is a purely additive function
   like `play_remote`/`queue_remote_list` were in API 3 — recommend the same
   treatment: bump `PLUGIN_API_VERSION` to `4`, add a `"ui.home_layout"`
   capability token to `plugin_capabilities[]`
   (`plugin_manager.c:2562-2568`) so a plugin can feature-detect this one
   function without requiring `api_min = 4` outright, and add an "API
   version 4 changelog" section to `PLUGINS.md` mirroring the existing
   version 2/3 changelog format.

## Documentation and example fallout once implemented

- `PLUGINS.md`: new `### plugin.set_home_layout(tiles, options)` section
  under "Lists and Settings UI" or its own "Home Layout" heading, plus the
  API-4 changelog entry and capability-token addition to the existing
  `has_capability()` list.
- `plugins_examples/PluginApiInfo.lua`: add `"ui.home_layout"` to its
  `CAPABILITIES` table and a `"plugin.set_home_layout(tiles, options)"` row
  to the UI & Interaction category — the same file just brought current in
  this session.
- `plugins_examples/HomeThemes.lua`: re-verify against the real
  implementation once built, particularly the `"mono"` question above and
  the exact clamp behavior for `height`/`width`/`radius` on its more extreme
  presets (Game Boy's `height = 52` sits right at `PILL_ROW_HEIGHT_MIN`,
  worth confirming it doesn't clip).

## Suggested implementation order

1. `home_layout_config_t` + `gui_plugin_set_home_layout()` in `gui_settings.c`,
   with `configured = false` wired through `build_home_screen()` unchanged
   (i.e., land the plumbing with zero visible behavior change first).
2. `l_plugin_set_home_layout()` in `plugin_manager.c`, `small`/`medium`/`large`
   only, `luaL_error` on anything else including `"mono"` — ship this and
   confirm real-device behavior for tile-mode overrides (the simpler path:
   `bg_color`/`text_color`/`radius`/`tile_gap` only, reusing already-wired
   `icon_grid_item_t` fields) before touching list mode at all.
3. List-mode support in `build_home_screen()` (`pill_list_item_t` array +
   `asset_path()`-resolved icons) — the larger, riskier half; test against
   `HomeThemes.lua`'s Game Boy/Terminal/Monastic/Wavy/Trees/Swamp/Mountain
   Sunset/Zen Terracotta presets specifically, since between them they
   exercise every list-mode field.
4. Capability token, API version bump, `PLUGINS.md`/`PluginApiInfo.lua`
   updates.
5. Decide and act on the `"mono"` question (real font vs. documented
   fallback) as a fast-follow, not a blocker for the rest.
