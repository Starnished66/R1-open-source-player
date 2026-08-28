# Plugin Ideas — Draft

Ideas for new `plugins_examples/` scripts, scoped to what `plugin.*` already
exposes today (verified against `src/plugins/plugin_manager.c` and
`PLUGINS.md`, api_min 1-3). Nothing here requires a new native function.
Excludes anything the 13 existing examples already cover (Audiobooks,
NetRadio, Themes, HomeThemes, SoundProfiles, PlaybackExtras, PlayThrough,
ExtendedSleepTimer, LastFmScrobbler, AsyncHttp, PluginApiInfo, NestedLists,
Qobuz).

## Known gap found while reviewing the interface

`HomeThemes.lua` (already committed) calls `plugin.set_home_layout()`, which
does not exist as a registered Lua function — `set_home_layout` only appears
in comments in `src/ui/screen_builders.c`/`.h`, never in `plugin_manager.c`'s
`plugin_funcs[]`. The plugin will error at load (`attempt to call a nil
value`) on the current build. Worth a separate fix; not touched here since it
wasn't part of this request.

## Fields the current API does NOT expose (constrains what's buildable)

- No genre tag, "date added," favorite flag, or play count on the song table
  (`id`, `path`, `title`, `artist`, `album`, `album_artist` only) — rules out
  a genre browser or a native-favorites-aware smart playlist.
- No inbound network listener/socket — rules out any local web UI, casting,
  or remote-control receiver. Only outbound `http_request`/`http_get`/
  `http_post`/`download_file_async` exist.
- No battery/power, Wi-Fi, Bluetooth, or headphone-state read API (Phase 7 of
  `PLUGIN_INTERFACE_EXPANSION_DRAFT.md`, not implemented) — rules out any
  power-aware behavior.
- `plugin.storage`/`plugin.secrets` are namespaced per plugin `id` with no
  cross-plugin read — rules out one plugin backing up another's settings.
- `json_decode`/`json_encode` only — no XML/RSS parser, so podcast-feed
  ideas need hand-rolled string parsing, not a real feed parser.

## Proposed plugins

### 1. Quick Search & Play (`system` or `settings` row)
`show_text_input()` for a query, `plugin.library_search(query, 25)`,
`show_list()` of hits, `play_file(song.path)` on tap. The simplest possible
new plugin — no example currently pairs the T9 keyboard with library search.

### 2. My Top Tracks (local play-count tracker)
Works around the missing native play-count field: subscribe to
`"track_started"`, increment `plugin.storage.set("count:" .. path, n)` on
each fire (using `plugin.storage.list("count:")` to enumerate), and expose a
"Top Tracks" row that sorts by stored count and plays the result with
`play_list()`. Demonstrates `storage.*` + `on()` + `library_get_song`/
`library_search` together, and delivers something the native app doesn't
have (Most Played is native-library-only).

### 3. Playlist Roulette
`playlist_list()` + `playlist_read()` to pick a random existing `.m3u`,
`play_list()` it, `show_toast()` the chosen name. A "surprise me" row for
Settings → Playback. Distinct from `NetRadio.lua` (local playlists, not
internet streams) and from `PlayThrough.lua` (which only continues
naturally, never picks).

### 4. Shuffle by Album
True album-shuffle rather than track-shuffle: `library_get_albums()`,
Fisher-Yates shuffle in Lua, then `get_album_tracks(artist, album)` per
album concatenated into one big path list for `play_list()`. Gives whole
albums in random order with tracks in-order inside each album — a mode nothing
in the native player or existing examples offers.

### 5. Generic Now-Playing Webhook
Same shape as `LastFmScrobbler.lua` but provider-agnostic: on `"track_started"`,
`http_request()` a POST of `{title, artist, album}` (via `json_encode()`) to a
user-supplied URL (`show_text_input()` once, then `plugin.secrets.set()`).
Works unmodified for Discord/Slack/ntfy/Home Assistant/n8n webhooks. A
genuinely reusable building block the Last.fm-specific plugin doesn't
generalize.

### 6. ListenBrainz Scrobbler
Same event/timer pattern as `LastFmScrobbler.lua`, different backend:
ListenBrainz's API takes a user token (no MD5 signing, no session exchange)
and a JSON body, so it's `show_text_input()` for the token +
`plugin.secrets.set()`, `json_encode()` the `submit_listens` payload, POST via
`http_request()`. A second reference implementation of the scrobbler pattern
against a JSON-native API instead of Last.fm's signed form POSTs.

### 7. Synced Lyrics Toast
`http_request()` to a plain-JSON lyrics API (e.g. one that returns an LRC
string in a JSON field — verify the exact response shape before coding
against it, don't guess a schema), `json_decode()` the response, parse
`[mm:ss.xx]` timestamps with Lua patterns, then a `set_interval(1, ...)`
comparing `get_position()` against the parsed cue list and `show_toast()`-ing
the current line. No overlay UI exists to render synced lyrics properly, so
this is a toast-based approximation — worth stating that limitation up front
in the plugin's own header comment.

### 8. Mode Switcher (composite presets)
A `show_settings_list()` menu of named presets ("Commute", "Focus", "Sleep"),
each one row's `on_select` firing a small bundle of already-independent
calls: `eq_load_profile(path)`, `set_volume(n)`, optionally
`plugin.play_list()` a specific playlist. Each piece already exists in
`SoundProfiles.lua`/native volume control; the new value is bundling them
behind one tap instead of three. Store the preset list itself in
`plugin.storage` so users can rename/edit without touching the script.

### 9. Auto Day/Night Theme
Extends the manual switcher in `Themes.lua` into a scheduled one:
`set_interval(60, ...)` checks `os.date("%H")` (still available — sandboxing
only removed `os.execute`/`getenv`/`exit`/`tmpname`) and calls
`set_background_color()`/`set_text_color()` to flip palettes at configured
hours, persisting the last-applied theme name in `plugin.storage` so it's
correct immediately on the next boot before the first timer tick.

### 10. Podcast Feed Downloader
`http_request()` the feed URL, hand-parse `<enclosure url="...">` and
`<title>` with Lua string patterns (no real XML parser is available —
`json_decode` won't help here), `mkdir()` a per-podcast folder under
`sd_root()`, `download_file_async()` each new episode, `playlist_create()`/
`playlist_add()` to build a listening queue automatically. The one idea here
needing a real parsing pass — call out in the plugin header that it depends
on typical (non-namespaced-XML) RSS output and may need adjusting per feed.

### 11. Playlist Merge/Dedupe Tool
`show_list()` twice to pick two playlists from `playlist_list()`,
`playlist_read()` both, dedupe in Lua (`table` keyed by path), then
`playlist_create()` + repeated `playlist_add()` into a new merged list.
Pure CRUD over the existing `playlist_*` functions with no native
equivalent — the app doesn't offer playlist merging today.

## Suggested build order

1. Quick Search & Play — smallest possible plugin, good template/starting
   point for anyone new to the API (fewer moving parts than `Audiobooks.lua`).
2. My Top Tracks — highest user-facing value; nothing else fills the
   missing-play-count gap.
3. Generic Now-Playing Webhook — reusable building block other plugin authors
   can copy for their own integration instead of writing a new HTTP client
   each time.
4. Shuffle by Album / Playlist Roulette / Mode Switcher — smaller, independent
   quality-of-life additions, any order.
5. Synced Lyrics Toast / ListenBrainz Scrobbler / Auto Day-Night Theme /
   Podcast Downloader / Playlist Merge — round out the set; podcast downloader
   is the only one with real external-format risk (RSS shape varies by feed),
   so it's last.
