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

**Only the first plugin to call `plugin.register_tile()` is currently
reachable.** There's no picker UI yet for choosing among several -- if two
plugin files each register a tile, the second's registration succeeds (it's
still tracked internally) but nothing in the UI currently invokes it.

The one reachable slot is **Books -> "Audio Books"** (a pre-existing native
row, `gui.c`'s `audio_books_row_cb()`): tapping it calls
`plugin_manager_tile_clicked(0)`, which runs tile 0's `on_open` function. If
no plugin has registered a tile, that row falls back to its original
"Audio Books coming soon" toast, so an app build with no plugins installed
behaves exactly as before.

This used to be a dedicated Home-screen icon per plugin instead. That was
dropped: `build_icon_grid_screen()`'s grid (real-device testing confirmed)
**cannot be scrolled**, and Home already has all 6 of its built-in tiles
filling the screen -- any 7th tile would render but be permanently
unreachable. Routing through an existing, already-visible native row sidesteps
that entirely. `plugin.register_tile()`'s `label`/`icon` arguments are
still accepted and stored (for a future picker UI once more than one
plugin needs to be reachable) but nothing reads them today.

## The `plugin` API

Every function below is a field on the global `plugin` table, available
from the moment your script starts running (injected before
`luaL_dofile()`). All of it is implemented in `src/plugins/plugin_manager.c`.

### `plugin.register_tile(label, on_open [, icon])`

Registers your plugin's entry point.

- `label` (string): stored, not currently shown anywhere (see above).
- `on_open` (function): called with zero arguments when your plugin is
  opened. This is where you'd call `plugin.show_list()` to show your first
  screen.
- `icon` (string, optional): a theme2-relative asset path (e.g.
  `"launcher/dac.png"`) for a future picker UI. Its "_s" (pressed) variant
  is derived automatically by inserting `_s` before the file extension --
  that variant must also exist as a real asset, same as every built-in
  tile's own icon pair. Omit it (or pass `nil`) to default to the Books
  icon.

Only the first call across all loaded plugins currently has any visible
effect (see above). A script can still call this more than once for
multiple internal "screens" of its own conceptual UI if that's useful to
its own structure, but only tile index 0 is ever dispatched from the
native UI.

Errors if more than 16 tiles (`PLUGIN_MAX_TILES`, across every loaded
plugin combined) are registered.

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
tapping any song elsewhere in the app.

### `plugin.play_list(paths [, start_index])`

Starts playback of `paths` (array table of absolute file path strings) as
a fresh playlist, beginning at the **1-based** `start_index` (default `1`
if omitted). The rest of the app's normal playback machinery (auto-advance
to the next track, Prev/Next, the "Track X of Y" label, gapless preload)
all apply exactly as if this were any other playlist. Paths beyond the
first 500 are silently dropped; `start_index` is clamped into range if
out-of-bounds rather than erroring.

### `plugin.show_toast(message)`

Shows the same transient toast used elsewhere in the app (e.g. "Added to
queue"). Useful for "nothing found" / error feedback -- see
`Audiobooks.lua`'s use of this when a book folder has no playable chapter
files in it.

## A complete example

`plugins_examples/Audiobooks.lua` uses every function above:
`sd_root()`/`list_dir()` to browse `Audiobooks/<book>/` folders on the SD
card, `show_list()` twice (book folder, then chapter list, chaining from
the first's `on_select` into the second), `play_list()` to start playback
from whichever chapter was tapped, and `show_toast()` for the "no chapters
found" / "no audiobooks found" empty states. Read it top to bottom as the
reference implementation; the `README.md`'s own Plugins section (section
5) has the install steps (copy to `.plugins/Audiobooks.lua`, create an
`Audiobooks/<book>/` folder structure).

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
   code) or `[plugins] tile '...' on_open error: ...` / `[plugins]
   show_list on_select error: ...` (a runtime error inside one of your
   callbacks). Per [TESTING.md](TESTING.md)'s launch method, this shows up
   directly in the foreground `adb shell` output.
4. There is currently no way to see two plugins' tiles side by side in the
   UI -- test with exactly one `.lua` file in `.plugins/` at a time until a
   picker exists.

## Extending the `plugin.*` API itself

If you're modifying `plugin_manager.c` to add a new function (not writing
a plugin script, but adding to what plugins *can* call): follow the
existing `l_plugin_*` functions as the pattern -- `luaL_check*`/`luaL_opt*`
to pull typed arguments off the Lua stack, do the work (usually by calling
into a `gui_plugin_*` bridge function declared in `gui.h` and implemented
in `gui.c`, since `plugin_manager.c` itself has no LVGL/screen code of its
own), push however many return values, `return <that count>;`, then add
the new `{ "name", l_plugin_name }` entry to the `plugin_funcs[]` table.
`gui.c`'s bridge functions are the *only* thing plugin_manager.c is allowed
to call into the rest of the app through -- keeps the Lua-facing surface
and the app's own internals from becoming entangled.
