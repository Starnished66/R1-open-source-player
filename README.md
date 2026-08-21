# Open Source Player for HiBy OS

An open-source music player replacement for HiBy OS, built from scratch in C with [LVGL](https://lvgl.io/).

The goal is simple: replace the closed-source stock `hiby_player` with a modern, community-driven player that makes better use of the hardware while retaining the parts of HiBy OS that already work well.

The project currently targets the **HiBy R1** and is being developed with support for additional HiBy devices in mind. The R1 has been tested extensively on real hardware, not just in the host simulator.

The player reuses the stock firmware's UI assets, fonts, and existing system services where practical, including Bluetooth, Wi-Fi, and DLNA functionality. Everything else is implemented from scratch.

> **Status:** Fully usable on real R1 hardware, with a growing set of features beyond the stock player.

---

# Support the Project

This Open Source Player is developed and tested on real hardware. If you would like to help fund continued development, device testing, and future hardware support (the HiBy R3 Pro II, for example), donation links can be found here:

- **PayPal:** [Donate via PayPal](https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=josegarita%40protonmail.com&currency_code=USD)

Contributing code, testing builds, documenting hardware behavior, and reporting reproducible bugs are equally valuable ways to support the project.

---

## Why?

HiBy's stock player is closed source and tightly integrated with the firmware. That makes fixing bugs, adding features, or changing how the device behaves difficult.

This project aims to change that.

The player provides:

- A fully open-source playback engine
- A more capable music library
- Better playlist handling
- Network streaming and remote control
- Improved device integration
- Modern touch interactions
- Features that are missing or unnecessarily restricted in the stock software
- A foundation for future community development

The intention isn't to recreate every quirk of the stock application. Where the stock player gets in the way, this project aims to improve it.

---

# Features

## 🎵 Playback

- FLAC, MP3, WAV, AIFF/AIFF-C
- DSD64 / DSD128 (`.dsf` / `.dff`)
- AAC — raw ADTS and `.m4a`
- ALAC
- APE / Monkey's Audio
- OPUS
- Gapless playback **enabled by default**
- Crossfade
- ReplayGain track gain
- 10-band parametric EQ (PEQ)
- Per-band filter type, gain, and Q
- Pre-amp control
- Save, overwrite, and reuse PEQ presets stored on the SD card
- Resume the last track either playing or prepared in a paused state, including its original album context
- Physical hardware controls for volume, play/pause, next/previous
- Automatic track advancement
- Sequential, Repeat All, Repeat One, and Shuffle playback modes
- Proper playlist-end handling
- Bluetooth playback support
- Bluetooth metadata streaming
- Bluetooth DAC finally works
- Album art downscaling, supporting up to 1200x1200 resolutions
- Embedded and external album artwork (`cover.*` and `folder.*`)
- Queue / "Up Next" — add, inspect, remove, and clear queued songs independently of shuffle or repeat mode

## 📚 Music Library

- File browser
- All Songs
- Artists
- Albums
- Album Artists
- Playlists — Favorites, Most Played, and user-created M3U/M3U8, all in one place
- Real tag scanning
- Embedded album artwork
- FLAC metadata
- MP3 ID3v1 / ID3v2
- WAV RIFF metadata
- M4A metadata
- M3U / M3U8 playlists
- Full screen synced Lyrics support. 
- Dynamic playlist loading directly from the SD card
- Incremental music database updates instead of rescanning the entire library
- Paged/virtualized song lists for very large albums and libraries
- SD card hotplug support
- Built-in plain-text (`.txt`) book reader with Favorites, scoped to `Books/` on the SD card
- Audiobook browsing and playback through the included Audiobooks plugin

## 🌐 Network & Streaming

- **Subsonic-compatible streaming**
  - Subsonic
  - Navidrome
  - Airsonic
  - Other compatible servers
  - HTTPS streaming with TLS
- **Subsonic music downloading**
- DLNA / UPnP-AV renderer
- AirPlay support using the existing stock protocols where possible
- Remote control over LAN
- Built-in lightweight web server
- Responsive browser-based remote-control interface
- Browse Artists, Albums, Favorites, Most Played, and SD-card playlists remotely
- Add songs to the queue and manage the current queue from the web interface
- Add songs to existing playlists from the web interface
- Wi-Fi music import with the stock limitations removed
- Import support for all supported file types

The LAN remote-control interface provides a simple way to control the player from a phone, tablet, or PC without requiring a dedicated application.

## 🔌 Device & Hardware Integration

- USB DAC support
- Smoothed real battery percentage that filters transient fuel-gauge jumps
- Real Wi-Fi signal strength
- 85% charge limiter for improved battery longevity
- **Safe Charging (500mA)** mode with immediate, persistent PMIC-level current limiting
- Configurable idle shutdown or suspend behavior
- Car mode
- Improved Bluetooth connection reliability
- Bluetooth A2DP status in the top bar
- Automatic playback stop when the audio output disconnects
- Physical hardware button support
- Time Zone picker with immediate application and persistent settings
- Startup volume
- Resume-last-track modes with preserved library context
- Storage, USB DAC, and ADB modes through a recoverable USB mode selector
- Configurable charge-status LEDs

## 🎨 Interface

- Stock firmware UI assets and fonts
- Interactive finger-following swipe transitions
- Swipe up to return home
- Pull-down quick controls
- Sleep timer
- Crossfade quick toggle
- Configurable screen timeout presets from 15 seconds to 30 minutes
- Optional idle screen dimming to approximately 5% brightness
- Configurable font size and battery-percentage display
- Font-aware row sizing for Medium and BlindMF accessibility text sizes
- Delayed, speed-normalized marquee scrolling for long row titles instead of wrapping or overlapping adjacent rows
- Non-Latin text rendering:
  - Cyrillic
  - Japanese kana/kanji
  - Korean Hangul
  - Thai
- App-wide accent color theming
- Focused Books section for plain-text reading and plugin-provided audiobook tools

## 🧩 Plugins

- Third-party Lua plugins — no rebuild or reflash needed
- Drop a `.lua` file in `.plugins/` on the SD card, it's picked up automatically
- Versioned `plugin.*` API with capability discovery
- Extension rows for Books and the Settings, Display, Playback, Power, and System screens
- Custom row sizing, icons, text sizing, list screens, settings screens, and text input
- Playback state/control/events, library access, synchronous and asynchronous HTTP, MD5, theming, and PEQ access
- Audiobooks example with per-book/chapter progress, resume, bookmarks, and Continue Listening
- File-configured Net Radio example that loads station names and URLs from `Radio.txt`
- Play Through example for continuing through folders or albums
- Extended Sleep Timer example with durations up to three hours
- Last.fm scrobbling, themes, sound profiles, playback tools, asynchronous HTTP, and other API examples

---

# Planned Features

The following features are planned or currently incomplete:

- Native audiobook support (currently available via the example Audiobooks plugin — see [PLUGINS.md](PLUGINS.md))
- Background image/color customization
- Full theme support (Base is already done with a plugin interface)
- Additional community-requested features

This list will evolve as development continues.

---

# Device Support

## HiBy R1

**Status: Supported and actively tested.**

The R1 is the primary development and testing platform for this project.

The player has been tested on real hardware using the device's framebuffer, touchscreen, audio hardware, physical buttons, Bluetooth, Wi-Fi, and other system interfaces.

## HiBy R3 Pro II

**Status: Currently untested.**

The R3 Pro II is not currently considered supported.

Additional work is required before it can be properly supported:

- Add support for the **balanced output port**
- Add support for the device's **additional hardware buttons**
- Test and adjust **UI scaling** for the R3 Pro II's display resolution
- Perform full real-device testing
- Me actually getting one of these devices on hand to do a full testing.

Support for other HiBy devices may also be possible, but should not be assumed without hardware testing.

---

# Terminology

- **Host device / host** — the machine you're developing on, such as your laptop or PC.
- **Target device / target** — the HiBy device you're building for, such as the R1.

---

# Development

## 1. Local Development — Arch Linux Host Simulator

The project includes an SDL2-based host simulator, allowing the GUI and much of the player logic to be developed without a physical device.

### Requirements

- `sdl2`
- `make`
- `gcc`
- `pkg-config`
- `git`

### Build

```bash
make
```

This automatically clones LVGL if necessary and builds the project for the host architecture.

### Run

```bash
./open_hiby_player_host
```

This opens the player in an SDL2 window.

Mouse click-and-drag can be used to simulate touch interaction and scrolling.

---

# 2. Cross-Compiling for HiBy OS

The HiBy devices use a MIPS target environment with an old glibc version (2.22). Rather than depending on a matching legacy sysroot, the target build uses a **musl-based static toolchain**.

The resulting executable is statically linked and therefore depends on the stable Linux kernel syscall ABI rather than the target device's libc.

This makes it practical to build for the older and somewhat unusual HiBy OS environment.

## Install the Target Toolchain

On Arch Linux:

```bash
yay -S mipsel-linux-musl-cross
```

The package builds a complete GCC + musl toolchain from source.

The initial build may take 15–30 minutes or more depending on the host system.

Support for other distributions is not documented yet. Contributions are welcome.

## Build for the Target

```bash
make target
```

This produces:

```text
open_hiby_player_target
```

The resulting binary is statically linked for little-endian MIPS (`mipsel`).

---

# Testing Without Hardware

The target binary can be sanity-checked using QEMU user-mode emulation.

On Arch Linux:

```bash
sudo pacman -S qemu-user
```

Then:

```bash
qemu-mipsel ./open_hiby_player_target
```

The program should print its startup banner and then fail when attempting to access devices such as:

```text
/dev/fb0
/dev/input/eventN
```

This is expected when running under normal host Linux. Those device nodes do not exist in the host environment.

The test is primarily intended to verify that the binary starts and executes correctly under the target architecture.

---

# Real Device Testing

For instructions on installing, launching, and testing the player on real HiBy hardware, see:

**[TESTING.md](TESTING.md)**

The testing guide contains the device-specific procedures and important warnings for working with the stock player, ADB, framebuffer, and other hardware interfaces.

---

# Modifying Unpacked Firmware

TODO

---

# Technical Notes & Real-Device Findings

Detailed implementation notes, reverse-engineering information, root causes, verification methods, and real-device bugs discovered during development are maintained separately in:

**[TECHNICAL_NOTES.md](TECHNICAL_NOTES.md)**

If you're simply looking to use the player, you can safely skip this section.

If you're modifying the code, debugging hardware behavior, or working on HiBy OS itself, the technical notes are the place to start.

---

# 🧩 Plugins

Third-party functionality can be added without recompiling the app, as plain Lua scripts — no toolchain, no C, no rebuild/reflash cycle.

Drop a `.lua` file into `.plugins/` on the SD card and it's picked up automatically at startup. The versioned `plugin.*` API exposes native extension rows and list screens, configurable row layout, text input, themes, SD-card and library access, playback control/events, PEQ, cryptographic helpers, and synchronous or asynchronous HTTP.

An example **Audiobooks** plugin is included (`plugins_examples/Audiobooks.lua`) — a book-folder browser → chapter-list → playback plugin with per-book progress, automatic resume, bookmarks, and Continue Listening, reachable from **Books → Audiobooks**. Other examples include Last.fm scrobbling, file-configured network radio, Play Through folders/albums, a three-hour sleep timer, themes, sound profiles, asynchronous HTTP, playback extensions, and broader plugin-interface demonstrations.

For the full `plugin.*` API reference and instructions on writing and testing your own plugin, see:

**[PLUGINS.md](PLUGINS.md)**


# Acknowledgments

This project builds on important prior work from the HiBy modding community.

### [hiby-modding/hiby_os_crack](https://github.com/hiby-modding/hiby_os_crack)

The parent toolkit this project was originally developed alongside.

Its `.upt` firmware unpacking/repacking tools, QEMU setup, device layouts, and general HiBy OS reverse-engineering work made it possible to build and flash modified firmware images in the first place.

### [bidhata/Hiby-R1-Mod](https://github.com/bidhata/Hiby-R1-Mod)

An unpacked/mirrored copy of the R1 stock firmware was used as the local development source for the host simulator's `theme2` UI asset mirror and for identifying stock font files used by the player at runtime.

Huge thanks to everyone who has contributed to the existing HiBy reverse-engineering ecosystem.

---

# License

This project is licensed under the **GNU General Public License v2.0**. See [LICENSE](LICENSE).

The project statically links [FAAD2](https://github.com/knik0/faad2) for AAC decoding, which is GPLv2-licensed. As a result, the resulting binary is distributed under GPLv2 as a whole rather than treating the AAC decoder as an isolated component.

Other major dependencies, including `dr_libs`, LVGL, and tinyalsa, use permissive licenses.

If GPLv2 is ever undesirable for a particular deployment, the practical solution would be to remove AAC support or replace FAAD2 with a decoder under a compatible permissive license—not to attempt to isolate the existing FAAD2 implementation from the resulting binary.

---

# Contributing

Contributions, bug reports, hardware testing, reverse-engineering findings, and feature requests are welcome.

If you have a HiBy device that isn't currently supported, testing and reporting hardware-specific behavior can be particularly useful.

Feature requests are also welcome—especially those that improve the player without adding unnecessary complexity.

---

# Project Direction

The long-term goal is not simply to make a replacement for `hiby_player`.

It's to build a **community-maintained music player for HiBy hardware** that is easier to modify, easier to debug, and capable of features that the stock software doesn't provide.

The project is still actively evolving. Expect rough edges, experimental features, and hardware-specific quirks—but also expect the player to keep getting better.
