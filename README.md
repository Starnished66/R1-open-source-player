# Compás Player

An open-source music player replacement for HiBy OS, built from scratch in C with [LVGL](https://lvgl.io/).

The goal is to replace the closed-source stock `hiby_player` with a community-driven player that makes better use of the hardware.

> **Status:** Fully usable on real R1 hardware. Board-specific builds also exist for the R3 Pro II and R3 II 2025; support is model-specific and should not be assumed for unlisted HiBy devices.

---

# Support the Project

This Open Source Player is developed and tested on real hardware. If you would like to help fund continued development, device testing, and future hardware support (the HiBy R3 Pro II, for example), donation links can be found here:

- **PayPal:** [Donate via PayPal](https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=josegarita%40protonmail.com&currency_code=USD)

Contributing code, testing builds, documenting hardware behavior, and reporting reproducible bugs are equally valuable ways to support the project.

---

# Device Support

Makefile `BOARD` values: `r1` (default), `r3proii`, `r3ii_2025`. R1 binaries keep the unsuffixed names; the others get a `_$(BOARD)` suffix so they cannot overwrite an R1 build.

## HiBy R1

**Status: Supported and actively tested.** Primary development platform. Tested on real hardware (framebuffer, touchscreen, audio, physical buttons, Bluetooth, Wi-Fi).

```bash
make target                 # -> open_hiby_player_target
make bootloader             # -> open_hiby_bootloader
```

## HiBy R3 Pro II

```bash
make target BOARD=r3proii   # -> open_hiby_player_target_r3proii
make bootloader BOARD=r3proii # -> open_hiby_bootloader_r3proii
```

The tree has a 480×720 layout, 4.4 mm balanced-output routing, extra charger-IC handling, and UI scaling. Do not treat an R3 build as an R1 binary. The scheduled GitHub **Daily build** workflow publishes these two R3 binaries as a downloadable workflow artifact. A complete R3 `.upt` release requires a separately approved R3 staging image and checksum secret; the stock image is not used automatically.

For a local R3 firmware candidate, supply your own R3 Pro II `.upt` and run `BOARD=r3proii scripts/repack_upt.sh BASE_R3_UPT open_hiby_player_target_r3proii open_hiby_bootloader_r3proii OUTPUT_R3_UPT`. The repacker checks the stock player for the R3 board marker, adds the bootloader handoff, and installs the 480×720 boot assets. Keep the original base image for recovery; the candidate has not been validated on R3 hardware.

When the repository secret `R3PROII_STAGING_IMAGE_SHA256` is configured, the same workflow can verify the R3 base image and publish a second, staging-only workflow artifact containing the repacked `.upt`. The manual workflow's `r3_base_asset` input selects the `.upt` asset from the private `staging-image-base` release. No public GitHub release is created by this path.

## HiBy R3 II 2025

```bash
make target BOARD=r3ii_2025 # -> open_hiby_player_target_r3ii_2025
```

The tree has a 320×480 layout and the same `BOARD` scaling path. Same caution: model-specific, not an R1 drop-in.

Support for other HiBy devices may be possible, but should not be assumed without hardware testing.

---

# Features

- 🎵 **Playback** — local files (FLAC, MP3, WAV, AIFF, DSD, AAC, ALAC, APE, Opus, and others), gapless by default, crossfade, ReplayGain, 10-band PEQ with SD-card presets, hardware buttons, Bluetooth (including Bluetooth DAC), USB DAC, queue / Up Next
- 📚 **Library** — file browser, artists / albums / playlists (including M3U/M3U8), Rockbox tagcache, incremental scans, synced lyrics, SD hotplug, plain-text book reader; audiobooks via plugin
- 🌐 **Network** — Subsonic-compatible streaming (HTTPS), downloads, DLNA renderer, AirPlay via stock protocols where possible, LAN web remote, Wi-Fi music import
- 🔌 **Device** — charge limiter and Safe Charging, idle shutdown or suspend, car mode, USB Storage / DAC / ADB selector, timezone, configurable charge LEDs
- 🎨 **Interface** — stock assets and fonts, swipe navigation, pull-down quick controls, `.theme` files, customizable Home screen, non-Latin text (Cyrillic, Japanese, Korean, Thai)

---

# 🧩 Plugins

Third-party features are plain Lua — no C toolchain, rebuild, or firmware reflash.

Drop a `.lua` file in `.plugins/` on the SD card. Plugins are loaded at startup. Enable or disable them from Settings without deleting the files. The versioned `plugin.*` API covers UI extensions (including Home tiles), playback, library access, HTTP, theming, and PEQ.

Examples live in `plugins_examples/` (Audiobooks, Themes, MSEB, Net Radio, Last.fm, Play Through, and others).

Full API: **[PLUGINS.md](PLUGINS.md)**

---

# Development

## Host simulator (Arch Linux)

Requirements: `sdl2`, `make`, `gcc`/`g++`, `git`. `make` clones LVGL if needed.

Stock graphics are not redistributed — populate `assets/theme2/` from your own firmware dump.

```bash
make
./open_hiby_player_host
```

(`make` is `make host`. Default `BOARD` is `r1`.) Click-and-drag in the SDL2 window simulates touch.

## Cross-compile for HiBy OS

HiBy devices are little-endian MIPS, so target builds use a static musl toolchain (`mipsel-linux-musl-gcc`). On Arch Linux:

```bash
yay -S mipsel-linux-musl-cross
```

That builds from source and takes a while. Other distributions are not documented yet.

```bash
make target
```

Produces a stripped `open_hiby_player_target` (or `open_hiby_player_target_<board>`). Pass `BOARD` as in Device Support.

---

# Installing on a device

## Player-only update (preferred)

> **Requires the full `.upt` to have been flashed first.** This method works only because the open-source bootloader is already on the device, and that bootloader arrives with the firmware image below. On stock firmware nothing reads this path and the update is ignored. Flash the `.upt` once, then use this for every update after it.

Download `open_hiby_player` from the **[releases page](https://github.com/Starnished66/R1-open-source-player/releases)**, or build your own with `make target`. Copy it to this exact path (*inside* `.open_hiby_player`; never replace that directory):

```text
/data/mnt/sd_0/.open_hiby_player/open_hiby_player
```

On the card that is `SD/.open_hiby_player/open_hiby_player`. Use the matching board binary, then reboot — the bootloader copies it to internal storage.

## Full firmware image (`.upt`)

This rewrites the recovery-updatable firmware. Needed once, to put the open-source bootloader on the device, and again only when the bootloader itself changes. After that, player updates go through the SD path above.

Download `r1.upt` from the **[releases page](https://github.com/Starnished66/R1-open-source-player/releases)**. Keep a known-good recovery image first.

### Building one yourself

Build the R1 binaries with `make target BOARD=r1` and `make bootloader BOARD=r1`, then follow **[docs/HOW_TO_BUILD_A_UPT_FILE.md](docs/HOW_TO_BUILD_A_UPT_FILE.md)**.

This needs a base image, which is not in the repository — download the *Base image for new releases* from the releases page. It has to be that approved staging image: the packer checks the base's `hiby_player.sh` and refuses an arbitrary stock `.upt` or an older public beta. It also rejects images over 45 MiB and will not mix R1 and R3 binaries.

To build both board images in one run, provide the approved R1 and R3 Pro II
base images and an output directory:

```sh
scripts/build_both_upt.sh /path/to/r1-base.upt /path/to/r3proii-base.upt output/
```

This builds each board's player and bootloader, then writes `output/r1.upt` and
`output/r3proii.upt` without modifying either base image.

The GitHub **Daily build** workflow also packages both board images as separate
staging artifacts. It reads `r1.upt` and the selected R3 `.upt` from the
`staging-image-base` release and requires their corresponding checksum secrets
(`STAGING_IMAGE_SHA256` and `R3PROII_STAGING_IMAGE_SHA256`). A package job is
shown as skipped when its secret is absent. The weekly public beta remains
R1-only until R3 Pro II hardware validation is complete.

---

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

See **[LICENSE](LICENSE)** (GNU GPL version 3).

The project statically links [FAAD2](https://github.com/knik0/faad2) for AAC decoding, which is GPLv2-licensed. Other major dependencies, including `dr_libs`, LVGL, and tinyalsa, use permissive licenses.

If that copyleft combination is ever undesirable for a particular deployment, the practical solution would be to remove AAC support or replace FAAD2 with a decoder under a compatible permissive license — not to attempt to isolate the existing FAAD2 implementation from the resulting binary.
