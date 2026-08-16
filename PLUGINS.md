# Writing plugins

Reference for authoring a third-party Lua plugin. Everything here is
derived directly from `src/plugins/plugin_manager.c` (the C side) and
`plugins_examples/Audiobooks.lua` (a complete real example) -- if this
document and the source ever disagree, the source is right.

## How a plugin gets loaded

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

## Reaching a plugin from the UI

There are two separate registries, reached two different ways -- pick
whichever fits your plugin's own subject matter:

**`plugin.register_list_item(list_id, ...)`** -- every plugin that calls
this gets its own row, appended to whichever native list screen `list_id`
names, after that screen's own built-in rows. Three recognized values:

- `"books"` -- the **Books** screen, after "Books"/"Favorites", up to
  `PLUGIN_MAX_BOOKS_LIST_ITEMS` (currently 8) combined across plugins.
- `"settings"` -- the top-level **Settings** screen, after "Playback"/
  "Display"/"Power"/"System"/"About", up to `PLUGIN_MAX_SETTINGS_LIST_ITEMS`
  (currently 8).
- `"display"` -- the **Settings -> Display** sub-screen, after "Accent
  Color"/"Font Size"/"Screen Timeout"/"Swipe Up for Home", up to
  `PLUGIN_MAX_DISPLAY_LIST_ITEMS` (currently 8). This is where a
  display-theming plugin's own entry point belongs -- see
  `plugins_examples/Themes.lua`.

Passing anything else raises a Lua error at load time rather than silently
registering into nothing. If no plugin registers a row for a given
`list_id`, that screen just shows its native rows, no placeholder.
`build_pill_list_screen()`'s rows genuinely scroll (real-device confirmed),
unlike the icon grid below, so there's no "only the first plugin wins"
limitation here the way the icon-grid registry has to work around.

**`plugin.register_stream_media_tile()`** -- every plugin that calls this
gets a real, visible icon-grid tile, appended to **Stream Media** after the
built-in Subsonic tile (up to `PLUGIN_MAX_STREAM_TILES`, currently 5, so 6
total). Unlike the Books list above, `build_icon_grid_screen()`'s grid
**cannot be scrolled** (real-device testing confirmed) -- Stream Media
happens to have room for this cap because it only has 1 built-in tile,
unlike Home, which is already full at 6 and has no room for plugin tiles
of its own at all.

## The `plugin` API

Every function below is a field on the global `plugin` table, available
from the moment your script starts running (injected before
`luaL_dofile()`). All of it is implemented in `src/plugins/plugin_manager.c`.

### `plugin.register_list_item(list_id, label, on_open)`

Adds a row to an existing native list screen.

- `list_id` (string): which screen to add to -- `"books"`, `"settings"`, or
  `"display"` (see "Reaching a plugin from the UI" above for what each
  targets) -- anything else raises a Lua error immediately, rather than
  silently registering into nothing.
- `label` (string): the row's visible text.
- `on_open` (function): called with zero arguments when the row is tapped.
  This is where you'd call `plugin.show_list()` to show your first screen.

Every plugin that calls this gets its own row (unlike the old
`register_tile()` this replaced, where only the first caller was ever
reachable) -- a script can still call it more than once if it genuinely
wants multiple independent entry points into the same list.

Errors if more than that `list_id`'s own cap (currently 8 for each of the
three) is registered, across every loaded plugin combined.

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
brand-new tile at whatever icon it likes, no special handling needed).
`register_list_item()` has no icon argument at all -- pill-list rows have
no icon slot to fill. For example,
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

### `plugin.show_list(title, items, on_select)`

Opens a list screen.

- `title` (string): the screen's header text.
- `items` (array table of strings): one row per entry, shown in order.
- `on_select` (function): called with the **1-based** index of whichever
  row was tapped (Lua array convention, not C's 0-based one) when the user
  taps a row. Not called if the user backs out without tapping anything.

Each call opens a **new** screen (from a pool of 4 reusable ones -- see
`PLUGIN_LIST_SCREEN_POOL_SIZE` in `gui.c`), so calling `show_list` again
from inside an `on_select` callback (to drill into a subfolder, for
example) pushes a second screen on top rather than replacing the first --
the device's Back gesture/button naturally returns to the previous list.
Nesting more than 4 levels deep reuses an earlier pool slot and will
corrupt back-navigation at that depth; no real plugin should need to nest
that far.

Only the **most recently opened** `show_list` call's `on_select` is
"live" -- if your plugin has two list screens open at once (one pushed on
top of the other) and the user backs out to the first, tapping a row there
will still route through this most-recent registration until that first
screen calls `show_list` again itself. In practice, structuring your
plugin as "each screen's `on_select` immediately calls `show_list` again
for the next screen" (exactly what `Audiobooks.lua` does) avoids ever
hitting this.

Items beyond the first 500 (`PLUGIN_MAX_LIST_ITEMS`) in a single call are
silently dropped.

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

#### Live stream URLs

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

### EQ / sound profiles

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

### Playback control

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

## Complete examples

`plugins_examples/Audiobooks.lua` uses most of the API: `sd_root()`/
`list_dir()` to browse `Audiobooks/<book>/` folders on the SD card,
`show_list()` twice (book folder, then chapter list, chaining from the
first's `on_select` into the second), `play_list()` to start playback from
whichever chapter was tapped, and `show_toast()` for the "no chapters
found" / "no audiobooks found" empty states. Read it top to bottom as the
reference implementation for `register_list_item()`; the `README.md`'s own
Plugins section (section 5) has the install steps (copy to
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
Settings -> Sound Profile, switched with `plugin.eq_load_profile()` and
`plugin.show_list()`, following the exact same "`register_list_item` ->
`show_list` -> apply and persist to a state file" shape `Themes.lua` already
established for theme switching.

## Writing and testing your own plugin

1. No toolchain needed -- a plugin is a plain text `.lua` file. Any editor
   works; there's no separate build step for the script itself (the *app*
   binary that runs it does need Lua vendored in, see below, but that's a
   one-time thing already done in this repo).
2. Copy your `.lua` file to `<SD card>/.plugins/` (create the folder if it
   doesn't exist yet) and (re)launch the app -- plugins are only scanned at
   startup, there's no hot-reload.
3. Watch stderr for `[plugins] failed to load ...` (a load-time error --
   syntax error, or an API call failing during your script's top-level
   code) or `[plugins] books list item '...' on_open error: ...` /
   `[plugins] settings list item '...' on_open error: ...` /
   `[plugins] display list item '...' on_open error: ...` /
   `[plugins] tile '...' on_open error: ...` / `[plugins] show_list
   on_select error: ...` (a runtime error inside one of your callbacks).
   Per [TESTING.md](TESTING.md)'s launch method, this shows up directly in
   the foreground `adb shell` output.
4. Both `plugin.register_list_item()` and `plugin.register_stream_media_tile()`
   let every installed plugin have its own row/tile -- no "only the first
   one wins" limitation on either, so testing multiple plugins side by
   side works normally.

## Extending the `plugin.*` API itself

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
