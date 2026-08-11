# Technical Notes & Real-Device Findings

Deeper implementation detail and real-hardware bugs found (and fixed)
during development, organized by area. See [README.md](README.md) for
the "what" (feature list, build/test instructions) and
[ISSUES.md](ISSUES.md) (local, gitignored) for the shorter "what's still
broken/missing" list.

## Audio decoders

- **DSD**: decimated (8:1 for DSD64, 16:1 for DSD128) to a fixed 352.8kHz PCM stream via a hand-designed windowed-sinc (Blackman) filter rather than a naive single-stage decimation to 44.1kHz. Verified against synthetic delta-sigma test tones and the filter's analytical frequency response. **Not yet verified for real-time CPU performance on target hardware.**
- **AAC**: originally planned to `dlopen()` the R1's system `libfdk-aac.so.2` at runtime, avoiding a vendored decoder — not possible, since musl's `dlopen()` refuses to work from statically-linked binaries (confirmed via QEMU against the real device library). FAAD2 is vendored and statically linked instead, which is why this project is GPLv2.
- **ALAC / MP4-contained AAC**: share a small hand-written MP4/ISO-BMFF box parser (`mp4_demux.c`) rather than a full container library. ALAC uses Apple's own reference decoder (Apache 2.0). Verified bit-exact against `ffmpeg`'s ALAC decode on real files and confirmed on real hardware. That on-device pass caught two real bugs: the vendored `EndianPortable.c` only self-detected little-endian for x86/Win32 (silently corrupting sample rate on mipsel, fixed via `-DTARGET_RT_LITTLE_ENDIAN=1`), and tinyalsa's `pcm_config` needed explicit `start_threshold`/`stop_threshold` instead of the zeroed default.
- **APE (Monkey's Audio)**: a from-scratch C port of the relevant parts of FFmpeg's `libavcodec/apedec.c` (LGPL 2.1+), scoped to fileversion 3980-3990 (unchanged since 2007, so this covers virtually all real files). **Unlike every other decoder here, only checked structurally** — no Monkey's Audio encoder was available in this dev environment to produce a real ground-truth file, so the predictor/entropy/filter math is unverified against a genuine `.ape` file.

## Metadata and album art

- Real tags: FLAC VORBIS_COMMENT, MP3 ID3v2.3/2.4 (falling back to ID3v1), WAV RIFF LIST/INFO. Handles UTF-16 and UTF-8, non-Latin scripts included.
- Embedded art: FLAC METADATA_BLOCK_PICTURE, MP3 ID3v2 APIC, M4A/ALAC `covr` atoms. LVGL's JPEG decoder (tjpgd) doesn't self-detect dimensions from in-memory sources the way its PNG decoder does — fixed by parsing the JPEG's own SOFn marker first (`jpeg_get_dimensions()` in `gui.c`). Shown at native resolution; a scale-to-fit pass was tried and dropped since LVGL's zoom transform doesn't apply to tjpgd's tile-decoded output. **Known limitation**: tjpgd silently fails on JPEGs using unusual chroma sampling (uniform non-1:1 factors across components) — real-world camera/phone/scraper output isn't affected, but a specific file's art might not show.
- Genre parsing only handles plain-text genre tags — legacy numeric ID3v1-style codes (e.g. `"(17)"`) aren't resolved to a name.

## Library scanning

- Music → All Songs / Artists / Albums / Album Artist / Genres are built from one library-wide tag-reading pass at startup (`library_scan_once()`), not live filesystem watching — matches a personal library's "rescan on restart" expectation.
- Untagged songs get an explicit "Unknown Artist/Album/Genre" bucket rather than disappearing. Album Artist is the one exception: with no explicit tag, it falls back to the track's own artist (the common non-compilation case) rather than "Unknown."

## Playback engine

- **Gapless + crossfade** (`audio.c`): a single long-lived playback thread for the app's whole life, rather than one thread per track. `gui.c` arms the next track ahead of time; the thread prefetches and hands off on its own near the current track's end, no GUI round-trip. Same-format back-to-back tracks hand off with the output device never closed (no click/gap); a format change still needs a brief reopen. Crossfade (off by default) blends a fixed 3-second fade, only when sample rate and channel count match exactly. Verified with a standalone harness driving `audio.c` directly through repeated auto-advances, a sample-rate-mismatch fallback, and 200 rapid manual restarts with no crash or deadlock.
- **ReplayGain**: reads `REPLAYGAIN_TRACK_GAIN`/`_PEAK` from FLAC/MP3 tags, applied as a linear multiplier clamped by peak. Verified against real fixtures tagged via `metaflac`/`mutagen`.
- **Hardware volume taper**: the codec's own ALSA "Playback Volume" registers now carry the volume curve directly, with digital PCM gain pinned at unity — fixes an audible noise floor at low volumes from digital attenuation. These registers looked non-functional at first (`amixer cget` always reports 0 regardless of what was written), but the write genuinely reaches the DAC.

## Real-asset UI

The whole screen hierarchy is built from the stock firmware's own `theme2` resource pack rather than hand-drawn widgets — real icons, pill-list rows, player-screen artwork, loaded at runtime via `LV_USE_LODEPNG`. Two reusable builders (`build_icon_grid_screen`, `build_pill_list_screen`) cover every icon-grid and settings-style screen. Neither build ships these assets: `assets/theme2/` is gitignored — populate it yourself from your own device/firmware dump (see `.gitignore`'s own note); on target the app reads the firmware's own copy directly. Nothing from the stock firmware is redistributed in this repo.

## App-wide accent color

A palette of swatches restyles sliders, switches, and the selected EQ band everywhere at once, via one shared LVGL style object. `lv_obj_report_style_change()` is the API that safely propagates this — a plain `lv_obj_refresh_style(NULL, ...)` looks like it should work too but crashes on a NULL dereference (confirmed via `gdb`). Only reaches native LVGL widgets; the player screen's PNG-sprite controls keep the firmware's own baked-in colors.

## Real device status (battery, Wi-Fi)

- **Battery**: reads `/sys/class/power_supply` the same way the stock binary does (confirmed via `strings`), scanning every `"Battery"`-typed entry rather than hardcoding a driver name. Real-device bug found: the R1 exposes *two* such entries (`axp_battery`, a raw PMIC node stuck at `capacity=0`, and `battery`, the real fuel gauge) — the scan used to stop at whichever `readdir()` returned first, sometimes locking onto the wrong one. Fixed to prefer a nonzero reading.
- **Wi-Fi**: shells out to `wpa_cli -i wlan0 status`/`signal_poll`, matching the stock binary. RSSI-to-signal-level thresholds aren't recoverable from the binary's strings, so conventional dBm buckets are used and documented as such rather than passed off as extracted values.

## Non-Latin text rendering

Cyrillic, Japanese kana/kanji, Korean Hangul, Thai (`src/fallback_font.c`). An earlier FreeType-based attempt hung the boot logo twice on real hardware and was reverted; this retry uses LVGL's lighter `tiny_ttf` renderer, chaining fallback fonts behind each label's primary font, with the actual font load deferred to a one-shot timer after the first frame is already on screen. Cyrillic/Japanese use an offline-converted, subsetted copy of the stock firmware's own CJK font; Korean/Thai are read directly from the stock firmware's own files at runtime. Arabic isn't covered (no font on-device has the glyphs, and correct rendering needs contextual shaping this project doesn't implement).

**Flash-image size fix**: the CJK font's kanji coverage originally included the *entire* Unicode CJK Unified Ideographs block plus Extension A (~27,500 characters) — this alone added ~9MB to the flash image and pushed a repack over its 45MB limit, since glyph outline data barely compresses. Narrowed to the 2,136-character Jōyō kanji list (Japan's official "common use" standard), cutting the file from 9.56MB to 714KB. Real tradeoff: a tag using a kanji outside Jōyō shows a blank glyph. JIS X 0208 (~6,355 kanji) is the documented next step up if the flash budget allows it later.

## Network streaming (Subsonic)

- **TLS from scratch**: the R1's own `libcurl`/OpenSSL are glibc-built and unreachable from a static musl binary (same `dlopen()` wall as AAC above). mbedTLS (Apache 2.0) is vendored instead, cross-compiled with zero source changes for both host and target. A Mozilla CA bundle is embedded as a C array for real certificate verification, since the device's own `/etc/ssl/certs` is empty.
- **`http_client.c`**: a hand-rolled HTTP/1.1 client (GET only) over raw sockets, wrapped in mbedTLS for HTTPS. Handles `Content-Length` and chunked transfer-encoding, buffered-in-memory or streamed-to-file. Verified against real HTTPS endpoints — real TLS 1.3 handshake, chunked decoding, binary download, error handling.
- **`subsonic_client.c`**: token-based auth (`t=md5(password+salt)`, mandated by the Subsonic protocol), JSON response mode (avoiding an XML parser). Verified against a local mock server implementing real Subsonic JSON shapes (no real Subsonic/Navidrome instance was available in this dev environment).
- **Download-then-play, not true streaming decode**: every decoder here is built around a plain seekable `FILE*`; retrofitting all of them for streaming reads was out of scope. A track downloads whole to `/tmp` first, with a "Downloading..." screen, before handoff to the normal decoder pipeline.
- **Known gaps**: no error/toast UI yet for a wrong password, unreachable server, or failed download — it just silently leaves you where you were. Jellyfin, remote Album Artist/Genre browsing, and search are out of scope for now.

## DLNA/UPnP-AV renderer

Reuses the stock firmware's own `dmrd` binary (a customized build of the open-source gmrender-resurrect project) for the actual SSDP/SOAP/UPnP protocol handling, rather than reimplementing that whole surface from scratch. `dmrd` doesn't decode or play audio itself — it relays an undocumented plain-text command protocol over a Unix domain socket (`/data/dmr_streamer`) to a companion process, which this project now is. Reverse-engineered live on a real device (traffic capture while casting from a phone and a PC): `set_uri`/`set_meta:title/artist/album`/`play@N`/`stop`/`get_volume` are all real and relayed correctly. Cast tracks are downloaded (Content-Type-based format detection, since cast URLs have no file extension) and played through the normal local decoder pipeline — same download-then-play shape as Subsonic streaming above.

**Known limitation, not fixable from this side**: real-device testing found genuine bugs inside `dmrd` itself — `Pause` fails with a SOAP fault ("Transition not allowed"), `Mute`/`SetMute` fail with "Missing action request argument (CurrentMute)," and `GetVolume`'s response doesn't reliably reflect back to the controller (silently, no error). All three fail inside dmrd's own SOAP layer, before ever reaching this project's code. Seek was never reachable to test. Play/stop/cast and metadata display are fully supported; treat pause/mute/volume/seek from a DLNA controller as unsupported.

## Time Zone selector

Region → City picker (Settings → Time Zone) matching the stock firmware's own flow, generated offline from the stock firmware's own city-name list (ICU CLDR format) cross-checked against the device's real `/usr/share/zoneinfo` tree. Applies immediately (`setenv("TZ", ...)` + `tzset()`) and persists across reboots by writing `/usr/data/localtime`. No manual date/time entry — the RTC already ships a reasonable time; only the zone needed a picker.

## Charge limiter fix

Real-device testing found charging actually stopped around 91-92% instead of the configured 85%. Root cause (confirmed against the official AXP2101 datasheet): the original fix zeroed the PMIC's constant-*current*-phase target (REG62/ICC), but once the battery reaches target voltage the charger enters constant-*voltage* mode and current tapers on its own — REG62 stops being the controlling constraint well before 85%. Fixed by disabling the charger's actual master enable bit (`module_en`/REG18H bit 1) instead, which halts the whole charge state machine — a careful read-modify-write, since that register also holds the fuel-gauge and watchdog enable bits.

## Topbar icons and auto-stop

Play/pause and Bluetooth A2DP status icons live in the same left-side flex row as the existing headphone-jack icon, so the row's existing hidden-children-collapse behavior handles layout for free. Pulling the headphone jack or disconnecting Bluetooth headphones mid-playback now stops playback outright (with a toast) instead of continuing to output audio nowhere — checked every timer tick, including while the screen is off.

## Drawer animation fix

The quick-access drawer's open/close slide got visually "sluggish" under repeated fast taps. Root cause: LVGL's own concurrent-animation de-duplication only fires when an animation sets `early_apply`, which the drawer's animations never did — re-triggering it mid-animation left two competing animations alive on the same object. Fixed by explicitly deleting any in-flight drawer animation before starting a new one.

## Interactive player-screen swipe

Swiping right-to-left now drags the player screen in live under the finger and either commits or springs back based on release position/flick velocity, rather than a fixed-duration fire-and-forget slide. Shares its transition machinery with the existing back-swipe. One real bug found: the existing fast-path optimization aliases the "from screen" snapshot directly to the live framebuffer rather than copying it — safe for the original ~165ms fixed-duration transition, but this interactive drag has no such bound, so the screen underneath visibly bled through in real time. Fixed with a `force_snapshot` parameter forcing a real, independent snapshot for this call site only.

## Import via Wi-Fi robustness

Fixed a real crash (stack overflow in the background library-scan thread) from unbounded/symlink-following directory recursion during the post-import rescan, and fixed the screen's own process cleanup (`killall` with no verification of actual death → `SIGKILL` + a bounded wait) that could leave orphaned server processes accumulating on this device's limited RAM. The rescan is now opt-in via a confirmation prompt.

## Touch, navigation, and input bugs found on real hardware

See [ISSUES.md](ISSUES.md) (local, gitignored) for the real-hardware touch/navigation/input bug list previously kept here.
