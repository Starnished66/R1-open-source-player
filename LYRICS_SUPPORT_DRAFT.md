# Synchronized Lyrics Support — Draft

## Goal

Tapping the 480×480 album artwork on the player screen toggles a synchronized
lyrics view. The artwork becomes blurred and darkened behind the lyrics, and
the highlighted line advances and scrolls with playback. Tapping again returns
to the normal artwork view. Existing playback controls remain unchanged.

## First release scope

- Load a same-basename `.lrc` sidecar beside the local audio file.
- Parse `[mm:ss.xx]`, `[mm:ss.xxx]`, multiple timestamps per line, and
  `[offset:+/-N]`.
- Accept common LRC metadata fields without rendering them as lyric lines.
- Load and parse lyrics asynchronously on each track change.
- Show a virtualized window containing two previous lines, the current line,
  and three or four upcoming lines.
- Highlight the current line with the accent color and mute surrounding lines.
- Synchronize against `audio_get_position_seconds()` and update LVGL only when
  the active line changes.
- Recalculate immediately after seeking and stop advancement while paused.
- Display a short `No synchronized lyrics found` state when no usable file is
  available.

## Architecture

Add `src/library/lyrics.c` and `src/library/lyrics.h`. The parser returns a
timestamp-sorted, caller-owned array of bounded lyric entries. Resolution,
file I/O, parsing, and blur generation must remain off the UI thread. Results
must carry the source track path or generation number so a late worker result
cannot be applied after the track changes.

The player screen gets a lyrics container layered over the artwork. It should
reuse a darkened 480×480 RGB565 blur derived once from the existing decoded
cover, using the current separable box-blur implementation. Do not blur during
UI refreshes. Keep only the current track's lyrics background and release
temporary channel planes after generation.

Use the existing 500 ms player timer for synchronization. Advance from the
current lyric index during normal playback and use binary search after a seek
or backward position jump. Animate only line transitions, for approximately
200–300 ms, and suppress animation while the screen is off.

## Resource and correctness limits

- Cap an LRC file at 256–512 KiB.
- Cap line length and total parsed line count.
- Sanitize invalid UTF-8 and clamp malformed or unreasonable timestamps.
- Avoid one LVGL object per lyric line; reuse the small visible label set.
- Do not read the SD card from `update_timer_cb()`.
- Handle pause, backward/forward seeks, rapid track changes, missing SD card,
  missing artwork, and playback completion.
- Use the default cover as the blurred source when no track artwork exists.

## Later phases

- Embedded synchronized lyrics such as ID3v2 `SYLT` and container equivalents.
- Unsynchronized embedded lyrics or `.txt` display.
- Manual lyric scrolling with timed return to the active line.
- Optional setting to keep lyrics visible across track changes.
- Streaming-service lyric sources where an authenticated API provides them.

## Verification

- Parser tests for malformed, multilingual, multi-timestamp, offset, empty,
  and very large files.
- Device tests for normal playback, pause/resume, seeking in both directions,
  track skipping during an active load, SD removal, and screen off/on.
- Profile CPU, RSS, UI responsiveness, and battery impact with and without the
  lyrics view visible.
