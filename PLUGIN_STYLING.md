# 🎨 Plugin Styling API

Every way a plugin can change how the player *looks* -- icons, screen
layout/reorder/restyle, colors, a wallpaper, and the small helpers that
support them. Split out of `PLUGINS.md` into its own doc because this
corner of the API has a lot of table shapes and clamped ranges that are
easy to lose track of mixed in with playback/networking/UI docs.

This guide is derived from `src/plugins/plugin_manager.c`,
`src/ui/gui.c`, and `src/ui/screen_builders.c`. If the guide and source
ever disagree, the source is authoritative. See `PLUGINS.md` for
everything else (lists, playback, networking, events) -- start there if
you haven't written a plugin before; this doc assumes you already have a
`plugin.define()`d script with at least one working entry point.

## 📖 Contents

- [`set_icon`](#set_icon) -- reskin one existing stock icon
- [`set_home_layout` / `set_screen_layout`](#set_screen_layout) -- reorder,
  hide, and restyle a screen's native tiles/rows; the big one, most of this
  doc
  - [Per-item fields](#per-item-fields) -- the full table, one row per field
  - [Per-screen `options` fields](#per-screen-options-fields)
  - [Which `screen_id`s exist, and their native keys](#screen-ids)
  - [Title styling](#title-styling)
- [`set_background_color` / `set_text_color`](#colors) -- the 5 global
  color slots
- [`set_background_image`](#set_background_image) -- app-wide wallpaper
- [`lerp_color`](#lerp_color) -- 2-stop gradient math
- [Row images, resizing, and text size](#row-images) -- the icon/height/
  width/text_size fields shared with `register_list_item()`/`show_list()`/
  `show_settings_list()` (not styling-only, documented here since it's the
  same field shapes as everything above)
- [Full worked example](#full-example)

<a id="set_icon"></a>

## `plugin.set_icon(theme2_relative_path, source_file_path)`

Reskins an **existing** tile's icon in place -- a different case from
`register_stream_media_tile()`'s own `icon` argument (which points a
brand-new tile at whatever icon it likes, no special handling needed) and
from `register_list_item()`'s/`show_list()`'s own `icon` option (which
puts a new icon on a row that has none, from an arbitrary SD-card file
rather than an existing theme2 asset -- see [Row images](#row-images)
below). This function is specifically for changing what an
*already-existing* stock icon looks like:

```lua
plugin.set_icon("launcher/book.png", plugin.sd_root() .. "/my_icon.png")
```

changes what Home's Books tile looks like.

Copies `source_file_path`'s bytes into the app's writable theme-override
directory under `theme2_relative_path`, the same mechanism this app
already uses for its own non-stock icons (e.g. Subsonic's). A user can
wipe every override this (or any other) plugin ever wrote via
**Settings > System > Reset Theme Icons**, which reboots the app back to
every stock icon.

**Call this from your plugin's top-level script code only** (i.e. during
load, not from inside `on_open`/`on_select`/any callback that might run
after the app has already shown a screen). LVGL caches decoded images by
path string for the app's whole lifetime, with no cache-invalidation call
anywhere in this codebase -- an override written *after* the first time
that path was resolved+decoded this boot will silently not take effect
until the next full app restart. Since every plugin's top-level code runs
during `plugin_manager_init()`, which always runs before `gui_init()`
builds any icon-grid/pill-list screen, calling this at load time is
always safe.

Also worth knowing: a tile's on-screen icon size/position is computed
once, at screen-build time, from the *original* icon's dimensions, and
never recomputed on a later icon change -- a replacement PNG with a very
different aspect ratio than the original will render at a size/position
tuned for the original, not the replacement. Keep replacement icons
roughly the same aspect ratio as what they're replacing.

`theme2_relative_path` is validated against path traversal -- a leading
`/` or any `..` component raises a Lua error rather than writing outside
the theme-override directory.

<a id="set_screen_layout"></a>

## `plugin.set_home_layout(tile_keys [, options])` / `plugin.set_screen_layout(screen_id, item_keys [, options])`

One mechanism, two entry points. `set_home_layout()` is sugar for
`set_screen_layout("home", tile_keys, options)` -- kept as its own
shorthand since it's the one every existing plugin already calls; Home is
just one of 7 screens this mechanism reaches.

Both reorder and/or hide a screen's native tiles/rows, optionally as a
plain vertical list instead of an icon grid (where the screen has one),
with per-item style overrides (background/text color, corner radius,
border, background opacity) and per-screen layout options (row/tile
spacing, title styling).

```lua
-- Icon grid: only System, DAC, and Music, in that order, System restyled
plugin.set_home_layout({
    "music",
    { key = "system", bg_color = 0x2a1a4e, text_color = 0xffcc00, radius = 24 },
    "dac",
})

-- Reorder + restyle Music's tiles, semi-transparent over a wallpaper
plugin.set_screen_layout("music", {
    { key = "all_songs", bg_color = 0x104020, bg_alpha = 160, text_color = 0x66ffaa },
    "artists",
    "albums",
})
```

Each entry in `tile_keys`/`item_keys` is either a plain string (that
item, unstyled) or a table `{ key = "...", ... }` to also style that one
item -- the same "string, or a table for the rows that need more" shape
`register_list_item()`'s/`show_list()`'s own `items` arrays already use.
Any field left out of a table entry (or every entry left a plain string)
keeps that item's default appearance.

**Call this from your plugin's top-level script code only**, same
load-time-only constraint as `set_icon()` above -- every screen is built
once, right after every plugin finishes loading, so a call from inside a
callback is a silent no-op until the next restart. If more than one
plugin targets the same `screen_id`, whichever call happens last wins
(same as `set_background_color()`/`set_text_color()`'s own single global
slots) -- one call always fully specifies both the layout and every style
override together for that screen, never layered onto an earlier call.
Different screens don't conflict -- `set_home_layout(...)` and
`set_screen_layout("music", ...)` from the same or different plugins
apply independently.

Raises a Lua error -- a screen must never end up with zero items -- for
an empty array, an unknown or duplicate item key, an unknown `screen_id`,
or an invalid enum value (`mode`, `align`, `text_size`, `title_align`,
`title_size`). Purely cosmetic numeric fields (`radius`, `bg_alpha`,
`border_width`, `row_gap`, `tile_gap`, `height`, `width`) are clamped into
range instead of erroring. Whatever layout was already in effect (the
native default, or an earlier successful call) is left untouched by a
call that errors.

<a id="per-item-fields"></a>

### Per-item fields

Every field below is optional. `icon` applies in both tile and list mode;
everything under "list mode only" is silently ignored in tile mode (the
icon grid has a fixed centered icon+label layout with no room for these).

| Field | Type | Default | Range / values | Modes | Notes |
|---|---|---|---|---|---|
| `key` | string | -- (required) | one of that screen's native keys | both | See [screen_id table](#screen-ids). |
| `bg_color` | `0xRRGGBB` int | native | any | both | Tile/row background fill. |
| `text_color` | `0xRRGGBB` int | native | any | both | Label (and, in list mode, chevron) color. |
| `radius` | int, px | native | `0`-`64` | both | Corner radius. |
| `bg_alpha` | int | `255` (opaque) | `0`-`255` | both | LVGL `lv_opa_t` scale. Lower values show [`set_background_image()`](#set_background_image)'s wallpaper through. |
| `border_color` | `0xRRGGBB` int | none | any | both | Inert alone -- needs `border_width > 0` too. |
| `border_width` | int, px | `0` (no border) | `0`-`8` | both | `0`/omitted draws no border regardless of `border_color`. |
| `icon` | bool | `true` (shown) | -- | both | `false` drops the item's launcher icon. Tile mode: label alone, centered. List mode: also reclaims the space normally reserved for the icon, so `align = "left"` sits flush against the row's real left edge. |
| `offset_x` | int, px (signed) | `0` (no shift) | `-150`-`150` | both | Pure visual reposition from the item's normal centered position -- negative shifts left, positive shifts right. A real coordinate shift (LVGL's flex/grid layouts fold it into their own placement math), not a draw-only effect, so touch hit-testing moves with it. The realistic use case is an alternating `+N`/`-N` by row/item index for a staggered "brick" list -- see the [full example](#full-example). |
| `height` | int, px | native (124px) | `48`-`220` | **list only** | Resizes the row. |
| `width` | int, px | native (448px) | `240`-`~608px` (device width dependent) | **list only** | Resizes and re-centers the row. |
| `align` | string | `"left"` | `"left"`, `"center"`, `"right"` | **list only** | Only the label's text position -- the icon (if shown) always stays at the row's left edge. |
| `accessory` | bool | `true` (shown) | -- | **list only** | `false` hides the trailing `">"` chevron. |
| `text_size` | string | `"medium"`-ish (native) | `"small"`, `"medium"`, `"large"`, `"mono"` | **list only** | `"mono"` is an 8x16 monospace/pixel bitmap font, ASCII-only -- accented/non-Latin characters render blank. |

```lua
-- A tight, chevron-free, monospace list flush to the left edge --
-- e.g. a retro boot-menu look
plugin.set_home_layout({
    { key = "music", bg_color = 0x141824, text_color = 0xe8d16b,
      radius = 0, height = 64, align = "left", accessory = false, text_size = "mono", icon = false },
    { key = "system", bg_color = 0x141824, text_color = 0xe0a1c4,
      radius = 0, height = 64, align = "left", accessory = false, text_size = "mono", icon = false },
}, { mode = "list", row_gap = 6 })
```

<a id="per-screen-options-fields"></a>

### Per-screen `options` fields

Passed as the 2nd argument to `set_home_layout()` / 3rd to
`set_screen_layout()`.

| Field | Type | Default | Range / values | Applies to | Notes |
|---|---|---|---|---|---|
| `mode` | string | `"tile"` | `"tile"`, `"list"` | icon-grid-native screens only | `"tile"` = the icon grid; `"list"` = every item becomes a plain row with icon + chevron. Silently ignored on the 3 already-list-native screens (`settings`, `books`, `dac`) -- they have no tile-grid equivalent to switch to, and are always in the list-mode shape. |
| `row_gap` | int, px | `6` | `0`-`24` | list mode (native or `mode="list"`) | Vertical spacing between rows. |
| `tile_gap` | int, px | `0` (flush cells, thin divider lines) | `0`-`40` | tile mode | Visible space between adjacent tiles; a nonzero value insets each tile within its grid cell and drops the divider lines (redundant once a real gap exists). |
| `title_align` | string | `"left"` | `"left"`, `"center"`, `"right"` | every screen with a title | See [Title styling](#title-styling). |
| `title_size` | string | native (28px) | `"small"`, `"medium"`, `"large"`, `"mono"` | every screen with a title | A larger scale than item `text_size` (`22`/`28`/`40px`) -- see [Title styling](#title-styling) for why. |
| `title_underline` | bool | `false` | -- | every screen with a title | Adds an underline text-decoration to the title label. |

<a id="screen-ids"></a>

### Which `screen_id`s exist, and their native keys

| `screen_id` | Shape | Native item keys |
|---|---|---|
| `"home"` | icon grid or list | `"music"`, `"stream_media"`, `"wireless"`, `"books"`, `"system"`, `"dac"` |
| `"music"` | icon grid or list | `"files"`, `"artists"`, `"albums"`, `"album_artist"`, `"all_songs"`, `"playlists"` |
| `"wireless"` | icon grid or list | `"wifi"`, `"bt"`, `"airplay"`, `"dlna"`, `"remote"`, `"import"` |
| `"stream_media"` | icon grid or list | `"subsonic"` (the only native item -- any `plugin.register_stream_media_tile()` tiles append after it, untouched by this) |
| `"settings"` | list only | `"playback"`, `"display"`, `"power"`, `"system"`, `"about"` (plugin rows from `register_list_item("settings", ...)` append after) |
| `"books"` | list only | `"books"`, `"favorites"` (plugin rows from `register_list_item("books", ...)` append after) |
| `"dac"` | list only | `"usb_dac"`, `"bluetooth_dac"` |

Home's icon grid is a fixed, non-scrollable 2x3 layout (matching the real
stock launcher, confirmed on real hardware) -- there's no way to add a 7th
tile of your own to Home (see `register_stream_media_tile()`/
`register_list_item()` in `PLUGINS.md` for where a plugin *can* get its
own entry point); `set_home_layout()`/`set_screen_layout("home", ...)`
only reorders/hides/restyles the existing 6.

`"settings"`, `"books"`, and `"dac"` are already a plain row list natively
(no icon-grid equivalent) -- `options.mode` is silently ignored for these
three, and every list-mode-only per-item field (`height`/`width`/`align`/
`accessory`/`text_size`) applies to them unconditionally, not just under
`{ mode = "list" }` like the other four. `icon` applies everywhere,
tile mode included.

<a id="title-styling"></a>

### Title styling

`options.title_align`/`title_size`/`title_underline` style the screen's
own title label (e.g. the "Music" or "Settings" text above the grid/list)
-- separate from every per-item field above, which only ever touches
tiles/rows, never the title.

```lua
plugin.set_home_layout(
    { "music", "system", "dac" },
    { mode = "list", title_align = "center", title_size = "large", title_underline = true }
)
```

Two things to know about `title_align = "center"`/`"right"` specifically:

- **Home in list mode is the one screen where this matters most.** Home
  has no back button (it's the navigation root), so its title spans the
  *full* screen width when re-aligned -- true screen-center, not "center
  of whatever space happens to be left." Home in tile mode has no title
  at all (matches the real stock launcher), so title options are a no-op
  there.
- **Every other screen has a back button.** Re-aligning their title keeps
  the same left-inset box the back arrow already reserves, so centered/
  right-aligned text still clears the arrow instead of overlapping it --
  it's "centered within the available space next to the arrow," not
  "centered across the raw screen width."

`title_size` accepts the same `"small"`/`"medium"`/`"large"`/`"mono"`
names as the per-item `text_size` field above, but maps to a **different,
larger** scale: the title's own native default is already `28px`
(equivalent to item `text_size = "large"`), so reusing the item scale
directly would make `title_size = "large"` land exactly on that default
-- a silent no-op. `"small"` is `22px`, `"medium"` is the native `28px`
(no visible change from the default), and `"large"` is `40px` --
genuinely, noticeably bigger, regardless of the device's own
accessibility font-size setting.

<a id="colors"></a>

## `plugin.set_background_color(slot, rgb)` / `plugin.set_text_color(slot, rgb)`

Five global color slots, live, app-wide, no restart needed -- unlike
`set_icon()`/`set_home_layout()`/`set_screen_layout()`, these are safe to
call at any time, including from inside a callback well after load. Each
is a plain style-property update plus a redraw request, no file/cache
involved.

| Function | `slot` | Affects |
|---|---|---|
| `set_background_color` | `"screen"` | Every screen's own background. |
| `set_background_color` | `"card"` | Every popup, EQ card, and settings slider card -- every neutral dark surface that isn't a plain screen or a list row. |
| `set_background_color` | `"list_row"` | Every row in every list screen (All Songs, Artists, Playlists, Files, Queue, ...). |
| `set_text_color` | `"primary"` | The app's dominant near-white text (labels, titles, list row text). |
| `set_text_color` | `"muted"` | Secondary/disabled-ish gray text (chevrons, timestamps, subtitles). |

`rgb` is a packed `0xRRGGBB` integer for both functions, e.g. `0x1E1E22`.
Destructive-red text (delete/reset confirmations) and accent-tinted text
are **not** covered by either `set_text_color()` slot -- they're
semantically fixed, not part of the light/dark split these slots (and
`set_background_color()`) drive.

Raises a Lua error if `slot` isn't one of that function's own recognized
names.

<a id="set_background_image"></a>

## `plugin.set_background_image(image_path)`

Sets a whole-screen wallpaper, live, app-wide, no restart needed -- every
screen shows it behind its own content, not just Home. Mutates the same
shared style `set_background_color("screen", ...)` uses, so the two
compose naturally: the wallpaper draws over that color, and a missing or
corrupt `image_path` just falls back to whatever `"screen"` color is set,
no error.

`image_path` follows the same convention every other plugin-supplied-file
option uses (`register_list_item()`'s own `icon`, for instance): a raw
absolute path, or one relative to the SD card's `.plugins/` folder (the
same folder your own `.lua` file lives in).

For a tile/row to show the wallpaper through instead of fully covering
it, give it `bg_alpha` (see [per-item fields](#per-item-fields) above)
below `255`.

```lua
plugin.set_background_image("wallpaper.jpg")
plugin.set_home_layout({
    { key = "music", bg_color = 0x1c2a36, bg_alpha = 200 },
    "system",
}, { mode = "list" })
```

<a id="lerp_color"></a>

## `plugin.lerp_color(from, to, t)`

Linearly interpolates each RGB channel independently between two packed
`0xRRGGBB` colors, `from` and `to`, at `t` (a float, `0.0`-`1.0`, clamped
into range). Returns a packed `0xRRGGBB` integer. Pure math, no UI effect
by itself -- use it to build an N-stop gradient across a screen's tiles
from just two endpoint colors instead of hand-picking every stop's own
hex value:

```lua
local keys = { "music", "stream_media", "wireless", "books", "system", "dac" }
local layout = {}
for i, key in ipairs(keys) do
    local t = (i - 1) / (#keys - 1)
    layout[i] = { key = key, bg_color = plugin.lerp_color(0x1c2a36, 0x6f93ab, t) }
end
plugin.set_home_layout(layout)
```

`plugins_examples/HomeThemes.lua` is the reference implementation: 11
ready-made Home looks (Game Boy, Terminal, Monastic, Wavy, Trees, Swamp,
Mountain Sunset, Zen Terracotta, Retro, Vaporwave, Earthy) spanning both
modes and every field documented above -- each one a genuinely different
shape/spacing/alignment recipe, not just a different palette on the same
skeleton -- picked from a Settings row and persisted the same way
`Themes.lua`'s own light/dark picker is. Each preset also calls
`set_background_color()`/`set_text_color()` (all 5 slots above) to match
the same palette, so the theme carries app-wide -- every screen's
background, every list row, every card/popup, and both text tiers -- not
just Home. Screenshots of all 11 are in
`plugins_examples/screenshots/home_themes/`.

<a id="row-images"></a>

## Row images, resizing, and text size

Not styling-only -- `register_list_item()`'s `options` table (see
`PLUGINS.md`), `show_list()`'s per-row table entries, and
`show_settings_list()`'s per-row tables all support these same optional
fields, documented once here since the shapes are identical. For
`show_list()`, width/height live in the call-level `options` table
instead of per-row, so every browsing row in that list stays uniform.

| Field | Type | Applies to | Notes |
|---|---|---|---|
| `icon` | string | all three | A **raw absolute filesystem path** (e.g. `plugin.sd_root() .. "/.plugins/my_icon.png"`) -- **not** a theme2-relative path like `register_stream_media_tile()`'s `icon` or `set_icon()`'s first argument -- **or** a plain relative filename/path (doesn't start with `/`), resolved against `<SD card>/.plugins/`. Any file your plugin can read works, including one it downloaded at runtime. A missing/corrupt file just renders at native size rather than erroring. |
| `height` | number, px | all except `show_settings_list()` `"slider"` rows | Clamped to a fixed range (currently 100-220px). Unset/zero keeps that row type's own default (124px pill row, 84px plain `show_list` row). |
| `width` | number, px | all three, sliders included | Clamped to 240-464px. Unset/zero keeps the native width. Resizing a pill row replaces its fixed-size background sprite with a matching rounded fill so artwork is never stretched. |
| `text_size` | string | all three | `"small"`, `"medium"`, or `"large"` -- every size uses a font with full non-Latin fallback (Cyrillic, CJK, Korean, Thai), correct for plugin-authored text which might not be English (unlike `set_home_layout()`/`set_screen_layout()`'s item `text_size`, this trio has no `"mono"` option). An unrecognized value raises a Lua error; omitting it keeps that row type's own existing default size. |

None of the three affect a row that doesn't set them -- a plugin that
never uses this section's fields renders exactly as it did before they
existed.

<a id="full-example"></a>

## Full worked example

A themed Home + Music, wallpaper, semi-transparent gradient tiles,
centered underlined titles, app-wide color cohesion -- most of this doc's
surface area in one script:

```lua
plugin.define({ id = "org.example.celestial", name = "Celestial", version = "1.0.0", api_min = 1 })

plugin.set_background_image("wallpaper.jpg")

local LIGHT, DARK, GOLD = 0x4a5f74, 0x28394a, 0xf0e2bf

local function themed_layout(keys)
    local layout = {}
    for i, key in ipairs(keys) do
        local t = (i - 1) / math.max(1, #keys - 1)
        layout[i] = {
            key = key,
            bg_color = plugin.lerp_color(LIGHT, DARK, t),
            bg_alpha = 120,
            text_color = GOLD,
            radius = 0,
            border_color = GOLD,
            border_width = 2,
            text_size = "large",
            align = "center",
            height = 64,
            -- Staggered "brick" look: row 1 left, row 2 right, row 3 left, ...
            offset_x = (i % 2 == 1) and -40 or 40,
        }
    end
    return layout
end

plugin.set_home_layout(
    themed_layout({ "music", "stream_media", "wireless", "books", "system", "dac" }),
    { mode = "list", row_gap = 8, title_align = "center", title_size = "large", title_underline = true }
)

plugin.set_screen_layout("music",
    themed_layout({ "files", "artists", "albums", "album_artist", "all_songs", "playlists" }),
    { mode = "list", row_gap = 8 }
)

plugin.set_background_color("card", 0x1f2c38)
plugin.set_background_color("list_row", DARK)
plugin.set_text_color("primary", GOLD)
plugin.set_text_color("muted", 0xa89a78)
```
