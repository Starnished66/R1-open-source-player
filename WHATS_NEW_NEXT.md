# What's New — Next Release

Unreleased changes following the September 14, 2026 release notes.

## Charging and USB

- **No unnecessary library scans after charging.** Unplugging from a wall
  charger or a charging-only cable no longer triggers a database update.
  Automatic scans now require a confirmed USB Storage connection to a computer
  and still respect the auto-rescan setting.
- A confirmed Storage session is remembered even if the USB controller
  suspends or resets before unplugging. Unreadable power-status samples are
  no longer treated as disconnections.

This detects a computer Storage session, not individual file transfers: a
computer connection can still trigger a scan even if no files were changed.

## Behind the scenes

- The battery icon in the status bar is now larger and easier to read at a
  glance.
- Bluetooth status checks behind the top bar and quick-drawer icons no
  longer fork one process per paired device on every refresh; a single
  check is used when the installed Bluetooth tools support it. This removes
  a source of UI slowdown lasting a couple of minutes after turning on both
  Bluetooth and Wi-Fi.
- Library scans that fail to save now record the specific cause (low disk
  space, low memory, or a write error) in the diagnostic log instead of a
  plain pass/fail flag, when database logging is enabled in Developer
  Options.
- Corrected the upgraded Bluetooth service's pairing-storage location so
  saved headphones and trust settings survive a restart. Previously, manual
  connection worked for the current boot but pairing could not be saved to
  the read-only firmware filesystem.
- Turning Bluetooth on now makes a bounded background attempt to reconnect
  the last used headphones. Manual device selection and turning Bluetooth
  off cancel the attempt; Bluetooth DAC mode is unchanged.
- Bluetooth sample-rate changes no longer count as an immediate headphone
  disconnect. Brief audio-output removal/re-creation is given time to settle
  before pausing playback or clearing the route; persistent removals still
  trigger the disconnect protection.
- Improved playback restart recovery: temporary output-open failures now get
  bounded retries, and eligible 24-bit tracks keep their output format during
  pause, seek and track-change fades. Bluetooth routing clears on disconnect
  or power-off and requires a usable audio connection. These changes still
  need testing on the player.
- Routine volume and playback settings now save in the background, while
  shutdown checkpoints still finish writing before exit. Closing a song
  list no longer waits for a pending page fetch, and Bluetooth monitor
  cleanup runs outside the UI thread.
- Web Import startup and Open Link/DLNA service changes now run in the
  background. Stopping Open Link or DLNA also interrupts idle client
  connections instead of waiting indefinitely for them to send data.
- Wi-Fi signal updates now run in the background instead of waiting on
  command-line tools on the UI thread. Read-only Wi-Fi queries use bounded
  timeouts, including a single elapsed-time budget for subprocess output
  and exit handling.
- Prepared a refresh of the firmware's Bluetooth audio and supporting
  libraries, led by BlueALSA 5.0.0. The player understands both the old and
  new Bluetooth tools, including their different volume-control formats.
- Updated ALSA, SBC, AAC, GLib, D-Bus, zlib and XML parsing libraries in the
  candidate base image. The device's kernel, hardware drivers and working
  LDAC libraries remain unchanged.
- Updated the external ALSA playback/recording and mixer tools to 1.2.16.
  These include `aplay`, which the player uses for Bluetooth and USB DAC
  output. Raw-PCM pipe tests pass on the R1 without changing mixer settings.
- Added compatibility with newer BlueZ paired-device commands and corrected
  retry handling when a Bluetooth software-volume command fails. Separate
  BlueZ and Wi-Fi upgrade candidates are being validated before inclusion.

