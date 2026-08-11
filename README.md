# Open Source Player for HiBy OS

A from-scratch, open-source reimplementation of the HiBy R1's stock music player firmware, built in C with LVGL. It fully replaces the closed-source stock `hiby_player` binary while reusing the stock firmware's own UI assets, fonts, and system daemons (Bluetooth, Wi-Fi, DLNA) wherever it makes sense to.

**Terminology**:
- "host device", "host": the machine you're developing on (your laptop/PC).
- "target device", "target": the machine you're building for (the R1 or R3 Pro II).

## License

This project is **GNU GPLv2** (see [LICENSE](LICENSE)) because it statically links [FAAD2](https://github.com/knik0/faad2) for AAC decoding, which is GPLv2-licensed. That's a stronger copyleft than the project's other dependencies (dr_libs, LVGL, tinyalsa are all permissive), and it binds the whole resulting binary, not just the AAC-decoding piece. If that's ever a problem, the fix is dropping AAC support (or swapping FAAD2 for something non-copyleft), not trying to isolate it.

## Features

Confirmed working on real R1 hardware (fbdev + evdev touch + tinyalsa), not just the host simulator. Deeper technical detail — root causes, verification methods, real-device bugs found and fixed — lives in [Technical Notes](#technical-notes--real-device-findings) below; this list is just the "what."

**Playback**
- Formats: FLAC, MP3, WAV, AIFF/AIFF-C, DSD (`.dsf`/`.dff`, DSD64/128), AAC (raw ADTS + `.m4a`), ALAC, APE (Monkey's Audio)
- Gapless playback, with an opt-in crossfade
- ReplayGain track gain
- Parametric EQ — 10 ISO-standard bands, per-band type/gain/Q, pre-amp
- Physical hardware buttons: volume, play/pause, next/prev
- Auto-advance to the next track; stops at playlist end

**Library**
- File browser (`./music` on host, the SD card mount point on target)
- Music → All Songs / Artists / Albums / Album Artist / Genres, built from real tag scans
- M3U/M3U8 playlist support
- Real tag metadata (FLAC, MP3 ID3v2/v1, WAV RIFF, M4A) and embedded album art (JPEG/PNG)

**Network**
- Subsonic-compatible streaming (Subsonic, Navidrome, Airsonic, …) — browse and stream over HTTPS with real TLS
- DLNA/UPnP-AV renderer — cast a track from a controller app on your phone/PC and it plays through this device

**Device integration**
- Real battery percentage and Wi-Fi signal status (read the same way the stock firmware does)
- Time Zone picker (Region → City, applies immediately and persists)
- Charge limiter — caps charging at 85% for battery longevity (Charge cap differ from one device to another(some at 88% and some at 91%, still in investigation, but works so far)
- Sleep timer and a crossfade quick-toggle in the pull-down drawer
- Startup volume (launch at a fixed level instead of resuming the last one)
- Non-Latin text rendering: Cyrillic, Japanese kana/kanji, Korean Hangul, Thai
- Topbar play/pause and Bluetooth A2DP status icons; auto-stops playback if the audio output disconnects mid-track
- Interactive (finger-following) swipe into the now-playing screen
- App-wide accent color theming
- Real-asset UI — built from the stock firmware's own icon/theme pack, not hand-drawn widgets

**Not yet implemented**: WMA Lossless , MSEB (HiBy's proprietary DSP effect), and Remote Control Playback(Similar to the Proprietary Hiby Link).

### A note on device testing

If you're testing on real hardware over ADB: stop the stock player gracefully (`killall hiby_player` **then** `killall -9 hiby_player` if it's still around, matching what `hiby_player.sh` itself does) before launching this binary. Force-killing it straight to `-9` was observed to leave the framebuffer/backlight in a bad state that persisted across warm reboots (`adb reboot`) and even affected the stock player on relaunch — only a full power-off/power-on cycle recovered it. Also note `/tmp` is tmpfs, so anything pushed there is wiped on every reboot.

## 1. Local Development (Arch Linux Host Simulation)

Running the GUI simulated on the host system (i.e. your laptop).

### Requirements

- `sdl2`
- `make`
- `gcc`
- `pkg-config`
- `git`

### Running the Simulator

```bash
make
```

This automatically clones LVGL (if not already present) and compiles the project for your host architecture.

```bash
./open_hiby_player_host
```

Opens an SDL2 window. Click-and-drag works the same as touch/scroll on the real device.

---

## 2. Cross-Compiling for the HiBy Device (MIPS Target)

The device's rootfs ships an old glibc (2.22), with no readily available matching cross sysroot. Rather than fight that, the target build uses a **musl-based static toolchain**: the resulting binary is statically linked and depends only on the (stable) Linux kernel syscall ABI, not the device's libc. This is the standard approach for cross-compiling to old/obscure embedded Linux targets.

### Install Target Cross-Compiler

```bash
# Arch Linux:
yay -S mipsel-linux-musl-cross
```

Builds a full self-contained toolchain (gcc + musl libc) from source — can take 15-30+ minutes the first time. No documented install path for other distros yet; contributions welcome.

### Build for Target

```bash
make target
```

Produces `open_hiby_player_target`, statically linked for MIPS (mipsel).

### Testing Without Hardware

Sanity-check that the binary runs (not full device behavior — no real framebuffer/touch) via QEMU user-mode emulation:

```bash
# Arch Linux:
sudo pacman -S qemu-user

qemu-mipsel ./open_hiby_player_target
```

It should print its startup banner, then fail trying to open `/dev/fb0`/`/dev/input/eventN` — that failure means the binary itself is working correctly, since those devices don't exist on a normal host.

### Modifying Unpacked Firmware

(TODO)

---

## Technical Notes & Real-Device Findings

Deeper implementation detail and real-hardware bugs found (and fixed) during development, organized by area, now lives in [TECHNICAL_NOTES.md](TECHNICAL_NOTES.md) — skip it unless you're modifying the code or debugging something specific.
