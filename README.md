# Hiby Open Source Player

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
- Charge limiter — caps charging at 85% for battery longevity
- Sleep timer and a crossfade quick-toggle in the pull-down drawer
- Startup volume (launch at a fixed level instead of resuming the last one)
- Non-Latin text rendering: Cyrillic, Japanese kana/kanji, Korean Hangul, Thai
- Topbar play/pause and Bluetooth A2DP status icons; auto-stops playback if the audio output disconnects mid-track
- Interactive (finger-following) swipe into the now-playing screen
- App-wide accent color theming
- Real-asset UI — built from the stock firmware's own icon/theme pack, not hand-drawn widgets

**Not yet implemented**: WMA Lossless (no way to produce a verifiable test file in this dev environment), MSEB (HiBy's proprietary DSP effect), and Jellyfin support.

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

Deeper implementation detail and real-hardware bugs found (and fixed) during development, organized by area. Skip this section unless you're modifying the code or debugging something specific.

### Audio decoders

- **DSD**: decimated (8:1 for DSD64, 16:1 for DSD128) to a fixed 352.8kHz PCM stream via a hand-designed windowed-sinc (Blackman) filter rather than a naive single-stage decimation to 44.1kHz. Verified against synthetic delta-sigma test tones and the filter's analytical frequency response. **Not yet verified for real-time CPU performance on target hardware.**
- **AAC**: originally planned to `dlopen()` the R1's system `libfdk-aac.so.2` at runtime, avoiding a vendored decoder — not possible, since musl's `dlopen()` refuses to work from statically-linked binaries (confirmed via QEMU against the real device library). FAAD2 is vendored and statically linked instead, which is why this project is GPLv2.
- **ALAC / MP4-contained AAC**: share a small hand-written MP4/ISO-BMFF box parser (`mp4_demux.c`) rather than a full container library. ALAC uses Apple's own reference decoder (Apache 2.0). Verified bit-exact against `ffmpeg`'s ALAC decode on real files and confirmed on real hardware. That on-device pass caught two real bugs: the vendored `EndianPortable.c` only self-detected little-endian for x86/Win32 (silently corrupting sample rate on mipsel, fixed via `-DTARGET_RT_LITTLE_ENDIAN=1`), and tinyalsa's `pcm_config` needed explicit `start_threshold`/`stop_threshold` instead of the zeroed default.
- **APE (Monkey's Audio)**: a from-scratch C port of the relevant parts of FFmpeg's `libavcodec/apedec.c` (LGPL 2.1+), scoped to fileversion 3980-3990 (unchanged since 2007, so this covers virtually all real files). **Unlike every other decoder here, only checked structurally** — no Monkey's Audio encoder was available in this dev environment to produce a real ground-truth file, so the predictor/entropy/filter math is unverified against a genuine `.ape` file.

### Metadata and album art

- Real tags: FLAC VORBIS_COMMENT, MP3 ID3v2.3/2.4 (falling back to ID3v1), WAV RIFF LIST/INFO. Handles UTF-16 and UTF-8, non-Latin scripts included.
- Embedded art: FLAC METADATA_BLOCK_PICTURE, MP3 ID3v2 APIC, M4A/ALAC `covr` atoms. LVGL's JPEG decoder (tjpgd) doesn't self-detect dimensions from in-memory sources the way its PNG decoder does — fixed by parsing the JPEG's own SOFn marker first (`jpeg_get_dimensions()` in `gui.c`). Shown at native resolution; a scale-to-fit pass was tried and dropped since LVGL's zoom transform doesn't apply to tjpgd's tile-decoded output. **Known limitation**: tjpgd silently fails on JPEGs using unusual chroma sampling (uniform non-1:1 factors across components) — real-world camera/phone/scraper output isn't affected, but a specific file's art might not show.
- Genre parsing only handles plain-text genre tags — legacy numeric ID3v1-style codes (e.g. `"(17)"`) aren't resolved to a name.

### Library scanning

- Music → All Songs / Artists / Albums / Album Artist / Genres are built from one library-wide tag-reading pass at startup (`library_scan_once()`), not live filesystem watching — matches a personal library's "rescan on restart" expectation.
- Untagged songs get an explicit "Unknown Artist/Album/Genre" bucket rather than disappearing. Album Artist is the one exception: with no explicit tag, it falls back to the track's own artist (the common non-compilation case) rather than "Unknown."

### Playback engine

- **Gapless + crossfade** (`audio.c`): a single long-lived playback thread for the app's whole life, rather than one thread per track. `gui.c` arms the next track ahead of time; the thread prefetches and hands off on its own near the current track's end, no GUI round-trip. Same-format back-to-back tracks hand off with the output device never closed (no click/gap); a format change still needs a brief reopen. Crossfade (off by default) blends a fixed 3-second fade, only when sample rate and channel count match exactly. Verified with a standalone harness driving `audio.c` directly through repeated auto-advances, a sample-rate-mismatch fallback, and 200 rapid manual restarts with no crash or deadlock.
- **ReplayGain**: reads `REPLAYGAIN_TRACK_GAIN`/`_PEAK` from FLAC/MP3 tags, applied as a linear multiplier clamped by peak. Verified against real fixtures tagged via `metaflac`/`mutagen`.
- **Hardware volume taper**: the codec's own ALSA "Playback Volume" registers now carry the volume curve directly, with digital PCM gain pinned at unity — fixes an audible noise floor at low volumes from digital attenuation. These registers looked non-functional at first (`amixer cget` always reports 0 regardless of what was written), but the write genuinely reaches the DAC.

### Real-asset UI

The whole screen hierarchy is built from the stock firmware's own `theme2` resource pack rather than hand-drawn widgets — real icons, pill-list rows, player-screen artwork, loaded at runtime via `LV_USE_LODEPNG`. Two reusable builders (`build_icon_grid_screen`, `build_pill_list_screen`) cover every icon-grid and settings-style screen. Neither build ships these assets: `assets/theme2/` is gitignored — populate it yourself from your own device/firmware dump (see `.gitignore`'s own note); on target the app reads the firmware's own copy directly. Nothing from the stock firmware is redistributed in this repo.

### App-wide accent color

A palette of swatches restyles sliders, switches, and the selected EQ band everywhere at once, via one shared LVGL style object. `lv_obj_report_style_change()` is the API that safely propagates this — a plain `lv_obj_refresh_style(NULL, ...)` looks like it should work too but crashes on a NULL dereference (confirmed via `gdb`). Only reaches native LVGL widgets; the player screen's PNG-sprite controls keep the firmware's own baked-in colors.

### Real device status (battery, Wi-Fi)

- **Battery**: reads `/sys/class/power_supply` the same way the stock binary does (confirmed via `strings`), scanning every `"Battery"`-typed entry rather than hardcoding a driver name. Real-device bug found: the R1 exposes *two* such entries (`axp_battery`, a raw PMIC node stuck at `capacity=0`, and `battery`, the real fuel gauge) — the scan used to stop at whichever `readdir()` returned first, sometimes locking onto the wrong one. Fixed to prefer a nonzero reading.
- **Wi-Fi**: shells out to `wpa_cli -i wlan0 status`/`signal_poll`, matching the stock binary. RSSI-to-signal-level thresholds aren't recoverable from the binary's strings, so conventional dBm buckets are used and documented as such rather than passed off as extracted values.

### Non-Latin text rendering

Cyrillic, Japanese kana/kanji, Korean Hangul, Thai (`src/fallback_font.c`). An earlier FreeType-based attempt hung the boot logo twice on real hardware and was reverted; this retry uses LVGL's lighter `tiny_ttf` renderer, chaining fallback fonts behind each label's primary font, with the actual font load deferred to a one-shot timer after the first frame is already on screen. Cyrillic/Japanese use an offline-converted, subsetted copy of the stock firmware's own CJK font; Korean/Thai are read directly from the stock firmware's own files at runtime. Arabic isn't covered (no font on-device has the glyphs, and correct rendering needs contextual shaping this project doesn't implement).

**Flash-image size fix**: the CJK font's kanji coverage originally included the *entire* Unicode CJK Unified Ideographs block plus Extension A (~27,500 characters) — this alone added ~9MB to the flash image and pushed a repack over its 45MB limit, since glyph outline data barely compresses. Narrowed to the 2,136-character Jōyō kanji list (Japan's official "common use" standard), cutting the file from 9.56MB to 714KB. Real tradeoff: a tag using a kanji outside Jōyō shows a blank glyph. JIS X 0208 (~6,355 kanji) is the documented next step up if the flash budget allows it later.

### Network streaming (Subsonic)

- **TLS from scratch**: the R1's own `libcurl`/OpenSSL are glibc-built and unreachable from a static musl binary (same `dlopen()` wall as AAC above). mbedTLS (Apache 2.0) is vendored instead, cross-compiled with zero source changes for both host and target. A Mozilla CA bundle is embedded as a C array for real certificate verification, since the device's own `/etc/ssl/certs` is empty.
- **`http_client.c`**: a hand-rolled HTTP/1.1 client (GET only) over raw sockets, wrapped in mbedTLS for HTTPS. Handles `Content-Length` and chunked transfer-encoding, buffered-in-memory or streamed-to-file. Verified against real HTTPS endpoints — real TLS 1.3 handshake, chunked decoding, binary download, error handling.
- **`subsonic_client.c`**: token-based auth (`t=md5(password+salt)`, mandated by the Subsonic protocol), JSON response mode (avoiding an XML parser). Verified against a local mock server implementing real Subsonic JSON shapes (no real Subsonic/Navidrome instance was available in this dev environment).
- **Download-then-play, not true streaming decode**: every decoder here is built around a plain seekable `FILE*`; retrofitting all of them for streaming reads was out of scope. A track downloads whole to `/tmp` first, with a "Downloading..." screen, before handoff to the normal decoder pipeline.
- **Known gaps**: no error/toast UI yet for a wrong password, unreachable server, or failed download — it just silently leaves you where you were. Jellyfin, remote Album Artist/Genre browsing, and search are out of scope for now.

### DLNA/UPnP-AV renderer

Reuses the stock firmware's own `dmrd` binary (a customized build of the open-source gmrender-resurrect project) for the actual SSDP/SOAP/UPnP protocol handling, rather than reimplementing that whole surface from scratch. `dmrd` doesn't decode or play audio itself — it relays an undocumented plain-text command protocol over a Unix domain socket (`/data/dmr_streamer`) to a companion process, which this project now is. Reverse-engineered live on a real device (traffic capture while casting from a phone and a PC): `set_uri`/`set_meta:title/artist/album`/`play@N`/`stop`/`get_volume` are all real and relayed correctly. Cast tracks are downloaded (Content-Type-based format detection, since cast URLs have no file extension) and played through the normal local decoder pipeline — same download-then-play shape as Subsonic streaming above.

**Known limitation, not fixable from this side**: real-device testing found genuine bugs inside `dmrd` itself — `Pause` fails with a SOAP fault ("Transition not allowed"), `Mute`/`SetMute` fail with "Missing action request argument (CurrentMute)," and `GetVolume`'s response doesn't reliably reflect back to the controller (silently, no error). All three fail inside dmrd's own SOAP layer, before ever reaching this project's code. Seek was never reachable to test. Play/stop/cast and metadata display are fully supported; treat pause/mute/volume/seek from a DLNA controller as unsupported.

### Time Zone selector

Region → City picker (Settings → Time Zone) matching the stock firmware's own flow, generated offline from the stock firmware's own city-name list (ICU CLDR format) cross-checked against the device's real `/usr/share/zoneinfo` tree. Applies immediately (`setenv("TZ", ...)` + `tzset()`) and persists across reboots by writing `/usr/data/localtime`. No manual date/time entry — the RTC already ships a reasonable time; only the zone needed a picker.

### Charge limiter fix

Real-device testing found charging actually stopped around 91-92% instead of the configured 85%. Root cause (confirmed against the official AXP2101 datasheet): the original fix zeroed the PMIC's constant-*current*-phase target (REG62/ICC), but once the battery reaches target voltage the charger enters constant-*voltage* mode and current tapers on its own — REG62 stops being the controlling constraint well before 85%. Fixed by disabling the charger's actual master enable bit (`module_en`/REG18H bit 1) instead, which halts the whole charge state machine — a careful read-modify-write, since that register also holds the fuel-gauge and watchdog enable bits.

### Topbar icons and auto-stop

Play/pause and Bluetooth A2DP status icons live in the same left-side flex row as the existing headphone-jack icon, so the row's existing hidden-children-collapse behavior handles layout for free. Pulling the headphone jack or disconnecting Bluetooth headphones mid-playback now stops playback outright (with a toast) instead of continuing to output audio nowhere — checked every timer tick, including while the screen is off.

### Drawer animation fix

The quick-access drawer's open/close slide got visually "sluggish" under repeated fast taps. Root cause: LVGL's own concurrent-animation de-duplication only fires when an animation sets `early_apply`, which the drawer's animations never did — re-triggering it mid-animation left two competing animations alive on the same object. Fixed by explicitly deleting any in-flight drawer animation before starting a new one.

### Interactive player-screen swipe

Swiping right-to-left now drags the player screen in live under the finger and either commits or springs back based on release position/flick velocity, rather than a fixed-duration fire-and-forget slide. Shares its transition machinery with the existing back-swipe. One real bug found: the existing fast-path optimization aliases the "from screen" snapshot directly to the live framebuffer rather than copying it — safe for the original ~165ms fixed-duration transition, but this interactive drag has no such bound, so the screen underneath visibly bled through in real time. Fixed with a `force_snapshot` parameter forcing a real, independent snapshot for this call site only.

### Import via Wi-Fi robustness

Fixed a real crash (stack overflow in the background library-scan thread) from unbounded/symlink-following directory recursion during the post-import rescan, and fixed the screen's own process cleanup (`killall` with no verification of actual death → `SIGKILL` + a bounded wait) that could leave orphaned server processes accumulating on this device's limited RAM. The rescan is now opt-in via a confirmation prompt.

### Touch, navigation, and input bugs found on real hardware

The stock `hiby_player.sh` launcher isn't a simple supervisor — it's a foreground script that runs `sleep 1; reboot` immediately after `hiby_player` exits, so killing the stock player directly triggers a full device reboot. The safe stop sequence is killing `hiby_player.sh` first (interrupting its blocking wait before the reboot line), then `hiby_player`. A few real, hardware-only bugs surfaced this way that never showed up in the host simulator:

- **Touch coordinate calibration**: the touch controller's declared native range (`EVIOCGABS`, 0-720/0-1280) turned out to be stale firmware metadata — empirical corner-tap testing showed the controller's real live output is already in 480x800 pixel space, matching the LCD directly. Trusting the declared range made every tap land stretched ~1.5-1.6x from the finger. Fixed by using raw passthrough with no calibration.
- **Back-button hit targets too tight**: every back button used a 44x44 clickable area; real taps aimed at the corner landed a handful of pixels outside it. Enlarged to 64x64 everywhere, icon kept at its original visual size.
- **Swipe-to-go-back not firing reliably**: every screen's root object never had `LV_OBJ_FLAG_SCROLLABLE` removed, so a drag was first consumed as a scroll attempt. Fixed centrally in `finalize_screen_navigation()`.
- **Scrollable containers swallowing the back-swipe**: on top of the fix above, the *content containers* inside each shared screen builder also defaulted to scrollable in every direction, so a horizontal swipe never reached the screen-level gesture handler on About and every Music-submenu screen. Fixed by restricting each one to vertical-only scrolling.
- **Physical power button did nothing**: `hw_buttons.c` deliberately left `KEY_POWER` unhandled, assuming the system would intercept it — nothing does once `hiby_player.sh` is out of the picture. Now toggles the backlight directly.
- Physical skip/next was confirmed reaching the app correctly via raw evdev capture — a reported "not working" case turned out to be an empty/single-track playlist, not a code bug.
