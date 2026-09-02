# 🧩 Writing Plugins

Build third-party features for Open Source Player with plain Lua
— no C toolchain, player rebuild, or firmware reflash required.

This guide is derived from `src/plugins/plugin_manager.c` and the scripts in
`plugins_examples/`. If the guide and source ever disagree, the source is
authoritative.

## 🧭 Start Here

1. Create a `.lua` file.
2. Give the plugin an identity with `plugin.define()`.
3. Register a row or Stream Media tile so users can open it.
4. Copy it to `<SD card>/.plugins/`.
5. Restart the player. Plugins are scanned only during startup.

### Minimal plugin

```lua
plugin.define({
    id = "org.example.hello",
    name = "Hello Player",
    version = "1.0.0",
    api_min = 1,
})

plugin.register_list_item("settings", "Hello Player", function()
    plugin.show_list("Hello Player", {
        "The plugin is working!",
        "Lua says hello 👋",
    }, function(index)
        plugin.show_toast("Selected row " .. index)
    end)
end)
```

Install it as:

```text
<SD card>/.plugins/HelloPlayer.lua
```

> [!TIP]
> Start with an example close to what you want to build. `Audiobooks.lua`,
> `NetRadio.lua`, `Themes.lua`, and `LastFmScrobbler.lua` cover most common
> plugin shapes.

## 📖 Guide Map

- [Loading and isolation](#loading)
- [Adding a plugin to the UI](#ui-entry-points)
- [Identity and capability checks](#identity)
- [Lists and settings screens](#plugin-ui)
- [Files and playback](#files-playback)
- [Networking](#networking)
- [Playback events and timers](#events)
- [Complete examples](#examples)
- [Testing your plugin](#testing)

<a id="loading"></a>

## ⚙️ How Plugins Load

At startup (`plugin_manager_init()`, called early in `gui_init()`, well
before anything could tap into one), every `*.lua` file directly under
`<SD card>/.plugins/` (`/data/mnt/sd_0/.plugins/` on the real device,
`./music/.plugins/` on the host simulator) is loaded, one at a time, into
its **own** `lua_State` (`luaL_newstate()` + `luaL_openlibs()`), with the
`plugin` table (below) injected before the file itself runs
(`luaL_dofile()`). Subdirectories and anything not ending in `.lua` are
skipped. Every eligible filename is collected and sorted in **ascending
order** (byte-wise, case-sensitive) -- not raw directory order, which is
filesystem-dependent and can silently change after copying or reinstalling
files -- before the first 16 (`PLUGIN_MAX_FILES`) of them are actually
loaded; with more than 16 `.lua` files present, it's always the
alphabetically-first 16, never whichever 16 the filesystem happened to
return first. This matters for every plugin API backed by a single
global slot that any plugin can overwrite (`set_background_color()`,
`set_text_color()`, `set_icon()`, `set_home_layout()`): if two installed
plugins both set the same one, the alphabetically-last plugin's own call
wins, reproducibly.

Each `lua_State` is kept open for the rest of the app's lifetime, never
closed -- a plugin's callbacks (`on_open`, a `show_list` row's
`on_select`) are Lua closures that need their owning state alive to be
invoked later, from a tap that can happen minutes after load.

If a file fails to load or errors while running its top-level code
(`luaL_dofile()` returning non-`LUA_OK`), that one file is skipped -- logged
to stderr as `[plugins] failed to load <path>: <error>` -- without
affecting any other `.lua` file in the folder.

### Sandboxing

A plugin is just a `.lua` file dropped into `.plugins/` -- this app treats
that the same as any other SD-card-supplied content, not as fully trusted
code, so `luaL_openlibs()`'s full standard library is trimmed right after
each `lua_State` is created (`sandbox_plugin_lua_state()`):

- **Removed entirely**: `load`, `loadstring`, `loadfile`, `dofile`,
  `require`, `package`, `debug`. Every one of the 7 bundled plugins is a
  single self-contained file with no multi-file/`require()` pattern
  (confirmed by actually checking, not assumed), so this doesn't take
  away anything a currently-shipped plugin needs -- but see the API
  version 2 changelog above: a *third-party* plugin that does split its
  own code across files via `require()`/`dofile()` will break.
- **Removed from `os`**: `execute`, `getenv`, `exit`, `tmpname`.
  `os.time`/`os.date`/`os.clock`/`os.difftime`/`os.remove`/`os.rename`
  are still available (see below for the last two).
- **Removed from `io`**: only `io.popen` (a shell-exec primitive, not
  file I/O). The rest of `io`, including `io.open`, stays -- all 7
  bundled plugins genuinely use `io.open()` to persist their own small
  state file under `plugin.sd_root() .. "/.plugins/..."`, and 3 of them
  use `os.remove()`/`os.rename()` for an atomic write-then-rename, the
  same pattern this app's own C code uses elsewhere. An earlier version
  of this sandbox removed all of `io` and `os.remove`/`os.rename`
  entirely; real-device testing against the actual installed plugins
  showed that broke real, legitimate use and was reverted.

**This means plain file I/O is not otherwise restricted** -- `io.open()`
can open any path the OS-level user can, same risk class as this app's
own file I/O, not a privilege escalation, with one deliberate exception:
`io.open()`, `io.lines()`, `io.input()`, `io.output()`, `os.remove()`,
and `os.rename()` all refuse any path that resolves into the internal
`plugin.storage`/`plugin.secrets` tree (`/usr/data/plugins/`) -- see that
section below for why this specific guard exists. `io.input()`/
`io.output()` only trigger the check when actually given a filename
(selecting/opening a path); calling either with no argument, or with an
already-open file handle, passes through unchanged since neither is a
path to check. Outside that one reserved tree, a plugin can still read,
write, or delete anything the SD card or `list_dir()`/`sd_root()` already
exposed to it; that has not changed.

**The same guard also applies to every native `plugin.*` function that
takes a caller-supplied filesystem path**, not just Lua's own `io`/`os`
entry points above -- these reach the filesystem directly in C and would
otherwise bypass the `io`/`os` wrappers entirely: `plugin.list_dir()`,
`plugin.mkdir()`, `plugin.play_file()`/`plugin.play_list()`,
`plugin.set_icon()`'s source path (its second argument -- the first,
`relative_path`, isn't an arbitrary filesystem path to begin with, see
that function's own doc below), `plugin.eq_load_profile()`/
`plugin.eq_save_profile()`, and `plugin.download_file_async()`'s
destination. All of these now raise a clean error naming the reserved
path rather than silently touching it. `plugin.download_file_async()`
checks twice -- once before its background worker even starts, and again
inside that worker immediately before it touches the filesystem -- so the
guard can't be skipped by a future code path that reaches the worker some
other way.

Everything a plugin legitimately needs -- file browsing, HTTP, playback
control, library queries, theme overrides -- has a purpose-built `plugin.*`
function below; only `io`/`os.remove`/`os.rename` fall outside that (kept
for existing plugins' own state-file needs, not because nothing else
covers it).

Every plugin call (a UI callback, a timer tick, an event handler, and the
file's own top-level code on load) also runs under a wall-clock time
budget -- 2 seconds, checked periodically via a Lua instruction-count hook
(`plugin_call()`, wrapping every `lua_pcall()` in `plugin_manager.c`). The
budget is cumulative Lua-busy time, not "start of the call to now": every
native `plugin.*` function, plus the guarded `io`/`os` entry points, adds
only its own elapsed native time to the deadline. Legitimate native work
(copying theme icons with `plugin.set_icon()` or a slow
`plugin.http_get()`) therefore cannot abort the plugin the moment Lua
resumes, while repeated cheap API calls cannot reset already-consumed Lua
time. File-handle methods such as `file:read()` remain part of the budget.
Without that credit, `Themes.lua` failed to load after a firmware flash --
it copies ~140 icons into a wiped `/usr/data/theme_overrides/` at top
level, before `plugin.register_list_item()`, and the 2-second wall clock
killed the whole file. A genuine runaway loop still hits the cumulative
budget, including one that repeatedly calls a cheap native API.

<a id="ui-entry-points"></a>

## 🧭 Adding Your Plugin to the UI

Choose the entry point that best matches the plugin:

`plugin.register_list_item(list_id, ...)` adds a row after the native rows
on one of these screens:

| `list_id` | Location | Good fit | Shared limit |
|---|---|---|---:|
| `"books"` | Books | Readers, audiobooks, reference tools | 8 |
| `"settings"` | Settings | General plugin configuration | 8 |
| `"display"` | Settings → Display | Themes and visual tools | 8 |
| `"playback"` | Settings → Playback | Audio and playback tools | 8 |
| `"power"` | Settings → Power | Battery and power tools | 8 |
| `"system"` | Settings → System | Device and maintenance tools | 8 |

Passing anything else raises a Lua error at load time rather than silently
registering into nothing. If no plugin registers a row for a given
`list_id`, that screen just shows its native rows, no placeholder.
`build_pill_list_screen()` rows scroll, so every registered row remains
reachable even when several plugins target the same screen.

`plugin.register_stream_media_tile()` gives a streaming plugin a visible
icon-grid tile in **Stream Media**, after the built-in Subsonic tile. Up to
`PLUGIN_MAX_STREAM_TILES` plugin tiles are supported (currently 5, for 6
tiles total). Unlike the Books list above, `build_icon_grid_screen()`'s grid
**cannot be scrolled** (real-device testing confirmed) -- Stream Media
happens to have room for this cap because it only has 1 built-in tile.
`plugin.register_home_tile()` gives Home the same capability, up to
`PLUGIN_MAX_HOME_TILES` (currently 6) plugin tiles -- a registered tile only
actually appears once a `set_home_layout()` call's `options.order` (or
another plugin's `.theme`-file-driven one, see `plugins_examples/Themes.lua`)
references its `id`.

## 🧰 API Reference

Every function below is a field on the global `plugin` table, available
from the moment your script starts running (injected before
`luaL_dofile()`). All of it is implemented in `src/plugins/plugin_manager.c`.

| Area | Main APIs |
|---|---|
| Identity | `define`, `api_version`, `has_capability`, `get_app_info`, `media_capabilities` |
| UI | `register_list_item`, `register_stream_media_tile`, `register_home_tile`, `show_list`, `show_settings_list`, `show_text_input`, `show_toast` |
| Theme | `set_icon`, `set_background_color`, `set_text_color`, `set_home_layout`, `refresh_theme`, `reload_ui` |
| Playback | `play_file`, `play_list`, `play_remote`, `queue_remote_list`, transport controls, playback state |
| Files & Playlists | `sd_root`, `list_dir`, `mkdir`, `playlist_list`, `playlist_read`, `playlist_create`, `playlist_add`, `playlist_remove`, `playlist_delete` |
| Storage & Secrets | `storage.get`/`set`/`delete`/`list`, `secrets.set`/`exists`/`delete` |
| Data & Crypto | `json_decode`, `json_encode`, `md5` |
| Library | `get_artist_albums`, `get_album_tracks`, `get_next_album_tracks`, `library_song_count`, `library_get_songs`, `library_search`, `library_get_song`, `library_get_artists`, `library_get_albums`, `refresh_library` |
| Audio | `eq_load_profile`, `eq_save_profile`, `eq_set_*`, `eq_reset` |
| Network | `http_request`, `download_file_async`, `cancel`, legacy `http_get`/`http_post` |
| Automation | `on`, `set_interval`, `clear_interval` |

<a id="identity"></a>

### 🪪 Identity, API Version, and Capabilities

New plugins should declare identity once, at the start of their top-level
script:

```lua
plugin.define({
    id = "org.example.my_plugin",
    name = "My Plugin",
    version = "1.0.0",
    api_min = 2,
})
```

`id` is a stable identifier using letters, digits, `.`, `_`, and `-`; it must
be unique among loaded plugins -- checked against every other already-loaded
plugin's id regardless of whether that id came from an explicit `define()`
or a legacy plugin's filename-derived fallback, so an explicit
`id = "legacy.foo"` and a plain `foo.lua` loaded in either order can never
silently collide and share one storage/secrets namespace. A legacy id that
would collide is disambiguated with a short, deterministic hash of the
plugin's own file path (stable across reloads) rather than by load order.
`api_min` rejects the plugin at load time when the player API is too old.
Existing plugins without `define()` remain supported as legacy plugins
using an identity derived from their filename.

- `plugin.api_version()` returns the current integer plugin API version (currently `4`).
- `plugin.has_capability(name)` reports whether an optional interface exists.
  Supported capability tokens:
  - UI: `ui.list`, `ui.settings`, `ui.row_width`, `ui.text_input`, `ui.toast`, `ui.theme`, `ui.home_layout`, `ui.launcher_layout`, `ui.home_background`
  - Playback & Audio: `playback.control`, `playback.state`, `playback.events`, `playback.remote`, `audio.peq`, `audio.hw_volume_curve`
  - Filesystem & Playlists: `filesystem.sd`, `filesystem.mkdir`, `filesystem.playlists`
  - Storage & Secrets: `storage.namespaced`, `storage.secrets`
  - Network: `network.http.sync`, `network.http.async`, `network.http.download`
  - Data & Crypto: `data.json`, `crypto.md5`
  - Library: `library.artist_albums`, `library.paged`, `library.refresh`
- `plugin.get_app_info()` returns `version`, `build`, `platform`, and
  `plugin_api` fields.

#### API version 2 changelog

New in API 2: `plugin.storage.*`, `plugin.secrets.*`, `plugin.json_decode()`/
`plugin.json_encode()`, `plugin.media_capabilities()`,
`plugin.download_file_async()`, `plugin.mkdir()`, and an extended
`plugin.http_request()` (arbitrary request headers, `PUT`/`PATCH`/`DELETE`/
`HEAD` methods, connect/read/total timeouts, bounded redirect-following,
response headers via a new 4th callback argument, and a `plugin.cancel()`
that now actually forces the connection closed rather than only
suppressing the callback) -- all backward compatible, see that function's
own doc section for the exact compatibility guarantees.

**One breaking change bundled into this same window, not gated by
`api_min`:** an earlier security-audit pass removed `require`, `dofile`,
`loadfile`, `load`/`loadstring`, `package`, `debug`, `os.execute`,
`os.getenv`, `os.exit`, `os.tmpname`, and `io.popen` from every plugin's
Lua environment. Real-device testing confirmed none of the 7 bundled
plugins used any of them, and plain file I/O (`io.open`) plus
`os.remove`/`os.rename` were deliberately kept since several bundled
plugins genuinely depend on those for their own state files. A
third-party plugin that split its own code across multiple files via
`require()`/`dofile()`, or shelled out via `os.execute()`/`io.popen()`,
will fail after upgrading to a player build with this change -- there is
currently no restricted module-loader replacement. If this affects your
plugin, inline your code into a single file for now.

#### API version 3 changelog

New in API 3: `plugin.play_remote()`/`plugin.queue_remote_list()` (see
their own doc section above) and the `"track_started"` event's two new
trailing `provider`/`track_id` arguments -- both purely additive, no
breaking changes bundled into this window. A plugin that only needs
`play_remote()`/`queue_remote_list()` specifically, without requiring the
whole API 3 batch, can feature-detect just this with
`plugin.has_capability("playback.remote")` instead of bumping `api_min`.

#### API version 4 changelog

New in API 4: `plugin.set_home_layout()` (see its own doc section below) --
per-tile color/radius/size/alignment/icon overrides for Home's 6 fixed
tiles, plus an optional switch from Home's native icon grid to a scrollable
pill-list. Purely additive, no breaking changes bundled into this window. A
plugin that only needs this one function can feature-detect it with
`plugin.has_capability("ui.home_layout")` instead of bumping `api_min`.

#### API version 5 changelog

New in API 5: `plugin.reload_ui()` (see its own doc section below) --
rebuilds every screen/style in the same process, so a `set_icon()`/
`set_background_color()`/`set_text_color()`/`set_home_layout()` call takes
full effect without the player being killed and relaunched. Purely
additive, no breaking changes bundled into this window. A plugin that only
needs this one function can feature-detect it with
`plugin.has_capability("ui.reload")` instead of bumping `api_min`.

#### API version 6 changelog

New in API 6: `plugin.refresh_theme()` -- applies theme assets and Home
layout without tearing down screens, plugins, or live services. Feature-
detect it with `plugin.has_capability("ui.theme_refresh")`.

#### API version 7 changelog

New in API 7: `plugin.register_home_tile()` (see its own doc section below)
-- a plugin can add its own tile to Home, the same way
`plugin.register_stream_media_tile()` already does for Stream Media. Paired
with `plugin.set_home_layout()`'s new `options.order` (an ordered list of
native-tile-keys/plugin-tile-ids controlling which tiles Home shows and in
what position -- see its own doc section), a theme can now reorder any
tile, drop a native one, or interleave a plugin-registered one among the
native six, none of which was previously possible (Home was always exactly
the 6 native tiles, always in a fixed order). Purely additive, no breaking
changes bundled into this window. A plugin that only needs this can
feature-detect it with `plugin.has_capability("ui.home_tiles")` instead of
bumping `api_min`.

#### API version 10 changelog

New in API 10: `plugin.set_home_layout()`'s new `options.background_image`
field (see its own doc section below) -- a static `.png`/`.jpg`/`.jpeg`
shown behind Home's own tiles/rows, in both tile and list mode. Unlike
`plugin.set_background_color("screen", ...)`, this affects Home's root
object only, not every screen in the app. Purely additive, no breaking
changes bundled into this window. A plugin that only needs this can
feature-detect it with `plugin.has_capability("ui.home_background")`
instead of bumping `api_min`.

#### API version 11 changelog

New in API 11: `plugin.set_hw_volume_curve()` (see its own doc section
above) -- lets a plugin replace the app's own UI-volume -> internal-DAC-
register mapping with a custom 101-entry table, e.g. to reproduce a real
device's Low/Medium/High Gain modes. Purely additive, no breaking changes
bundled into this window. A plugin that only needs this can feature-detect
it with `plugin.has_capability("audio.hw_volume_curve")` instead of
bumping `api_min`.

<a id="plugin-ui"></a>

## 🖥️ Lists and Settings UI

### `plugin.register_list_item(list_id, label, on_open [, options])`

Adds a row to an existing native list screen.

- `list_id` (string): which screen to add to -- `"books"`, `"settings"`,
  `"display"`, `"playback"`, `"power"`, or `"system"` (see "Adding Your
  Plugin to the UI" above for what each targets) -- anything else raises
  a Lua error immediately, rather than silently registering into nothing.
- `label` (string): the row's visible text.
- `on_open` (function): called with zero arguments when the row is tapped.
  This is where you'd call `plugin.show_list()` or
  `plugin.show_settings_list()` to show your first screen.
- `options` (table, optional): `{ icon = "...", height = n, width = n, text_size =
  "..." }` -- see "Row images, resizing, and text size" below for all
  three.

Every plugin that calls this gets its own row (unlike the old
`register_tile()` this replaced, where only the first caller was ever
reachable) -- a script can still call it more than once if it genuinely
wants multiple independent entry points into the same list.

Errors if more than that `list_id`'s own cap (currently 8 for each of the
six) is registered, across every loaded plugin combined.

### `plugin.register_stream_media_tile(label, on_open [, icon])`

A separate registry from `register_list_item()` above, reached differently
-- see "Reaching a plugin from the UI". Use this one for a plugin that
thematically belongs in Stream Media (a streaming/radio source, for
instance -- see `plugins_examples/NetRadio.lua`).

- `label` (string): shown as the tile's caption.
- `on_open` (function): called with zero arguments when the tile is
  tapped.
- `icon` (string, optional): a theme2-relative asset path, actually shown
  this time. Defaults to `"stream_media/radio.png"` (a real stock asset)
  if omitted -- a sensible default for the kind of plugin this registry is
  for.

Errors if more than `PLUGIN_MAX_STREAM_TILES` (currently 5, across every
loaded plugin combined) are registered.

### `plugin.register_home_tile(id, label, on_open, icon)`

Registers a tile a theme can place on Home via `set_home_layout()`'s
`options.order` below -- naming this tile by `id` alongside any of the 6
native keys. Registering alone doesn't show it anywhere; a theme (or your
own plugin's own top-level code) still has to reference `id` in `order` for
it to actually appear.

- `id` (string): a stable name other code references this tile by. 1-39
  characters, letters/digits/`.`/`_`/`-` only (same rule `plugin.define()`'s
  own `id` uses) -- a home tile id can end up inside a `.theme` file's
  comma-separated `home_order=` line (`plugins_examples/Themes.lua`), so a
  comma or whitespace would parse incorrectly there. Must also not be one of
  the 7 native keys (`"music"`, `"stream_media"`, `"wireless"`, `"books"`,
  `"settings"`, `"dac"`, `"subsonic"`) -- those always resolve to the real native tile
  first, so a plugin tile registered under one would silently never be
  reachable. Must be unique across every registered home tile, not just
  your own plugin's -- two plugins picking the same id is an immediate Lua
  error. Required, unlike `register_stream_media_tile()` above, since
  Stream Media has no reordering concept and never needed a name to
  reference a tile by.
- `label` (string): shown as the tile's caption.
- `on_open` (function): called with zero arguments when the tile is tapped.
- `icon` (string): a theme2-relative asset path, non-empty, at most 79
  characters. Unlike `register_stream_media_tile()`'s own optional `icon`
  (which falls back to a small default), this is required -- Home's tiles
  are large and deliberate, and there's no generic placeholder asset that
  would look right at that size across every device/theme.

Errors if more than `PLUGIN_MAX_HOME_TILES` (currently 6, across every
loaded plugin combined) are registered, or if `id`/`icon` fails any of the
checks above.

A tile registered this way but never referenced by any `order` (or
referenced by a theme that failed to load, or hasn't loaded yet -- see
`set_home_layout()`'s own note on plugin load order) simply isn't shown --
not an error.

### `plugin.set_icon(theme2_relative_path, source_file_path)`

Reskins an **existing** tile's icon in place -- a different case from
`register_stream_media_tile()`'s own icon argument above (which points a
brand-new tile at whatever icon it likes, no special handling needed) and
from `register_list_item()`'s/`show_list()`'s/`show_settings_list()`'s own
`icon` option (which puts a NEW icon on a row that has none, from an
arbitrary SD-card file rather than an existing theme2 asset -- see "Row
images, resizing, and text size" below). This function is specifically for
changing what an *already-existing* stock icon looks like. For example,
`plugin.set_icon("launcher/book.png", plugin.sd_root() .. "/my_icon.png")`
changes what Home's Books tile looks like.

Copies `source_file_path`'s bytes into the app's writable theme-override
directory under `theme2_relative_path`, the same mechanism this app
already uses for its own non-stock icons (e.g. Subsonic's).

**Call this from your plugin's top-level script code only** (i.e. during
load, not from inside `on_open`/`on_select`/any callback that might run
after the app has already shown a screen). LVGL caches decoded images by
path string for the app's whole lifetime, with no cache-invalidation call
anywhere in this codebase -- an override written *after* the first time
that path was resolved+decoded this boot will silently not take effect
until the next full app restart. Since every plugin's top-level code runs
during `plugin_manager_init()`, which always runs before `gui_init()`
builds any icon-grid/pill-list screen, calling this at load time is always
safe.

Also worth knowing: a tile's on-screen icon size/position is computed once,
at screen-build time, from the *original* icon's dimensions, and never
recomputed on a later icon change -- a replacement PNG with a very
different aspect ratio than the original will render at a size/position
tuned for the original, not the replacement. Keep replacement icons
roughly the same aspect ratio as what they're replacing.

Raises a Lua error if `source_file_path` can't be read or copied.

### `plugin.set_background_color(slot, rgb)`

Sets one of three background-color slots, live, app-wide, no restart
needed:

- `"screen"` -- every screen's own background.
- `"card"` -- every popup, EQ card, and settings slider card -- every
  neutral dark surface that isn't a plain screen or a list row.
- `"list_row"` -- every row in every list screen (All Songs, Artists,
  Playlists, Files, Queue, ...).

`rgb` is a packed `0xRRGGBB` integer, e.g. `0x1E1E22`. Unlike
`plugin.set_icon()`, this is safe to call at any time, including from
inside a callback well after load -- it's a plain style-property update
plus a redraw request, no file/cache involved.

Raises a Lua error if `slot` isn't one of the three names above.

### `plugin.set_text_color(slot, rgb)`

Sets one of two text-color slots, live, app-wide, no restart needed:

- `"primary"` -- the app's dominant near-white text (labels, titles, list
  row text).
- `"muted"` -- secondary/disabled-ish gray text (chevrons, timestamps,
  subtitles).

Destructive-red text (delete/reset confirmations) and accent-tinted text
are **not** covered by either slot -- they're semantically fixed, not part
of the light/dark split these two slots (and `set_background_color()`)
drive. `rgb` is a packed `0xRRGGBB` integer, same convention as
`set_background_color()`. Safe to call at any time, same live-update
mechanism as `set_background_color()` -- no file/cache involved.

Raises a Lua error if `slot` isn't `"primary"` or `"muted"`.

### `plugin.set_home_layout(tiles, options)`

Restyles Home's tiles, and optionally switches Home from its native icon
grid to a scrollable pill-list. Since API 7, `options.order` (below) also
controls which tiles Home actually shows and in what position -- a native
tile left out of `order` isn't shown, and a `plugin.register_home_tile()`
id included in it is. The native `"subsonic"` key is opt-in and opens
Subsonic directly; omitting `order` preserves the original six-tile Home.
Before API 7, Home was always exactly its 6 native
tiles (`"music"`, `"stream_media"`, `"wireless"`, `"books"`, `"settings"`,
`"dac"`), always in that fixed order, and this call could only restyle them
-- omitting `order` entirely still gets you exactly that.

**Never itself persisted, and only takes effect the next time Home is
actually built** -- a real app start, `plugin.refresh_theme()`, or the
legacy full `plugin.reload_ui()`. This call only updates an in-memory config that
`build_home_screen()` reads at that build time; nothing here writes to disk.
Calling it from inside a callback with no reload/restart following is not an
error, but has no visible effect until one of those two things actually
rebuilds Home -- and if nothing ever re-calls this again before that
happens, the NEXT rebuild reverts to native, same as never having called it
at all (there is no native persistence to lean on here). To have a layout
survive a restart or a reload, a plugin must persist its own chosen layout
to its own state file (the same way `Themes.lua` persists a theme choice)
and re-call `set_home_layout()` with it from top-level script code every
time -- see `plugins_examples/Themes.lua`'s own `tile.<key>.<field>=...`
`.theme` file format for a complete reference implementation of exactly
this pattern.

**Multiple plugins calling this both restyle the same single global Home
layout -- there's no per-plugin slot.** Whichever plugin's call runs last
during startup wins outright (its config completely replaces any earlier
plugin's, not merged field-by-field); see "How Plugins Load" above for the
now-deterministic (alphabetical-by-filename) load order this follows. Two
layout-changing plugins installed together should be considered mutually
exclusive by design, same as two theme-color plugins both calling
`set_background_color()`.

`tiles` (array table): one entry per tile being restyled, each

```lua
{
    key = "music",         -- required: music/stream_media/wireless/books/settings/dac/subsonic, or a plugin tile id
    bg_color = 0xRRGGBB,   -- optional
    text_color = 0xRRGGBB, -- optional
    radius = 12,           -- optional, px

    -- list mode only (ignored in tile mode):
    height = 88, width = 420,   -- optional, px -- clamped the same as register_list_item()'s own height/width
    align = "center",            -- optional: "left" (default), "center", or "right"
    accessory = true,            -- optional: show the row's chevron
    text_size = "medium",        -- optional: "small", "medium", "large", or "mono"
    icon = true,                 -- optional: show the tile's own native icon on its list row
}
```

A tile whose key is never mentioned keeps every field at its native
default. Unlike before API 7, an unrecognized-looking `key` is NOT rejected
here -- it may name a plugin tile that simply hasn't registered yet (see
"How Plugins Load" above for load order), so it's resolved later, when Home
is actually built; a key that still doesn't resolve to anything at that
point is silently dropped (not shown), not a crash. An `align` or
`text_size` that isn't one of the values listed above, a non-table array
entry, more entries than tiles could ever legitimately need (currently 12,
across native and plugin tiles combined -- this can only mean a duplicate
key or a copy-paste mistake, so unlike `show_list()`'s own item cap this is
not silently truncated), or a `height`/`width`/`radius` outside a 32-bit
integer's range, all still raise a Lua error immediately. A repeated `key`
within the same `tiles` array cleanly replaces the earlier entry for that
tile (every field, not just the ones the later entry sets) rather than
merging the two. `radius` must also be non-negative.

`options` (table, optional -- a non-table, non-nil value here is also a Lua
error, since there's no reasonable non-table `options`):

```lua
{
    mode = "list",  -- "tile" (default, today's icon grid) or "list"
    tile_gap = 6,   -- tile mode only, px between tiles, clamped to 0-64
    row_gap = 10,   -- list mode only, px between rows, clamped to 0-84 (0 = the native default of 6)
    background_image = plugin.sd_root() .. "/wallpaper.jpg",
                    -- optional: a static image (.png, .jpg, or .jpeg) shown
                    -- behind Home's own tiles/rows, in both tile and list
                    -- mode. ONLY Home's background -- unlike
                    -- plugin.set_background_color("screen", ...), every
                    -- other screen is unaffected. Copied once into this
                    -- app's writable theme-override storage the moment this
                    -- call runs (same mechanism plugin.set_icon() uses), so
                    -- treat it like set_icon(): call from your plugin's
                    -- top-level script code for a real app start to pick it
                    -- up reliably, or follow a later call with plugin.
                    -- refresh_theme()/reload_ui() the same way any other
                    -- set_home_layout() change needs one to take visible
                    -- effect. LVGL draws a bg_image at native size, CENTERED,
                    -- never stretched to fill -- author the file at exactly
                    -- the panel's own full-screen resolution (480x800 on
                    -- R1 -- confirmed in main.c's own SCREEN_WIDTH/
                    -- SCREEN_HEIGHT; 480x320 is a specific sub-panel
                    -- elsewhere in the UI, not the whole screen) rather
                    -- than relying on any scale-to-fit. Raises a Lua error
                    -- if the file isn't .png/.jpg/.jpeg or can't be read.
    order = { "dac", "music", "stream_media", "wireless", "books", "settings" },
                    -- optional (API 7+); which tiles Home shows, and in what
                    -- position -- position IS order. Each entry is a native
                    -- key or a plugin.register_home_tile() id (resolved the
                    -- same deferred way `tiles`' own `key` is, above). A
                    -- native key simply left out is not shown at all --
                    -- this is how you drop a tile, not just reorder one.
                    -- Omit `order` entirely to keep today's fixed native
                    -- order and show all 6. A literal duplicate entry is
                    -- rejected immediately (unlike an unresolved name, a
                    -- repeat can't just be a load-order timing issue).
                    -- In tile mode specifically, `order` is capped at 6 --
                    -- build_icon_grid_screen()'s 2-column grid cannot
                    -- scroll, so more than that would render with tiles
                    -- unreachable off-screen; switch to list mode instead.
}
```

Each call to `set_home_layout()` replaces the whole stored config -- there
is no incremental merge across separate calls, so pass every tile you want
styled (and the full `order` you want) in the same call.

```lua
plugin.set_home_layout({
    { key = "music", bg_color = 0x1e3524, text_color = 0xd8c9a3, radius = 24,
      height = 92, width = 440, align = "center", accessory = true, text_size = "medium", icon = true },
    -- ... one entry per tile you want to restyle
}, { mode = "list", row_gap = 10, order = { "music", "dac", "books" } })
```

See `plugins_examples/Themes.lua` and its `plugins_examples/Themes/*.theme`
files for a complete reference implementation spanning both tile and list
mode -- e.g. `Terminal.theme`/`GameBoy.theme` (list mode) and
`Retro.theme`/`Vaporwave.theme` (tile mode).

### `plugin.set_launcher_layout(options)`

API 8. Switches the native `music`, `stream_media`, and `wireless` launcher
screens independently between their normal icon grid and pill-list rows.
Each optional menu table accepts `mode = "tile"|"list"` plus `row_gap`,
`height`, `width`, `bg_color`, `text_color`, `radius`, `align`, `accessory`,
`text_size`, and `icon`. Omitted menus retain the native grid; an empty call
resets all three. Like Home layout changes, call `plugin.refresh_theme()` to
rebuild the affected screens without restarting plugins or live services.

```lua
plugin.set_launcher_layout({
    music = { mode = "list", height = 108, width = 480, row_gap = 10,
              bg_color = 0x000000, text_color = 0x33FF33,
              align = "left", accessory = true, text_size = "mono", icon = true },
    stream_media = { mode = "list", height = 108, width = 480 },
    wireless = { mode = "list", height = 108, width = 480 },
})
```

### `plugin.refresh_theme()`

Applies already-written theme assets and the current Home layout after the
calling callback returns. It drops LVGL's decoded-image cache, replaces only
Home, and refreshes navigation/player/quick-drawer render caches. All other
screens, plugin Lua states, navigation state, playback, and Wi-Fi/Bluetooth/
AirPlay/DLNA/Remote Control connections remain alive.

Use this after a batch of `set_icon()`, color, `set_home_layout()`, and
`set_launcher_layout()` calls.
Requests are deferred past active transitions and coalesced. The Themes
example applies its full icon inventory during startup, ensuring existing
screens already reference the override paths that this call refreshes.

### `plugin.reload_ui()`

Rebuilds every screen and style in the same process -- the same trick a
`set_icon()`/`set_background_color()`/`set_text_color()`/`set_home_layout()`
call would otherwise need a full player restart to take complete effect
for (an already-decoded icon stays cached, and most screens are only ever
built once). Never restarts the process, and never touches audio playback
or any network/Bluetooth/D-Bus connection (Wi-Fi, Subsonic, DLNA, AirPlay,
Remote Control) -- those stay exactly as they were before the call.

Takes no arguments and returns nothing. Safe to call from any plugin
callback, including your own settings-row `on_open`/`on_select` handler --
the actual reload is deferred to run right after your callback returns, not
inline, so it's never touching your plugin's own still-running script when
it tears everything down.

**Only call this from a callback that fires in response to an actual user
action -- never unconditionally from your plugin's own top-level script
code.** The reload re-runs every plugin's top-level code as part of
rebuilding, same as a real boot. A guard prevents that from becoming an
infinite loop (a request made while a reload is already running is dropped,
not queued), but an unconditional top-level call still wastes one full
reload the first time your plugin loads, for no reason -- calling this
conditionally, only when something actually changed, is still the correct
thing to do, the way the example below does (only inside the `on_select`
callback, only when the user actually picked something).

```lua
plugin.register_list_item("display", "Theme", function()
    plugin.show_list("Theme", { "Dark", "White" }, function(index)
        apply_theme(index == 2 and "white" or "dark") -- your own set_icon()/set_background_color() calls
        plugin.refresh_theme() -- targeted update; connections and navigation survive
    end)
end)
```

Navigation always lands back on Home after a reload, regardless of which
screen was showing before -- there is no "return to where you were" state
carried across it.

### `plugin.show_list(title, items, on_select [, options])`

Opens a list screen.

- `title` (string): the screen's header text.
- `items` (array table): one row per entry, shown in order. Each entry is
  either a plain string (a row with just a label, as before) or a table
  `{ label = "...", icon = "...", text_size = "..." }` for a row with its
  own icon and/or text size -- see "Row images, resizing, and text size"
  below.
- `on_select` (function): called with the **1-based** index of whichever
  row was tapped (Lua array convention, not C's 0-based one) when the user
  taps a row. Not called if the user backs out without tapping anything.
- `options` (table, optional): `{ height = n, width = n, selected = index }` --
  resizes every row in this call (not per-row) and optionally draws an
  accent outline around the selected 1-based row. Selecting another row
  moves that outline automatically.

Each call opens a **new** screen (from a pool of 4 reusable ones -- see
`PLUGIN_LIST_SCREEN_POOL_SIZE` in `gui.c`), so calling `show_list` again
from inside an `on_select` callback (to drill into a subfolder, for
example) pushes a second screen on top rather than replacing the first --
the device's Back gesture/button naturally returns to the previous list.
Nesting more than 4 levels deep reuses an earlier pool slot and will
corrupt back-navigation at that depth; no real plugin should need to nest
that far.

Each pool slot owns its own `on_select` callback. Backing out of a nested list
therefore restores the earlier screen and its correct callback. Reusing a slot
beyond four simultaneously stacked plugin lists remains unsupported.

### `plugin.show_settings_list(title, items)`

Opens a **settings submenu** -- indistinguishable from a native Settings
screen, with real toggle switches and sliders, not plain tappable text.
Use this instead of `show_list()` whenever your plugin's own screen is
itself a small settings panel (see `plugins_examples/PlaybackExtras.lua`).

- `title` (string): the screen's header text.
- `items` (array table of row tables): one row per entry, shown in order.
  Every row needs `type` and `label`; the rest depends on `type`:
  - `{ type = "row", label = "...", on_select = function() ... end }` -- a
    plain tap. `on_select` is called with no arguments. A `show_settings_list`
    call from inside `on_select` is how you nest a submenu inside a
    submenu -- no separate "submenu" row type needed.
  - `{ type = "toggle", label = "...", value = true/false, on_change =
    function(new_value) ... end }` -- a real on/off switch. `value` is its
    initial state; `on_change` is called with the new boolean every time
    it's tapped.
  - `{ type = "slider", label = "...", min = n, max = n, value = n,
    on_change = function(new_value) ... end }` -- a real slider, integer
    range `min`..`max`, initial position `value`. `on_change` fires **once,
    when the drag is released** -- not on every intermediate tick, same
    convention every native settings slider (Screen Timeout, Startup
    Volume, the EQ bands) already uses, so a callback that writes to disk
    isn't hammered mid-drag.

  Every row type also accepts `icon`, `height`, `width`, and `text_size` (see "Row
  images, resizing, and text size" below) -- except `height` on a
  `"slider"` row, which is ignored (its card has its own fixed layout with
  no spare room to grow into).

Capped at 24 rows per call, and 4 `"slider"`-type rows per call
specifically (the underlying swipe-gesture-safety bookkeeping -- see below
-- needs a real, bounded slot per slider). Both cases silently drop the
excess rather than erroring, same convention as `show_list()`'s own item
cap. An unknown/missing `type`, an unrecognized `text_size`, or a row
missing its required callback (`on_select` for `"row"`, `on_change` for
`"toggle"`/`"slider"`), raises a Lua error immediately instead.

Each call opens a screen from a small **separate** pool from `show_list()`'s
own (2 slots, smaller since settings submenus nest shallower in practice --
`PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE` in `plugin_manager.h`). Unlike
`show_list()`, each pool slot keeps its **own** row callbacks (not one
shared "most recently opened" registration) -- so if your plugin has a
submenu open on top of another, backing out to the first one still routes
its toggle/slider taps correctly, no equivalent to `show_list()`'s own
"structure it as immediate re-calls" caveat above. Nesting more than 2
levels deep reuses an earlier pool slot and will corrupt back-navigation at
that depth, same bounded-pool tradeoff `show_list()` makes at 4.

Every slider row you create is also registered against a fixed-size,
app-wide "don't let a drag on this become a swipe-to-player/back gesture"
list -- purely internal bookkeeping (`gui.c`'s `swipe_dead_zones[]`), not
something your plugin needs to manage, but it's *why* the 4-sliders-per-call
cap above exists and isn't just an arbitrary round number.

Items beyond the first 500 (`PLUGIN_MAX_LIST_ITEMS`) in a single call are
silently dropped.

### 🎛️ Row Images, Resizing, and Text Size

`register_list_item()`'s `options` table, `show_list()`'s per-row table
entries, and `show_settings_list()`'s per-row tables support these optional
layout fields. For `show_list()`, width and height live in the call-level
`options` table so every browsing row stays uniform:

- **`icon`** (string) -- either a **raw absolute filesystem path**, e.g.
  `plugin.sd_root() .. "/.plugins/my_icon.png"` -- **not** a theme2-relative
  path like `register_stream_media_tile()`'s `icon` argument or
  `set_icon()`'s first argument use -- **or** a plain relative filename/path
  (doesn't start with `/`), e.g. `"my_icon.png"`, resolved against
  `<SD card>/.plugins/` -- the same folder your plugin's own `.lua` file
  already lives in, so a relative path just means "a file sitting next to
  my script." Any file your plugin can read works, including a file it
  downloaded itself at runtime. Drawn to the left of the row's label,
  scaled to a consistent on-screen size regardless of the source file's own
  resolution. A missing or corrupt file just renders at its native size
  rather than erroring -- an icon is cosmetic, not worth failing the whole
  row over.
- **`height`** (number, px) -- resizes the row. Clamped to a fixed range
  (currently 100-220px) rather than erroring if you ask for more or less.
  Not available on a `show_settings_list()` `"slider"` row (see that
  function's own section above) -- everywhere else, an unset/zero height
  keeps that row type's own default (124px for a pill/`register_list_item`
  row, 84px for a plain `show_list` row).
- **`width`** (number, px) -- resizes and keeps the row centered. It is
  available for native-list plugin rows, `show_list()` rows, and every
  `show_settings_list()` row including sliders. Values are clamped to
  240-464px; unset/zero keeps the native width. Resizing a pill row replaces
  its fixed-size background sprite with the matching rounded fill so the
  artwork is never stretched.
- **`text_size`** (string) -- `"small"`, `"medium"`, `"large"`, or `"mono"`.
  `"small"`/`"medium"`/`"large"` use a font with full non-Latin fallback
  (Cyrillic, CJK, Korean, Thai) -- correct for plugin-authored text, which
  (unlike this app's own fixed English UI chrome) might not be English.
  `"mono"` is an 8x16 bitmap monospace font (`lv_font_unscii_16`) for a
  pixel/terminal look -- **ASCII-only**, no accented or non-Latin glyphs, so
  only reach for it when you control the text yourself and know it stays
  plain ASCII (see `plugins_examples/Themes/GameBoy.theme`/`Terminal.theme`).
  An unrecognized value raises a Lua error; omitting it keeps that
  row type's own existing default size.

None of the three affect a row that doesn't set them -- a plugin that
never uses this section's fields renders exactly as it did before they
existed.

<a id="files-playback"></a>

## 💾 Files and Playback

### `plugin.list_dir(path)`

Lists a directory's immediate children.

- `path` (string): an **absolute** path (build one with `plugin.sd_root()`,
  there's no relative-path resolution).
- Returns: an array table, one entry per non-hidden (not starting with
  `.`) child, each `{ name = "chapter1.mp3", dir = false }`. Order is
  whatever `readdir()` returns -- not sorted; sort it yourself
  (`table.sort`) if you need a specific order.
- If `path` doesn't exist or can't be opened, returns an empty table (not
  an error).

### `plugin.sd_root()`

Returns the SD card's absolute mount path as a string (`/data/mnt/sd_0` on
the real device, `./music` on the host simulator) -- build every path your
plugin touches from this rather than hardcoding `/data/mnt/sd_0`, so the
same script works unmodified in the host simulator too.

### Playlist management (`plugin.playlist_*`)

CRUD over `.m3u` playlists, in the same `Playlists` folder the native
Playlists screen reads from -- a playlist a plugin creates or deletes shows
up/disappears there immediately, no rescan needed.

- `plugin.playlist_list()` -- array of every playlist's absolute path, or
  `nil` if there are none.
- `plugin.playlist_read(m3u_path)` -- array of the playlist's song paths in
  file order, or `nil` if it can't be read or is empty.
- `plugin.playlist_create(name, song_path)` -- creates a new playlist named
  `name` with `song_path` as its first entry. Returns the new playlist's
  absolute path, or `nil` on failure.
- `plugin.playlist_add(m3u_path, song_path)` / `plugin.playlist_remove
  (m3u_path, song_path)` -- append/remove a song from an existing playlist.
  Both return `bool`; `remove` deletes every matching occurrence, not just
  the first.
- `plugin.playlist_delete(m3u_path)` -- deletes the whole playlist file
  outright (not a single song within it -- see `playlist_remove` for that).
  Returns `bool`.

```lua
local m3u = plugin.playlist_create("Favorites Mix", "/path/to/song1.mp3")
plugin.playlist_add(m3u, "/path/to/song2.mp3")
local songs = plugin.playlist_read(m3u)
### `plugin.mkdir(path)`

Creates `path` and any missing parent directories (`mkdir -p` semantics).
An existing directory is success. Returns `true` on success or `nil, error`
if a component cannot be created or an existing component is not a directory.

```lua
local downloads = plugin.sd_root() .. "/Talks/Downloads"
local ok, err = plugin.mkdir(downloads)
if not ok then plugin.show_toast(err) end
```

### `plugin.play_file(path)`

Starts playback of a single file (`path`, absolute) as a fresh one-song
playlist -- same "starting something new clears Up Next" semantics as
tapping any song elsewhere in the app. `path` may also be a live
`http://` or `https://` stream URL (internet radio) -- see "Live stream
URLs" below for what that actually supports today.

### `plugin.play_list(paths [, start_index])`

Starts playback of `paths` (array table of absolute file path strings, or
`http(s)://` stream URLs) as a fresh playlist, beginning at the **1-based**
`start_index` (default `1` if omitted). The rest of the app's normal
playback machinery (Prev/Next, the "Track X of Y" label) applies exactly as
if this were any other playlist. Paths beyond the first 500 are silently
dropped; `start_index` is clamped into range if out-of-bounds rather than
erroring.

#### 📻 Live Stream URLs

`path`/`paths` entries starting with `http://` or `https://` are treated as
a live network stream rather than a local file. A dedicated network thread
reads the stream into an internal buffer so a slow or jittery connection
doesn't stall audio output outright, but there's no reconnect-on-drop: if
the connection is lost mid-stream, playback just stops, the same as it
would for a local file hitting a read error.

**Format defaults to MP3**, since most stream URLs (internet radio mount
points especially) don't end in anything recognizable. To stream FLAC
instead, append a `#.flac` suffix to the URL, e.g.
`"https://example.com/track.flac#.flac"` or even just
`"https://example.com/opaque-id#.flac"` -- this is a purely local hint (a
URL fragment is never sent to the server, per RFC 3986) read off the string
before the request goes out, not a real extension or query parameter. No
other format is currently supported for streaming; a URL serving something
else (AAC/Opus/Ogg, or MP3/FLAC mislabeled) will fail to open rather than
silently misdecode. MP3 and FLAC are the two decoders here with a
callback-based streaming API available at all -- everything else either
prescans the whole file up front (incompatible with an unbounded source) or
has no such API in this vendored library.

A few other things behave differently for a live stream than a local
track, by design rather than by omission:

- **No seeking**, for either format -- tapping the progress bar does
  nothing, there's nothing to seek back to on a live source.
- **Duration differs by format.** FLAC streams show a real, correct
  duration (from the STREAMINFO metadata block near the top of the file,
  not a full-stream prescan) and the progress bar fills normally. MP3
  streams have no equivalent authoritative source without downloading the
  whole file first, so the progress bar stays at 0:00 for those.
- **Auto-advance/gapless/crossfade**: for a genuinely unbounded stream
  (internet radio), none of these ever engage -- there's no "end" to
  advance from, so a queued next track never starts on its own; the user
  has to manually skip. A FLAC stream of a real, finite file is the one
  case this doesn't apply to: it ends when the file ends, same as local
  playback, and auto-advance into a queued next track works normally.
- ID3v2 tags a station sends at the start of the stream, and Shoutcast/
  Icecast's own "now playing" metadata extension (ICY metadata), are both
  ignored -- `plugin.play_file()`/`plugin.play_list()` don't surface a
  live stream's track title, only whatever label your own plugin already
  passed to `show_list()`/`register_stream_media_tile()`.

### `plugin.play_remote(track)` / `plugin.queue_remote_list(tracks [, start_index])`

The native side of a provider-neutral remote music plugin (Qobuz/Tidal/etc):
starts playback of one track (`play_remote`) or a fresh playlist of up to
500 (`queue_remote_list`, same 1-based `start_index` convention as
`play_list`). Each `track` is a table:

```lua
plugin.play_remote({
    provider = "qobuz",           -- required, bounded
    track_id = "12345",           -- required, bounded -- stable per track
    stream_url = "https://...",   -- required -- the real, fetchable URL
    verify_tls = true,            -- optional, default true
    title = "Song Name", artist = "...", album = "...",  -- optional
    duration_ms = 214000,         -- optional
    artwork_url = "https://...",  -- optional
    codec = "flac",               -- "mp3"/"flac"/"aac", default "mp3"
    sample_rate = 44100, bit_depth = 16, channels = 2, bitrate_kbps = 0, -- optional, display only
    replaygain_db = -3.5,         -- optional
})
```

`provider` + `track_id` become a stable synthetic key,
`remote://<provider>/<track_id>` -- this, never `stream_url`, is what
Favorites/Most Played/History key against, so the underlying favorite
toggle and play count *accumulate* correctly across repeated plays even
though `stream_url` itself may be a single-use or time-limited signed URL
refreshed on every call.

**This does not yet mean a remote track shows up in the Favorites/Most
Played screens.** Those screens only ever list rows also present in the
on-device local library scan (they inner-join against it, so a favorited
song that's since vanished from your library silently drops off the list
rather than showing a dead entry) -- a `remote://` key is never a row
there, so it's filtered out the same way a stale local favorite would be.
The heart can be toggled and the play count keeps counting up invisibly in
the database, ready to be surfaced once a persistent remote-track catalog
(and a way to refresh a track's `stream_url` outside of an active queue)
exists; neither does yet, so don't build a plugin around a remote track's
Favorites/Most Played entry being visible or reachable anywhere in the app
today.

For the same expiring-URL reason, **resume-on-launch skips remote tracks
entirely** -- there's no stable URL to resume into on a cold launch, so
`last_track` is simply never set to one.

Playback itself goes through the exact same MP3/FLAC callback-streaming
path `play_file()`'s `http(s)://` streaming already uses (see "Live Stream
URLs" above) -- **no seeking, no reconnect-on-drop, no auth/expiring-URL
refresh yet**. `codec` is used directly instead of the extension/Content-
Type sniffing a plain stream URL needs, since the plugin already knows it
from its own catalog API.

### `plugin.show_toast(message [, duration_ms])`

Shows the same transient toast used elsewhere in the app (e.g. "Added to
queue"). Useful for "nothing found" / error feedback -- see
`Audiobooks.lua`'s use of this when a book folder has no playable chapter
files in it. The optional duration defaults to 5000ms and accepts
100..30000ms.

### 🎚️ EQ and Sound Profiles

This app has its own 10-band parametric EQ (`src/audio/peq.c`) -- these
functions expose it directly, `luaL_check*`-validated wrappers straight over
`peq.c`'s own C API (unlike most of the rest of this table, no `gui.c`
bridge is involved, since the EQ engine has no LVGL/screen state to keep in
sync -- see "Extending the `plugin.*` API itself" below). Every setter
below persists immediately (same `peq_save()` every native EQ-screen control
already calls after each change), so a plugin-driven EQ change survives
reboot with no extra work and stays consistent with the native EQ screen.

- **`plugin.eq_load_profile(path)` -> bool** -- loads and applies a `.peq`
  profile file from an arbitrary path (see `eq_save_profile` below for the
  format), applying it immediately and persisting it as the new
  always-current EQ state. Returns `false` (not a Lua error) if `path`
  doesn't exist or can't be read.
- **`plugin.eq_save_profile(path)` -> bool** -- saves the current EQ state
  (bypass, preamp, all 10 bands) to `path`, plain text, one `key=value` line
  per field (`bypass=`, `preamp=`, `bandN_freq=`/`bandN_gain=`/`bandN_q=`/
  `bandN_type=`/`bandN_enabled=` for `N` 0-9). This is the exact format the
  native EQ screen's own "Save Profile" already writes and "Load Profile"
  already reads -- a plugin's saved/loaded profiles are fully interchangeable
  with ones a user creates by hand in the EQ screen. Returns `false` if
  `path` can't be written.
- **`plugin.eq_reset()`** -- restores every band, the preamp, and bypass to
  built-in defaults (same as the native EQ screen's Reset button), then
  persists.
- **`plugin.eq_set_bypass(enabled)`** -- `enabled` (bool): `true` bypasses
  the whole EQ (flat, unprocessed).
- **`plugin.eq_set_preamp(db)`** -- `db` (number): the whole-EQ pre-amp gain,
  in dB.
- **`plugin.eq_set_band(index, freq_hz, gain_db, q)`** -- sets one band's
  frequency, gain, and Q. `index` is **1-based** (1..10, matching every
  other 1-based index in this API), not `peq.c`'s own 0-based one. Raises a
  Lua error if `index` is out of range.
- **`plugin.eq_set_band_type(index, type)`** -- `type` (string): `"peaking"`,
  `"low_shelf"`, or `"high_shelf"`. Raises a Lua error on an unrecognized
  string or out-of-range `index`.
- **`plugin.eq_set_band_enabled(index, enabled)`** -- toggles whether that
  band is applied at all.

There's no getter -- nothing in the profile-switcher use case these are
built for needs to read EQ state back into Lua, and the native EQ screen
already re-reads `peq_get_*()` fresh every time it's opened, so it stays
correct regardless of what a plugin changed. See
`plugins_examples/SoundProfiles.lua`.

### `plugin.set_hw_volume_curve(curve)`

Replaces the entire UI-volume (0-100%) -> internal-DAC-hardware-register
mapping this app's own volume slider drives, letting a plugin implement
things like the real device's own Low/Medium/High Gain modes (or any other
custom curve, e.g. one hand-tuned for a specific pair of IEMs) that the
app's single built-in taper can't cover on its own.

`curve` is a Lua table of **exactly 101 integers, 0-255**, index 1
(`curve[1]`) through index 101 (`curve[101]`) representing UI volume 0%
through 100% respectively. Each value is a *raw hardware register value*
for the internal codec's own "Left"/"Right Playback Volume" ALSA controls,
not a dB number -- lower is louder (0 is the hardware's own loudest,
zero-attenuation point), higher is quieter, and there is no requirement
that the curve be monotonic if you have a real reason for it not to be.
Pass `nil` (or call with no arguments) to restore the app's own built-in
curve. Raises a Lua error if the table isn't exactly 101 entries long, or
if any entry is outside 0-255 (naming the offending index).

Same shape as `plugin.eq_set_band()` above -- calls straight into
`src/audio/audio.c`, no `gui.c`-paired UI state to keep in sync (it
doesn't move the volume slider or its popup; it changes what a given
slider position maps to internally), and takes effect immediately at
whatever volume is currently set, not just on the next change. In-process
only, like every other plugin config call in this API -- not persisted,
so re-apply it at the top of your own script on every boot, same as
`set_home_layout()` and friends.

**Has no effect on Bluetooth or USB output** -- neither ever reaches this
codec's own hardware register at all (USB pipes PCM to a separate ALSA
device/process; Bluetooth volume is synced over AVRCP), so a curve set
here only ever affects the device's own internal headphone/line-out jack.

See `plugins_examples/GainMode.lua` for a complete Low/High Gain
implementation using the real stock firmware's own measured curves (the
real firmware also defines a third "Medium" curve, omitted there because
it's numerically identical to Low on the real device it was extracted
from).

### ⏯️ Playback Control

Unlike the EQ functions above, these **do** go through `gui.c` bridges
(`gui_plugin_toggle_pause()` and neighbors, declared in `gui.h`) rather than
calling `src/audio/audio.c` directly -- the native UI always pairs a
playback change with other state (the play/pause icon, the volume
slider/popup, shuffle-aware next/prev stepping) that a direct `audio_*` call
would leave stale. Each function here calls the exact same local helper the
native UI itself uses, so a plugin-driven change looks identical to a
button/remote-control-driven one.

- **`plugin.toggle_pause()`** -- same as tapping the play/pause button
  (respects the same Bluetooth-DAC/AirPlay-mode block that button does).
- **`plugin.stop()`** -- stops playback outright (not resumable the way
  pause is).
- **`plugin.next_track()` / `plugin.prev_track()`** -- shuffle-aware
  next/previous, same stepping logic Prev/Next and a Bluetooth/phone remote
  already use.
- **`plugin.seek(seconds)`** -- seeks the current track to an absolute
  position.
- **`plugin.set_volume(percent)`** -- `percent` (0-100, clamped): sets
  system volume, also updating the on-screen volume slider/popup, same as a
  hardware volume-button press.
- **`plugin.is_playing()` / `plugin.is_paused()`** -> bool.
- **`plugin.get_position()` / `plugin.get_duration()`** -> number (seconds).

These are always called from inside a plugin callback (`on_open`,
`on_select`, a tile click), which is itself already dispatched synchronously
on the main UI thread from an LVGL click event -- the same thread every
native playback control already runs on, so there's nothing new to worry
about thread-safety-wise.

<a id="networking"></a>

## 🌐 Networking

> [!IMPORTANT]
> New plugins should use asynchronous `plugin.http_request()`. The older
> `http_get()` and `http_post()` functions block the UI thread while waiting.

### `plugin.http_get(url [, verify_tls])`

Synchronous GET request, bridging `src/network/http_client.c` (the same one
this app's own Subsonic integration uses) directly -- no `gui.c` layer
needed, since there's no LVGL/playback state involved. `verify_tls` defaults
to `true`; pass `false` only for a server you've deliberately chosen to
trust despite a self-signed/invalid certificate.

Returns `status, body` on success (`status` an HTTP status code, `body` the
full response as a Lua string -- binary-safe, not truncated at a NUL byte).
On a DNS/connect/TLS-level failure, returns `nil, "network error"` instead
of raising, so a plugin can show its own error toast rather than crash. A
real HTTP error status (404, 500, ...) is **not** treated as failure here --
you still get `status, body` back and inspect `status` yourself.

**Runs on the calling thread** -- i.e. whatever plugin callback invoked it,
always the main UI thread. A slow or hanging server blocks the whole app's
UI until the request completes or times out. Keep calls fast, or trigger
them from a user tap (a `register_list_item`/`show_list` row) rather than
anywhere that could be called in a loop.

### `plugin.http_post(url, body [, content_type] [, verify_tls])`

Same shape, return values, and blocking caveat as `http_get()` above, for a
POST. `content_type` defaults to `"application/x-www-form-urlencoded"` if
omitted or `nil` -- what most simple API POSTs (including Last.fm's) expect;
pass `"application/json"` or anything else your target API needs. `body` is
sent as-is, byte for byte -- this function doesn't URL-encode or otherwise
transform it, so build the string yourself first.

### `plugin.http_request(options, callback)` / `plugin.cancel(handle)`

Preferred non-blocking HTTP interface. It returns a request handle immediately;
DNS, connection, TLS, upload, and response reads happen on a native worker.
`callback(status, body, error, headers)` is later invoked on the main Lua/UI
thread. On success, `status` and the binary-safe `body` are set, `error` is
`nil`, and `headers` is a `{Name = "value", ...}` table of the response's
headers. On failure, `status` and `body` are `nil` and `error` is a short,
stable string (see the error codes below).

```lua
local handle, err = plugin.http_request({
    url = "https://example.com/api",
    method = "POST", -- GET, POST, PUT, PATCH, DELETE, or HEAD; GET is the default
    headers = { Authorization = "Bearer " .. token, ["X-Custom"] = "value" },
    body = '{"hello":"world"}',
    content_type = "application/json",
    verify_tls = true,
    max_response_bytes = 262144,
    connect_timeout_ms = 10000,
    read_timeout_ms = 15000,
    total_timeout_ms = 30000,
    redirect_limit = 5,
}, function(status, body, request_error, headers)
    if request_error then
        plugin.show_toast(request_error)
        return
    end
    plugin.show_toast("HTTP " .. status .. ", " .. #body .. " bytes, content-type=" ..
                       (headers["Content-Type"] or "?"))
end)
```

**Compatibility:** every field above except `url` is optional, and a callback
declared with the original 3 parameters (`status, body, error`) keeps working
completely unchanged -- Lua silently drops extra arguments a function doesn't
declare, so the new 4th `headers` argument is invisible to existing plugins.
`method` still defaults to `GET`; `headers`/`connect_timeout_ms`/
`read_timeout_ms`/`total_timeout_ms`/`redirect_limit` all default to their
exact previous (nonexistent) behavior when omitted -- no timeout, no
redirects followed (a 3xx response is returned as an ordinary result with
that status code, matching curl's own `--max-redirs 0` convention, not
treated as an error), no extra headers. `body`/`content_type` are only ever
sent for `POST`/`PUT`/`PATCH` -- exactly as before, when only `GET`/`POST`
existed and a `body` set on a `GET` request was silently ignored; that
silent-ignore behavior for non-body methods is preserved deliberately, not
newly introduced.

`max_response_bytes` defaults to 512 KiB and may be 1 byte through 2 MiB.
Request bodies are capped at 1 MiB. Request headers are capped at 16 entries.
At most four asynchronous requests may be active across all plugins. The
returned handle includes a generation number, so a stale handle cannot
affect a newer request that reused the pool slot.

A repeated response header name keeps only the LAST occurrence in the
`headers` table passed to your callback -- true case-insensitive
last-occurrence-wins (matching HTTP's own case-insensitive header-name
rule), not just a Lua-table-key coincidence: `Content-Type: a` followed by
`content-type: b` on the same response collapses to one `Content-Type`
(or `content-type`, whichever spelling came last) key holding `"b"`.

`connect_timeout_ms`/`read_timeout_ms`/`total_timeout_ms` may not exceed
5 minutes; `redirect_limit` may not exceed 10. A value outside that range
is a Lua error immediately, not silently clamped -- fix the value rather
than relying on it being adjusted for you.

**Redirects follow standard method/credential semantics**, not a blind
resend of the original request: a `303` always becomes a `GET` with no
body, regardless of the original method; `301`/`302` conventionally
rewrite `POST` to `GET` with no body (matching curl's own default and
every major browser); `307`/`308`, and any non-`POST` method on
`301`/`302`, preserve the original method and body exactly. Separately,
**`Authorization`/`Cookie`/`Cookie2`/`Proxy-Authorization` are stripped
from any redirect hop that changes origin** (scheme, host, or port) --
your bearer token never follows a redirect to a different host. An
`https://` request redirected to plain `http://` is refused outright
(`insecure_redirect`) while `verify_tls` is `true`, rather than silently
sending anything over the downgraded connection.

**Error codes** (the `error` string on failure): `invalid_url`,
`invalid_request` (a header name/value or the URL itself contained a
raw CR or LF byte -- rejected outright, not sanitized, since that's the
actual header-injection vector), `dns_failure`, `connect_failed`,
`connect_timeout`, `tls_failure`, `timeout` (a `total_timeout_ms`
deadline was hit -- this now genuinely bounds connect/TLS/upload/
response-header reads too, each stage using whatever's left of the
budget, not only the time between response body chunks), `cancelled`,
`too_many_redirects`, `response_too_large`, `malformed_response`,
`io_error`, `insecure_redirect`. These are stable strings safe to match
on in your own code, not just display.

`plugin.cancel(handle)` returns whether it matched an active request. Unlike
before, this now forces the underlying connection closed immediately (the
same technique the internet-radio player already used internally) -- it can
interrupt a request stuck in connect/TLS handshake, not only between
response chunks. One honest limitation: DNS resolution itself cannot be
interrupted this way (it's a single blocking system call with no portable
way to cancel it from another thread) -- a cancel requested while still
resolving the hostname is only noticed once resolution finishes, same as
before. The occupied worker slot becomes reusable as soon as the native
operation notices and unwinds, not only once the connection
finishes or times out on its own.

### `plugin.download_file_async(url, dest_path [, verify_tls], callback)`

Downloads a GET response directly to disk on a native worker without holding
the response in RAM or blocking the UI. `verify_tls` defaults to `true`.
The callback runs on the main Lua/UI thread as `callback(saved_path, error)`:
`saved_path` is the requested destination and `error` is `nil` on success;
on failure they are `nil, "download failed"`.

```lua
local dir = plugin.sd_root() .. "/Talks"
local ok, mkdir_err = plugin.mkdir(dir)
if ok then
    local handle, err = plugin.download_file_async(
        "https://example.com/talk.mp3",
        dir .. "/talk.mp3",
        function(saved_path, download_err)
            plugin.show_toast(download_err or ("Saved " .. saved_path))
        end)
end
```

The response is streamed into a unique temporary file beside the destination
and atomically renamed only after a complete HTTP 2xx response. Failed and
cancelled downloads remove the temporary file and leave an existing destination
unchanged. Download jobs share the four-slot asynchronous HTTP pool, and their
handles can be passed to `plugin.cancel(handle)`. Unlike buffered requests,
cancellation interrupts a file download during its next received chunk.

The older `http_get()` and `http_post()` remain available for compatibility,
but run synchronously on the UI thread. New plugins should use
`http_request()`.

### `plugin.md5(text)`

Returns the MD5 hash of `text` as a lowercase hex string. Bridges
`mbedtls_md5()` (already vendored, already used the same way by this app's
own Subsonic integration for its token auth) -- no new dependency. Useful
for any API that needs a request signed this way, e.g. Last.fm's own
`api_sig` scheme (see `plugins_examples/LastFmScrobbler.lua`).

### `plugin.json_decode(text [, limits])` / `plugin.json_encode(value [, limits])`

Bounded JSON support (wraps the vendored cJSON, already used internally for
Subsonic responses). Both run synchronously on the calling thread --
decode/encode a body you already fetched via `http_request()`, don't hold a
large body across a `set_interval()` tick waiting to parse it.

`json_decode(text, limits)` returns `value, nil` on success or `nil, error`
on failure. `limits` is optional; every field has a default and a hard
ceiling a plugin cannot raise past:

| Field | Default | Hard ceiling |
|---|---|---|
| `max_input_bytes` | 512 KiB | 512 KiB |
| `max_nesting` | 32 | 64 |
| `max_entries` | 10000 | 10000 |

The hard ceiling is the real safety limit on this ~56 MiB device -- a
decode simultaneously holds the source string, the cJSON tree, and the
converted Lua tables/strings, so the ceiling is set to what the *default*
used to be, not a higher number a plugin could opt into via `limits`.
Passing a larger value in `limits` is silently clamped down to the
ceiling, not an error.

`json_encode(value, limits)` returns `text, nil` or `nil, error`. Its
`limits` table uses `max_output_bytes` (same default/ceiling numbers as
`max_input_bytes` above) instead of `max_input_bytes`, plus the same
`max_nesting`/`max_entries`.

Known limitations, not bugs -- both follow ordinary JSON-in-a-dynamic-
language conventions, not something specific to this player:
- JSON `null` decodes as Lua `nil`, indistinguishable from a missing table
  key or array hole. Don't rely on `null`'s mere presence in a response.
- Numbers decode as Lua numbers (IEEE-754 double) -- exact only up to
  2^53. A provider that needs an exact large ID should emit it as a JSON
  *string*; treat provider track/album/artist IDs as strings in your own
  plugin code rather than relying on round-tripping a huge JSON integer.
- A Lua table with `#t > 0` encodes as a JSON array (indices `1..#t`); any
  other table, including an empty one, encodes as a JSON object. Object
  keys must be Lua strings -- a non-string key is a clean encode error,
  not a crash.
- Invalid UTF-8 in either direction is a clean decode/encode error rather
  than being silently passed through or replaced.

```lua
local resp_text = ... -- from an http_request() callback's result body
local data, err = plugin.json_decode(resp_text)
if not data then
    plugin.show_toast("Bad response: " .. err)
    return
end

local body, err2 = plugin.json_encode({ query = "abba", limit = 50 })
```

### `plugin.storage` / `plugin.secrets`

Namespaced key/value storage, private to your plugin's own `id` (from
`plugin.define`) and stored under this device's internal persistent
partition -- **never the removable SD card**, so it survives an SD swap
and isn't readable by just pulling the card.

```lua
plugin.storage.set("last_sync", tostring(os.time()))
local last = plugin.storage.get("last_sync", "never")

local keys = plugin.storage.list("cache_") -- array of matching key names
plugin.storage.delete("last_sync")
```

- `plugin.storage.get(key [, default])` -- returns the stored value, or
  `default` (or `nil` if omitted) when the key doesn't exist.
- `plugin.storage.set(key, value)` -- `value` is any Lua string, including
  binary bytes (up to 256 KiB per value); returns `true`, or `false` if it
  failed (invalid input, or a quota below would be exceeded). A `set()`
  that would store the exact same bytes already on disk is a fast no-op --
  it doesn't touch flash again.
- `plugin.storage.delete(key)` -- returns whether a key was removed *and*
  that removal's durability across an immediate power loss was confirmed.
  Same nuance as `set()` below: `false` can mean the key really is gone
  but that durability confirmation failed, not that it's still there.
- `plugin.storage.list(prefix)` -- returns an array of key name strings
  starting with `prefix` (or all keys, if `prefix` is omitted/`""`).

**Quotas**: each plugin's combined `storage` + `secrets` usage is capped
at 2 MiB and 500 keys, and every plugin's byte usage combined is capped
at 8 MiB total -- this keeps one plugin from filling the same
`/usr/data` partition the app's own settings and music database need to
write to. Usage is tracked with an in-memory cache seeded from a real
directory scan the first time a plugin id is touched and updated
incrementally on every write/delete afterward (periodically re-validated
against a fresh scan), not recomputed by scanning every file on every
single call -- `set()` stays fast even with many existing keys. `set()`
returns `false` if a quota would be exceeded; there's no separate error
code for "quota" vs. other failures yet, so check the return value, not
just assume success. `false` can also mean the value was written and
already exists on disk, just that the write's durability across an
immediate power loss couldn't be confirmed -- not that nothing happened.
Don't retry a "failed" `set()` in a tight loop assuming it's a no-op.

`plugin.secrets` has the identical `set`/`exists`/`delete` shape for
credentials (tokens, refresh tokens) but **deliberately no `list()`** --
your plugin has to remember its own secret key names, so nothing can
enumerate what secrets exist just by holding a reference to the plugin's
id.

```lua
plugin.secrets.set("access_token", token)
if plugin.secrets.exists("access_token") then ... end
plugin.secrets.delete("access_token") -- e.g. on log out
```

**Threat model, stated plainly:** this device has no confirmed hardware
keystore or secure enclave. "Secrets" storage means restrictive file
permissions (owner-only, `0600`/`0700`) on the internal partition rather
than the SD card, **plus** a path guard on `io.open()`/`io.lines()`/
`io.input()`/`io.output()`/`os.remove()`/`os.rename()` that refuses any
path resolving into `/usr/data/plugins/` (see "Sandboxing" above) -- that guard, not the file
permissions, is what actually stops one plugin from reading another's
secrets: every plugin's Lua code runs in the *same* OS process and user,
so 0600/0700 alone would do nothing against a plugin that simply
`io.open()`'d another plugin's predictably-named secrets file directly.
Neither layer defends against a local attacker with root or physical
access to this specific running device; there's no device-unique key
available to this codebase to wrap the data with that such an attacker
couldn't equally derive. Don't describe this to your own users as
"encrypted" storage -- it isn't.

### `plugin.media_capabilities()`

Reports what this build's audio pipeline actually supports, as a plain
table -- useful for a remote-catalog plugin deciding what quality tier to
request:

```lua
local caps = plugin.media_capabilities()
-- caps.codecs               = {"mp3", "aac", "flac"}
-- caps.containers           = {"mp3", "adts", "flac"}
-- caps.max_bit_depth        = 16   -- every output path (internal DAC,
--                                     USB DAC, Bluetooth) is fixed 16-bit
-- caps.max_channels         = 2
-- caps.max_sample_rate      = 0    -- not independently verified against
--                                     real hardware limits yet; treat as
--                                     informational only, not a real cap
-- caps.direct_http_streaming = true
-- caps.range_seeking        = false -- not built yet
-- caps.hls / caps.dash      = false
-- caps.encryption_modes = {}, caps.drm_systems = {} -- none supported
```

### `plugin.show_text_input(title, initial_text, is_password, on_submit)`

Opens the app's own T9 multi-tap keypad text-entry screen -- the same one
used for Wi-Fi passwords and Subsonic server login.

- `title` (string): the screen's header text.
- `initial_text` (string or `nil`): pre-fills the field; `nil` starts empty.
- `is_password` (boolean): masks the input.
- `on_submit` (function): called with the entered text when the T9 keypad's
  Enter key is pressed.

**`on_submit` is not called if the user backs out instead of submitting** --
not with an empty string, not at all. Don't assume it always fires.

This is a **true singleton screen**. The call returns `true` when it opens, or
`false, "text input busy"` if another plugin request is pending. Cancelling
with Back releases ownership. Chaining calls from within `on_submit` itself
(for example username followed by password) works because the first request is
released before its callback runs.

### `plugin.get_now_playing()`

Returns `title, artist, album, duration_seconds` for whatever's currently
loaded, or a single `nil` if nothing has played yet this session. The same
metadata a `"track_started"` event handler already receives as arguments
(see "Events" below) -- this is for code that isn't reacting to that event
directly, e.g. a `set_interval()` tick double-checking what's still playing.

### `plugin.get_play_mode()`

Returns one of `"sequential"`, `"repeat_all"`, `"repeat_one"`, `"shuffle"` --
the current state of the order/loop/single/random icon on the player
screen.

### `plugin.get_current_track_path()`

Returns the absolute path of the currently loaded track, or `nil` if
nothing is loaded (no active playlist yet, or playback was stopped rather
than paused).

### `plugin.get_artist_albums(artist)`

Returns `{ album_name, ... }` (a plain array, 1-indexed) for every album
that has at least one song tagged with this artist, in the same
alphabetical order the on-device Artists screen's own drill-down uses. `nil`
if `artist` doesn't match any song's artist tag. Matching is
case-insensitive, same as every other Artists/Albums grouping in this app.

### `plugin.get_album_tracks(artist, album)`

Returns `{ track_path, ... }` for one artist's one album, in the same order
the on-device album view would display them (path-sorted). `nil` if either
`artist` or `album` doesn't match. `artist` scopes the lookup so a
same-named album by a different artist is never returned by mistake.

### `plugin.get_next_album_tracks(artist, current_album)`

Same return shape as `get_album_tracks()` above, for whichever album comes
immediately after `current_album` in that artist's own alphabetical album
order. Returns `nil` both when `current_album` doesn't match anything and
when it's already the artist's last album -- this does **not** wrap around
to the first album. Meant for auto-continuing playback across an artist's
discography once an album finishes; pair with `"track_started"` or
`get_current_track_path()` to detect the album boundary.

### Paged library access (`library_*`)

Unlike `get_artist_albums()`/`get_album_tracks()`/`get_next_album_tracks()`
above (already scoped to one artist or album), these query the on-disk
library database directly and are bounded per call -- none of them ever
materializes the whole library, regardless of how many songs it has. Every
call accepts `offset`/`limit`; `limit` is silently clamped to 200 (songs) or
200 (artist/album groups) if omitted or too large. Check
`plugin.has_capability("library.paged")` before relying on this group if you
need to support older player builds.

A **song** table has the fields `id`, `path`, `title`, `artist`, `album`,
`album_artist`. `id` is a stable identifier for that song (survives a
re-scan) -- hand it back to `library_get_song(id)` later rather than storing
a path if you need to look the same song up again. A **group** table (from
`library_get_artists`/`library_get_albums`) has `name`, `count` (song
count), `first_song_id` (a representative song, e.g. for cover art), and
`album_artist` (only meaningful from `library_get_albums` -- an album's
real identity is the *pair* of its name and album artist, so two different
artists' same-titled albums show as separate rows with this field set to
tell them apart; empty from `library_get_artists`).

- `plugin.library_song_count()` -- total number of songs in the library.
- `plugin.library_get_songs([offset], [limit], [filters])` -- returns
  `{ song, ... }, total`. `filters` is an optional table with any of
  `query` (title/artist substring match), `artist`, `album_artist`, `album`
  (each an exact, case-insensitive match) -- every field is optional.
  `total` is the match count across every page, useful for a "page N of M"
  UI.
- `plugin.library_search(query, [limit])` -- `{ song, ... }`, a plain
  title/artist substring search with no offset (always replace, not append,
  your previous result set, same convention this app's own on-device search
  follows).
- `plugin.library_get_song(id)` -- one song table, or `nil` if `id` doesn't
  match any song (e.g. it was deleted in a later re-scan).
- `plugin.library_get_artists([offset], [limit])` -- `{ group, ... }`.
- `plugin.library_get_albums([offset], [limit], [artist])` -- `{ group,
  ... }`. `artist` restricts to one artist or album\_artist's own albums;
  omit it for every album, unfiltered.

### `plugin.refresh_library()`

Triggers the same background rescan as Settings -> Update Music Database --
useful after a plugin writes new files under `plugin.sd_root()`, since none
of the `library_*()` functions above notice new files on their own;
there's no filesystem watcher, only a rescan triggered by this, that same
Settings row, an SD reinsert, or a restart. Shows the same native "Updating
music database..." progress screen a user tapping that row would see.
Returns `true, "started"` when accepted, or `false, "already_running"` /
`false, "rate_limited"`. Each plugin may start at most one scan per minute;
requests made while a scan is active are coalesced by returning
`already_running`. Check `plugin.has_capability("library.refresh")` before
relying on this interface.

```lua
plugin.define({ id = "org.example.library_browser", api_min = 1 })

local songs, total = plugin.library_get_songs(0, 50, { artist = "Boards of Canada" })
plugin.show_toast(("Found %d songs (%d shown)"):format(total, #songs))

local artists = plugin.library_get_artists(0, 100)
for _, a in ipairs(artists) do
    print(a.name, a.count)
end
```

<a id="events"></a>

## 🔔 Events and Timers

### `plugin.on(event, callback)`

Subscribes to a playback lifecycle change your plugin didn't itself cause.
Unlike `register_list_item()` (where each plugin's row coexists as its own
list entry), an event has no UI real estate to divide up -- **every**
plugin subscribed to a given event fires, not just the first or the most
recent. Four recognized events:

- `"track_started"` -- `callback(title, artist, album, duration_seconds,
  provider, track_id)`. Fires whenever a new track begins playing, whatever
  the cause: a manual tap, next/prev, a gapless or crossfade auto-advance, a
  plugin's own `play_file()`/`play_list()`/`play_remote()`, or remote
  control. There's deliberately no separate "next"/"prev" event -- from a
  subscriber's point of view it's the same fact ("a new track started,
  here's its metadata") regardless of what triggered it. `provider`/
  `track_id` are `""` for a local or Subsonic track, and only non-empty for
  one started via `plugin.play_remote()`/`queue_remote_list()` -- added
  after `duration_seconds`, so an existing handler declared with only 4
  parameters keeps working unchanged (Lua silently drops extra arguments a
  function doesn't declare).
- `"paused"` / `"resumed"` -- `callback()`, no arguments. Fire on every
  pause/resume transition, however it was triggered (the play/pause button,
  a hardware button, `plugin.toggle_pause()`).
- `"stopped"` -- `callback()`, no arguments. Fires when playback stops
  outright (not paused) -- deleting the currently-playing song, a DLNA stop
  request, enabling Bluetooth/USB/AirPlay DAC mode (which force-stops local
  playback), or `plugin.stop()`. **Known gap**: does *not* fire when a
  playlist simply reaches its end with Repeat off -- that path doesn't
  route through any of this app's own explicit stop points. Not needed for
  scrobbling (which only cares about elapsed listening time, and simply
  stops noticing once nothing's playing), but worth knowing if you're
  relying on it for something else.

Passing an unrecognized event name raises a Lua error immediately, same
convention as an unrecognized `list_id`. Capped at 8 subscribers per event,
across every loaded plugin combined.

### `plugin.set_interval(seconds, callback)` / `plugin.clear_interval(handle)`

A generic repeating timer -- for anything that needs to poll rather than
react to an event, e.g. checking `get_position()` against some threshold
periodically. `set_interval()` returns a `handle` to later pass to
`clear_interval()` to stop it. `seconds` below 1 is silently clamped up to
1 second (not an error) -- this prevents one plugin from flooding the main
UI thread with timer callbacks. Capped at 8 concurrently-active intervals,
across every loaded plugin combined -- exceeding that raises a Lua error.

A callback registered this way runs on the main UI thread, exactly like
every other `plugin.*` callback -- a slow `http_get()`/`http_post()` inside
one will visibly stall the whole UI until it returns, same tradeoff
`http_get()` itself already documents.

<a id="examples"></a>

## 🧪 Complete Examples

| Example | What it demonstrates |
|---|---|
| `Audiobooks.lua` | SD browsing, nested lists, chapter playback, progress |
| `NetRadio.lua` | Stream Media tile and live MP3 streams |
| `Themes.lua` | Display row, icon overrides, background/text colors |
| `SoundProfiles.lua` | PEQ profile selection and persistence |
| `MSEB.lua` | Chained settings-list screens, summed EQ band contributions, backup/restore |
| `GainMode.lua` | Hardware volume curve switching, real stock Low/High Gain curves, safe unset-until-chosen default |
| `PlaybackExtras.lua` | Native-looking toggles, sliders, and nested settings |
| `PlayThrough.lua` | Natural-end detection and folder/album continuation |
| `ExtendedSleepTimer.lua` | Persistent duration, session timer, and delayed playback stop |
| `LastFmScrobbler.lua` | Events, timers, text input, MD5, async HTTP |
| `AsyncHttp.lua` | Bounded requests and cancellation |
| `PluginApiInfo.lua` | Identity, version, and capability discovery |
| `NestedLists.lua` | Nested callback ownership and text-input busy handling |

The summaries below explain when each example is useful and call out its
important implementation details.

`plugins_examples/Audiobooks.lua` uses most of the API: `sd_root()`/
`list_dir()` to browse `Audiobooks/<book>/` folders on the SD card,
`show_list()` twice (book folder, then chapter list, chaining from the
first's `on_select` into the second), `play_list()` to start playback from
whichever chapter was tapped, and `show_toast()` for the "no chapters
found" / "no audiobooks found" empty states. Read it top to bottom as the
reference implementation for `register_list_item()`; the `README.md`'s
Plugins section has the install steps (copy to
`.plugins/Audiobooks.lua`, create an `Audiobooks/<book>/` folder
structure).

`plugins_examples/NetRadio.lua` is the reference implementation for both
`register_stream_media_tile()` and live stream URLs. It reloads
`<SD card>/Radio.txt` whenever its tile opens, displays each station through
`show_list()`, and starts it with `plugin.play_list()`. Use one station per
line as `Station Name | http(s)://direct-mp3-stream`; blank lines and `#`
comments are ignored, and URL-only lines are accepted. A ready-to-copy
`plugins_examples/Radio.txt` is included. The plugin header covers the
remaining live-stream limitations (MP3-only, no seeking or auto-reconnect).

`plugins_examples/Themes.lua` is the reference implementation for the full
theme API. It discovers `.theme` files, installs their icon sets, applies
colors/Home layout, persists the selection, and uses `refresh_theme()` for
live switching without disrupting navigation or services.

`plugins_examples/SoundProfiles.lua` is the reference implementation for the
EQ functions -- a handful of curated `.peq` presets reachable from
Settings -> Playback -> Sound Profile, switched with `plugin.eq_load_profile()` and
`plugin.show_list()`, following the exact same "`register_list_item` ->
`show_list` -> apply and persist to a state file" shape `Themes.lua` already
established for theme switching.

`plugins_examples/MSEB.lua` builds on the same EQ functions -- ten
mood-based tuning sliders (Sound Temperature, Bass Extension, Vocal
Position, and so on, modeled after the stock HiBy firmware's own "MSEB"
feature, reverse-engineered down to the real slider list but not its
unrecoverable mapping formulas -- this plugin's mapping is original) grouped
across three chained `show_settings_list()` screens, since
`PLUGIN_SETTINGS_LIST_MAX_SLIDERS` caps a single call at 4. Two axes share
a PEQ band each (a whole-spectrum "tilt" control alongside a dedicated
bass/treble one) -- demonstrates summing contributions into a shared band
with `plugin.eq_set_band()` rather than one axis overwriting another's.
Also demonstrates snapshotting the live PEQ curve with
`plugin.eq_save_profile()` the first time the feature is enabled and
restoring it with `plugin.eq_load_profile()` on disable, so toggling it
never destroys a hand-tuned manual curve -- and calling `eq_set_band()`
only for the specific band(s) a changed slider actually owns, never all 10
bands on every release, since each call persists to disk immediately.

`plugins_examples/PlaybackExtras.lua` is the reference implementation for
`register_list_item("playback", ...)` and `show_settings_list()` -- a
"Loudness Boost" row in Settings -> Playback opening a submenu with a real
toggle switch, a real slider (both driving `plugin.eq_set_preamp()`), and a
nested `"row"` ("About") that opens a second `show_settings_list()` screen
on top of the first, demonstrating submenu-inside-a-submenu nesting -- and,
since this session's row-images/resizing/text-size work, the toggle row's
own `icon` (pointed at a real stock theme2 asset by its raw filesystem
path) and the "About" row's `text_size = "large"`.

`plugins_examples/PlayThrough.lua` adds persistent **Play Through Folders**
and **Play Through Albums** controls under Settings → Playback. It combines
playback events, a short polling interval, library album queries, SD-card
directory browsing, and `play_list()` to continue only after a genuine
natural playlist end. Album continuation has priority when both options are
enabled; folder continuation is the fallback. Explicit Stop and non-sequential
play modes are deliberately respected.

`plugins_examples/ExtendedSleepTimer.lua` adds an **Extended Sleep Timer**
under Settings → Playback with a 15–180 minute range. It demonstrates a
persistent preferred duration, session-only armed state, countdown status,
and a lightweight interval that calls `plugin.stop()` at expiration. Like the
native timer, an active countdown does not survive restarting the player.

`plugins_examples/LastFmScrobbler.lua` is the reference implementation for
`plugin.on()`, `set_interval()`/`clear_interval()`, `get_now_playing()`,
`http_request()`, `md5()`, and `show_text_input()` together -- a real Last.fm
scrobbler. Logs in via `show_text_input()` (username, then password,
chained), signs and POSTs `auth.getMobileSession` to obtain a session key
(persisted; the password itself isn't), sends `track.updateNowPlaying` on
every `"track_started"` event, and uses a 15-second `set_interval()` to
POST `track.scrobble` once `get_position()` crosses Last.fm's own "50% or 4
minutes played" threshold. Every request is asynchronous, so a slow Last.fm
connection cannot freeze touch input. A scrobble is marked complete only after
Last.fm accepts it; failures remain eligible for retry. Requires a free Last.fm
API account -- see the plugin's own header comment for where to get one and
where to put the key.

`plugins_examples/AsyncHttp.lua` is a compact `http_request()` and `cancel()`
example. It starts a bounded HTTPS GET, demonstrates that the UI remains
responsive, reports its completion callback, and offers a separate cancel row.

`plugins_examples/PluginApiInfo.lua` demonstrates `define()`, `api_version()`,
`get_app_info()`, and `has_capability()` by presenting the current build and a
few optional interfaces in a list.

`plugins_examples/NestedLists.lua` exercises the corrected per-screen callback
ownership: open a child list, go Back, and the parent callback remains active.
It also demonstrates the success/busy return contract of `show_text_input()`.

<a id="testing"></a>

## 🚀 Writing and Testing Your Plugin

1. Write the plugin in any text editor—there is no separate plugin build.
2. Optionally run `luac -p MyPlugin.lua` to catch syntax errors locally.
3. Copy it to `<SD card>/.plugins/`.
4. Either restart the player, or open Settings -> System -> Plugins and tap
   "Apply Plugin Changes" to pick up the new/edited file without restarting (same
   full reload `plugin.reload_ui()` itself triggers -- every other loaded
   plugin's top-level code re-runs too, and navigation lands back on Home).
   That screen also has a per-plugin on/off toggle. **The toggle only
   controls whether a plugin's script runs on the next reload -- it does
   not undo anything that plugin already did.** A plugin that only adds
   list rows/tiles or registers callbacks disappears cleanly. A plugin that
   called `set_icon()` (theme icon files under `theme_overrides/`, never
   attributed to any one plugin) or the `eq_*` family (persisted EQ state)
   leaves those changes in place after being toggled off; only reverting
   them yourself (or a plugin that explicitly does so, e.g. re-toggling a
   theme back on and picking a different one) removes them.
5. Follow [TESTING.md](TESTING.md) when launching through ADB and watch the
   foreground output.

### Reading plugin errors

| Log shape | Meaning |
|---|---|
| `[plugins] failed to load <path>: <error>` | Syntax error or failure in top-level plugin code |
| `[plugins] <context> error: <error>` | A callback failed after the plugin loaded |

The context identifies the failing callback, such as a list item's
`on_open`, `show_list on_select`, a settings `on_change`, an event handler,
an interval callback, or text-input submission.

> [!NOTE]
> A broken plugin or callback is isolated. It is logged and skipped without
> crashing the player or disabling other plugins.

Multiple plugins can register rows and Stream Media tiles at the same time;
there is no “first plugin wins” limitation.

## 🛠️ Extending the `plugin.*` API Itself

If you're modifying `plugin_manager.c` to add a new function (not writing
a plugin script, but adding to what plugins *can* call): follow the
existing `l_plugin_*` functions as the pattern -- `luaL_check*`/`luaL_opt*`
to pull typed arguments off the Lua stack, do the work, push however many
return values, `return <that count>;`, then add the new
`{ "name", l_plugin_name }` entry to the `plugin_funcs[]` table.

"Do the work" means one of two things, depending on whether LVGL/screen
state is involved:

- If it touches LVGL objects/styles or other `gui.c`-local state (screens,
  widgets, the current playlist/play-button icon) -- go through a
  `gui_plugin_*` bridge function declared in `gui.h` and implemented in
  `gui.c`, same as `show_list`/`play_file`/`set_background_color`/the
  playback-control functions all do. `gui.c` is where that state actually
  lives; `plugin_manager.c` itself has no LVGL/screen code of its own.
- If it's a self-contained subsystem with no LVGL dependency of its own
  (`src/audio/peq.c`'s EQ engine, `src/network/http_client.c`'s HTTP client)
  -- call its public API directly from `plugin_manager.c`, the way
  `plugin.eq_*()`/`plugin.http_get()` and `list_dir`/`sd_root` already do.
  No pointless `gui.c` indirection for something that was never a
  screen/widget concern in the first place.

When in doubt, check whether the native UI's own call sites for that
functionality do anything beyond the bare library call (icon updates, extra
persistence, a guard condition) -- if they do, that logic needs to live in
the `gui.c` bridge too, not be silently skipped by a plugin. This is exactly
why playback control (`toggle_pause`/`stop`/`next_track`/...) goes through
`gui.c` while the EQ and HTTP functions don't: `audio.c`'s own functions are
always paired with `gui.c`-local UI updates at their native call sites,
while `peq.c`/`http_client.c` are not.
