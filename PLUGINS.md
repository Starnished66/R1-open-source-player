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
skipped. Up to 16 plugin files (`PLUGIN_MAX_FILES`) are loaded this way.

Each `lua_State` is kept open for the rest of the app's lifetime, never
closed -- a plugin's callbacks (`on_open`, a `show_list` row's
`on_select`) are Lua closures that need their owning state alive to be
invoked later, from a tap that can happen minutes after load.

If a file fails to load or errors while running its top-level code
(`luaL_dofile()` returning non-`LUA_OK`), that one file is skipped -- logged
to stderr as `[plugins] failed to load <path>: <error>` -- without
affecting any other `.lua` file in the folder.

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
happens to have room for this cap because it only has 1 built-in tile,
unlike Home, which is already full at 6 and has no room for plugin tiles
of its own at all.

## 🧰 API Reference

Every function below is a field on the global `plugin` table, available
from the moment your script starts running (injected before
`luaL_dofile()`). All of it is implemented in `src/plugins/plugin_manager.c`.

| Area | Main APIs |
|---|---|
| Identity | `define`, `api_version`, `has_capability`, `get_app_info` |
| UI | `register_list_item`, `register_stream_media_tile`, `show_list`, `show_settings_list`, `show_text_input`, `show_toast` |
| Theme | `set_icon`, `set_background_color`, `set_text_color` |
| Files | `sd_root`, `list_dir` |
| Playback | `play_file`, `play_list`, transport controls, playback state |
| Library | `get_artist_albums`, `get_album_tracks`, `get_next_album_tracks` |
| Audio | `eq_load_profile`, `eq_save_profile`, `eq_set_*`, `eq_reset` |
| Network | `http_request`, `cancel`, legacy `http_get`/`http_post` |
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
    api_min = 1,
})
```

`id` is a stable identifier using letters, digits, `.`, `_`, and `-`; it must
be unique among loaded plugins. `api_min` rejects the plugin at load time when
the player API is too old. Existing plugins without `define()` remain supported
as legacy plugins using an identity derived from their filename.

- `plugin.api_version()` returns the current integer plugin API version.
- `plugin.has_capability(name)` reports whether an optional interface exists.
  API 1 includes `ui.list`, `ui.settings`, `ui.row_width`, `ui.text_input`, `ui.toast`,
  `ui.theme`, `filesystem.sd`, `playback.control`, `playback.state`,
  `playback.events`, `library.artist_albums`, `network.http.sync`, `network.http.async`,
  `crypto.md5`, and `audio.peq`.
- `plugin.get_app_info()` returns `version`, `build`, `platform`, and
  `plugin_api` fields.

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
- `options` (table, optional): `{ height = n, width = n }` -- resizes every row in
  this call (not per-row -- a plain browsing list mixing wildly different
  row heights would look broken in a way an occasional taller settings-
  submenu row doesn't).

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
- **`text_size`** (string) -- `"small"`, `"medium"`, or `"large"`. Every
  size uses a font with full non-Latin fallback (Cyrillic, CJK, Korean,
  Thai) -- correct for plugin-authored text, which (unlike this app's own
  fixed English UI chrome) might not be English. An unrecognized value
  raises a Lua error; omitting it keeps that row type's own existing
  default size.

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

### `plugin.show_toast(message)`

Shows the same transient toast used elsewhere in the app (e.g. "Added to
queue"). Useful for "nothing found" / error feedback -- see
`Audiobooks.lua`'s use of this when a book folder has no playable chapter
files in it.

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
`callback(status, body, error)` is later invoked on the main Lua/UI thread.
On success, `status` and the binary-safe `body` are set and `error` is `nil`.
On failure, `status` and `body` are `nil` and `error` describes the failure.

```lua
local handle, err = plugin.http_request({
    url = "https://example.com/api",
    method = "POST", -- GET or POST; GET is the default
    body = '{"hello":"world"}',
    content_type = "application/json",
    verify_tls = true,
    max_response_bytes = 262144,
}, function(status, body, request_error)
    if request_error then
        plugin.show_toast(request_error)
        return
    end
    plugin.show_toast("HTTP " .. status .. ", " .. #body .. " bytes")
end)
```

`max_response_bytes` defaults to 512 KiB and may be 1 byte through 2 MiB.
Request bodies are capped at 1 MiB. At most four asynchronous requests may be
active across all plugins. The returned handle includes a generation number,
so a stale handle cannot affect a newer request that reused the pool slot.

`plugin.cancel(handle)` returns whether it matched an active request. It
suppresses that request's callback and releases its resources when the native
operation returns. Cancellation is currently logical rather than a forced
socket close, so the occupied worker slot is not reusable until the underlying
connection finishes or times out.

The older `http_get()` and `http_post()` remain available for compatibility,
but run synchronously on the UI thread. New plugins should use
`http_request()`.

### `plugin.md5(text)`

Returns the MD5 hash of `text` as a lowercase hex string. Bridges
`mbedtls_md5()` (already vendored, already used the same way by this app's
own Subsonic integration for its token auth) -- no new dependency. Useful
for any API that needs a request signed this way, e.g. Last.fm's own
`api_sig` scheme (see `plugins_examples/LastFmScrobbler.lua`).

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

<a id="events"></a>

## 🔔 Events and Timers

### `plugin.on(event, callback)`

Subscribes to a playback lifecycle change your plugin didn't itself cause.
Unlike `register_list_item()` (where each plugin's row coexists as its own
list entry), an event has no UI real estate to divide up -- **every**
plugin subscribed to a given event fires, not just the first or the most
recent. Four recognized events:

- `"track_started"` -- `callback(title, artist, album, duration_seconds)`.
  Fires whenever a new track begins playing, whatever the cause: a manual
  tap, next/prev, a gapless or crossfade auto-advance, a plugin's own
  `play_file()`/`play_list()`, or remote control. There's deliberately no
  separate "next"/"prev" event -- from a subscriber's point of view it's
  the same fact ("a new track started, here's its metadata") regardless of
  what triggered it.
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
| `PlaybackExtras.lua` | Native-looking toggles, sliders, and nested settings |
| `PlayThrough.lua` | Natural-end detection and folder/album continuation |
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
`register_stream_media_tile()` and live stream URLs -- a hardcoded list of
internet radio stations (label + `http(s)://` MP3 stream URL), shown via
`show_list()`, played with `plugin.play_list()`. Its own header comment
covers the "Live stream URLs" caveats above (MP3-only, no seeking, no
auto-advance into or out of it).

`plugins_examples/Themes.lua` is the reference implementation for
`"display"` list items, `set_icon()`, `set_background_color()`, and
`set_text_color()` together -- a Dark/White theme switcher reachable from
Settings -> Display. It reskins icons app-wide by copying from the stock
firmware's own `theme1`/`theme2` litegui asset packs (both present on every
real R1 at `/usr/resource/litegui/`), persists the chosen theme in its own
state file under `.plugins/` (re-applied at the top of the script on every
boot, since that's how every plugin already survives a restart -- no native
settings storage involved), and is a useful reference for the load-time-only
constraint on `set_icon()` (backgrounds/text apply live; icons need a
restart to fully catch up after switching mid-session).

`plugins_examples/SoundProfiles.lua` is the reference implementation for the
EQ functions -- a handful of curated `.peq` presets reachable from
Settings -> Playback -> Sound Profile, switched with `plugin.eq_load_profile()` and
`plugin.show_list()`, following the exact same "`register_list_item` ->
`show_list` -> apply and persist to a state file" shape `Themes.lua` already
established for theme switching.

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
4. Restart the player; there is currently no hot reload.
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
