# What's New

Changes from August 31 to September 14, 2026, for the next weekly beta.

This update brings a redesigned player and menus, lyrics that open directly
from the album cover, smoother navigation, and improvements to audio playback
and library browsing. It also adds headphone remote controls and clearer
feedback when a Subsonic connection or library request fails.

## A refreshed player and interface

- **A new Now Playing layout.** Album art sits in a rounded card over a
  blurred background drawn from the cover. The title and smaller, gray
  artist/album line are left-aligned, with a short pause before long text
  starts scrolling. The cover also shows your position in the queue.
- **Lyrics stay part of the player.** Tap the cover to shrink it into the
  upper-left corner, move the track information beside it, and show lyrics
  below. Tap again to bring back the full cover and playback controls.
  Fixed a drawer-gesture conflict that hid the return animation. Lyrics can
  be disabled in Music Controls.
- **Clearer playback details.** The quality badge expands to show the
  format, bit depth, and sample rate without scrolling. Its outer ring was
  removed after it caused visible flickering. The three-dot menu now sits
  beside the next-track button, and the favorite button is easier to tap.
- **A more consistent look.** Home, Settings, Wireless, and other main menus
  have refreshed icons and backgrounds. Net Radio and Audiobooks follow
  the new menu style too. The status bar uses Lucide icons, including the
  battery, with a centered clock and sizing independent of text-size settings.
- **More display options.** Home supports custom background images and
  individual tile colors through themes and plugins. You can also choose
  how long the screen waits before dimming. Custom fonts have improved
  performance and optional emoji support.

## Playback and everyday controls

- Added native 24-bit USB audio output and support for 24-bit crossfades.
  Improved high-sample-rate playback and fixed USB DAC compatibility with macOS.
- Improved Bluetooth DAC buffering for longer listening sessions and fixed
  Bluetooth/USB output write-timeout handling.
- Improved automatic switching between the R3 Pro II's 3.5mm and 4.4mm
  outputs. Fixed playback-position restoration after a restart, seek-bar
  positioning, and EQ preset saving.
- Added an **In-line Remote** setting for supported wired-headphone
  controls. It is enabled by default.
- Reworked playlist and queue handling. Queue state is now stored on the
  SD card, and the active queue clears when that card is removed.

## Faster browsing and better artwork support

- More artwork stays cached, reducing repeated thumbnail loading as you
  browse. Player-sized cover caching and lighter metadata reads also
  reduce work when opening tracks and lyrics.
- Added progressive JPEG support and improved handling of large JPEGs,
  16-bit PNGs, BMPs, and JPEGs with unusual color-sampling layouts. Artwork
  detection and decoding are more reliable across different files.
- Album lists now show **Album Artist** information.
- In Files, the back button and back swipe move up one folder before
  leaving the browser. Quick swipes are less likely to bounce back, and a
  crash during player-screen swipe navigation was fixed.

## Subsonic and plugins

- **Subsonic requests now have visible failure feedback.** Connection and
  library-loading screens have a 30-second UI timeout. Errors distinguish
  network failures, HTTP responses, and invalid server data instead of
  silently returning to the previous screen.
- **Failed artist loading can be retried.** A successful login no longer
  requires a populated artist list. If the initial artist request fails,
  opening Artists retries it rather than leaving a permanently empty cache.
- Added the missing Subsonic download-button icon and improved handling of server
  addresses with trailing slashes or `/rest`.
- Last.fm now keeps a persistent queue of scrobbles for upload when the
  service is reachable again, including after a restart.
- Plugins can be enabled, disabled, and reloaded from Settings without a
  firmware reinstall. Fixed unexpected restarts during plugin management
  and integrated plugin options into Music Settings categories.
- Added the **MSEB** plugin, with MSEB-inspired sound shaping using the
  player's own parametric EQ, and improved its navigation.
  Lock Screen gained a clock over artwork and smoother swipe-up behavior;
  Play Through also received fixes.

## Device support, charging, and updates

- Added board-specific builds and display layouts for **R3 Pro II** and
  **R3 II 2025**, alongside the R1 build. Hardware support and behavior
  remain specific to each model.
- On R3 Pro II, Charge Limiter and Safe Charging now apply voltage and
  current limits to the dedicated charger, with restoration when disabled.
  Charging status also reads from the correct hardware source.
- Improved installation of player updates from the SD card. The bootloader
  compares build dates so an older installed player does not override a
  newer player included in a firmware update.
- Fixed the Firmware Update action powering off instead of rebooting into
  recovery, and corrected updater status text.

## Behind the scenes

Upgraded LVGL, the library that powers the interface, from **9.1 to 9.5**.

Shared UI builders and substantial cleanup across the player reduce duplicated
code and make layouts easier to maintain. Other work includes safer handling
of overlapping DLNA requests, reduced memory use during plugin reloads,
and improved crash diagnostics. The build configuration
again selects mbedTLS 3.6.2 after connectivity regressions with 3.6.7.

Automated daily builds continue; weekly beta builds are scheduled for Mondays
at 1:00 p.m. Costa Rica time.

## Before updating

Emoji support needs the new font supplied in the full firmware package;
replacing only the player executable will not install it. Updated plugin
features also require the corresponding plugin files.
