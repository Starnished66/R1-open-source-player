# What's New

This file is the curated changelog for the next weekly beta. Update it in the
same pull request or commit as a user-visible player change; every Monday
release embeds its current contents and links back to the exact revision used.

## Last 2 Weeks

### User-Perceptible Features

**Now Playing & Lyrics**

- New Now Playing layout: 350x350 cover art card over a full-screen frosted
  glass blur background, format/bit-depth quality pill, native progress rail,
  reachable heart/favorite and 3-dot menu.
- Frosted glass background effect standardized and reused across the Lyrics
  screen and other theme surfaces.
- Lyrics screen reworked (layout, backdrop, settings listing fixed).
- Setting to enable/disable showing the Lyrics screen when tapping the cover
  image (on by default).
- Long artist names no longer overlap the format quality badge.

**Home & Theming**

- Home screen and top status bar redesigned, with a new icon set and digit
  typography for the clock/volume/battery readouts.
- Home background images and per-tile theming support.
- UI scaling fixes across Home and list screens (icon sizing, list-mode
  layout parity with tile mode).

**Playback & Audio**

- Native 24-bit USB DAC output, including seamless 24-bit crossfade.
- Improved Bluetooth DAC buffering for long listening sessions (better
  clock-drift/jitter tolerance).
- Fixed BT/USB output pipe write timeouts.
- Playback resumes correctly on reboot; seek bar position fixed.
- PEQ preset saving fixed.
- Proper output-port switching between DAC/Bluetooth/etc.
- Playlists reworked.

**Cover Art & Metadata**

- Progressive JPEG cover support.
- JPEG covers up to 4K decode correctly when the scaled result stays under
  the 1200px cap; PNG/BMP covers hardened, including 16-bit PNGs.
- JPEG with vertical-only chroma subsampling now decodes correctly.
- General cover art and metadata pipeline hardening (more art detected
  correctly, fewer decode failures).
- Album art thumbnail cache raised from 32 to 100 concurrently loaded
  covers, and up to 250 covers can now be pre-cached on cold boot, smoothing
  out library scrolling.
- Album Artist now shown in the Albums list.

**Library & Navigation**

- "Files" navigation changed.
- Smoother, more responsive swipe gestures and screen-transition animations;
  a quick swipe now completes the transition instead of bouncing back.
- Favorite icon hitbox enlarged (previously hard to tap).
- Queue state now saved to the SD card and cleared automatically when the
  card is removed.

**Network & Plugins**

- Subsonic and Last.fm connection attempts no longer hang indefinitely on
  a bad or unresponsive network -- they now fail cleanly with a real error
  after a bounded timeout instead of leaving "Connecting..." on screen
  forever.
- Subsonic track download button added.
- Last.fm scrobbler plugin upgraded.
- Lock Screen plugin rewritten in C with enhanced features (including a
  clock overlay on lock-screen album art/images) and swipe-up animation
  fixes.
- Runtime plugin management: enable/disable and reload plugins from
  Settings without reflashing; plugins integrated into Music Settings
  categories.
- Fixed a device reboot triggered by enabling/disabling plugins from the
  Plugin Manager.
- Plugin-configurable hardware gain curves.
- MSEB (HiBy's sound-shaping feature) reverse-engineered and added as a
  plugin.
- Emoji support added to custom fonts (requires a full firmware flash next
  release, since the emoji font ships in the package).

**Battery & Charging**

- Safer charging: a real current/voltage cap implemented for R3 Pro II
  (500mA current cap, voltage cap via the MP2731 charger, since the stock
  AXP2101 cap had no effect on that board), with the pre-cap limits
  correctly restored when Safe Charging is turned off.
- Correct battery charging status shown on R3 Pro II (previously stuck
  reporting "Discharging" even while charging).

**Other Fixes**

- Fixed USB DAC output on macOS.
- Fixed MSEB navigation.
- Fixed a crash (use-after-free) during player-screen swipe navigation.
- Fixed higher sample-rate playback reliability.

### Architectural / Internal Improvements

- Decluttered and standardized every major module (library, audio, core,
  hardware, network, plugins, bootloader, and UI screen builders): removed
  dead code, unified duplicated logic behind shared helpers, normalized
  deprecated LVGL API usage, all independently cross-checked by a second
  model (Grok and/or Codex) before and after each pass, verified against
  host-buildable regression suites and `make target` throughout. Two real
  correctness bugs were caught and fixed along the way during this
  auditing (uninitialized track/disc numbers, and firmware
  recovery/factory-reset not actually rebooting -- both listed above).
- Subsonic/HTTP client hardened: connection, read, and TLS handshake now
  each have real, enforced timeouts (including a previously-unbounded DNS
  resolution step now run with a bounded wait), and requests can be
  cancelled cleanly (e.g. when Wi-Fi is turned off mid-request) instead of
  leaking a blocked background thread.
- mbedTLS updated to v3.6.7 and then reverted back to v3.6.2 after the
  newer version caused real-device connectivity regressions.
- Fixed a DLNA concurrency bug where rapid `play@` commands could clobber
  each other's downloads or publish a stale track (last-completed instead
  of last-requested winning); fixed via a generation counter that fences
  stale in-flight downloads.
- PNG streaming decoder (Phase 2): non-interlaced 8-bit RGB/RGBA PNGs now
  decode via manual chunk parsing and a vendored `tinfl` inflate path
  instead of falling back to "oversized" for large covers.
- Multi-board build support: compile-time BOARD switch for R3 Pro II
  scaffolding (isolated object dirs/binaries, board-driven screen/player
  geometry), independent of the R1's own default 480x800 build.
- Bounded memory usage across plugin UI reloads and large plugin list/play
  requests (Lua-GC-managed sizing instead of large fixed static buffers).
- Added crash diagnostics (signal handler, backtrace logging) and
  developer database logging for field debugging.
- CI/release pipeline: automated weekly (now Monday 1:00pm Costa Rica time)
  and daily builds, bundled MIPS toolchain for reproducible automated
  builds, versioned releases aligned to automated builds, test plugins
  packaged automatically.
