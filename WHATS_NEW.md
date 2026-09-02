# What's New

This file is the curated changelog for the next weekly beta. Update it in the
same pull request or commit as a user-visible player change; every Sunday
release embeds its current contents and links back to the exact revision used.

## Beta 2

- Added runtime plugin management and consolidated theme support, including
  custom Home layouts and Home-only background images.
- UI and theme reloads now preserve active Bluetooth, Wi-Fi, remote-control,
  AirPlay, DLNA, and Wi-Fi Import services while releasing stale UI resources.
- Reduced memory pressure during plugin reloads, artwork/database scans, long
  track seeking, and Stock-player handoff.
- Improved boot selection: Stock always exposes the menu, newer player builds
  are preferred, and equal builds prefer the internal player.
- Added manual clock controls, automatic synchronization, 12/24-hour display,
  and timezone selection under Settings > System > Clock.
- Added collection action menus for artists, album artists, and albums.
- Improved long MP3/AAC/Opus seeking and rapid skip/seek recovery.
- Made volume and brightness dragging responsive while coalescing hardware
  updates in the background.
- Added adaptive SD-card readiness handling and clearer boot diagnostics.

## Everything in Beta 2

This is the first tracked release, so here's the complete feature set, not
just the delta above.

### Playback

- FLAC, MP3, WAV, AIFF/AIFF-C, DSD64/128, AAC, ALAC, APE, OPUS
- Gapless by default, crossfade, ReplayGain track gain
- 10-band PEQ with saved presets
- Resume last track (playing or paused), original album context kept
- Hardware volume/play-pause/next/previous controls
- Sequential, Repeat All, Repeat One, Shuffle
- Bluetooth playback, metadata streaming, working DAC output
- Album art up to 1200x1200, embedded and external (`cover.*`/`folder.*`)
- Queue / "Up Next" independent of shuffle or repeat

### Music Library

- File browser, All Songs, Artists, Albums, Album Artists
- Playlists: Favorites, Most Played, user M3U/M3U8, all in one place
- Real tag scanning: FLAC, MP3 ID3v1/v2, WAV RIFF, M4A
- Full-screen synced lyrics
- Incremental database updates on a Rockbox-tagcache backend, no SQLite
- Paged/virtualized song lists for large libraries
- SD card hotplug
- Plain-text book reader with Favorites, scoped to `Books/`
- Audiobook browsing and playback via the Audiobooks plugin

### Network & Streaming

- Subsonic-compatible streaming (Subsonic, Navidrome, Airsonic, others) over HTTPS
- Optional Subsonic tile on Home, plus Subsonic music downloading
- DLNA/UPnP-AV renderer, AirPlay support
- LAN remote control with a browser-based web UI: browse, queue, playlists
- Wi-Fi music import, all supported file types, stock limits removed

### Device & Hardware

- USB DAC support
- Smoothed real battery percentage, real Wi-Fi signal strength
- 85% charge limiter, Safe Charging (500mA) mode
- Configurable idle shutdown/suspend, car mode
- More reliable Bluetooth, A2DP status in the top bar
- Auto-stop playback when audio output disconnects
- Physical hardware button support
- Time zone picker, startup volume
- Resume-last-track modes with preserved library context
- Storage/USB DAC/ADB mode selector
- Configurable charge-status LEDs

### Interface

- Stock UI assets and fonts, swipe transitions, pull-down quick controls
- Sleep timer, crossfade quick toggle
- Configurable screen timeout and idle dimming
- Configurable font size/battery display, marquee scrolling for long titles
- Non-Latin text: Cyrillic, Japanese, Korean, Thai
- App-wide accent theming, live-switchable `.theme` files
- Customizable Home: reorder/hide/add tiles, grid or list mode, plugin background image
- Independently themeable Music, Stream Media, and Wireless screens
- Manual or automatic clock, 12/24-hour
- Books section with plugin-provided audiobook tools

### Plugins

- Third-party Lua plugins, no rebuild or reflash
- Drop a `.lua` file in `.plugins/` on the SD card, picked up automatically
- Versioned API with capability discovery
- Extension rows/tiles for Books, Settings, Stream Media, and Home
- Live reload of icon, color, background image, and layout changes
- Enable/disable individual plugins from Settings without removing files
- Examples included: Audiobooks, Themes, MSEB, Home Background, Net Radio,
  Play Through, Extended Sleep Timer, Last.fm scrobbling, sound profiles,
  async HTTP
