# What's New

Changes since the September 15, 2026 release notes, for the September 21
weekly beta.

This update redesigns the quick drawer, returns Now Playing to three
centered title, artist, and album lines, and replaces the power-button
countdown with Power Off and Reboot. Bluetooth headphones stay connected
when a track's sample rate differs from the wireless link, with per-headset
sample-rate controls. Tracks tagged with more than one artist are filed
under each name, and USB remotes, gapless playback, and ADB placement
are improved. Large libraries also browse more smoothly, update more
quickly when files have not changed, and recover more safely from a damaged
database.

## A refreshed player and interface

- **Now Playing shows title, artist, and album as three centered lines.**
  The combined gray artist/album line is gone. The block sits above the
  cover from each line's own size, and tapping the cover for lyrics still
  moves all three beside the smaller artwork.
- **Player text no longer follows Font Size.** Title, artist, and album
  stay at one size per model so the layout does not collide with the cover
  when Display -> Font Size is increased.
- **A redesigned quick drawer.** Pull down for Wi-Fi, Bluetooth, Sleep, and
  Crossfade. Drag the handle, or anywhere above the now-playing card, to
  reveal AirPlay, DLNA, Gapless, and Remote, then brightness and volume.
  Installed plugins can add up to four extra tiles; Gain Mode and MSEB do
  so when those plugins are present. An upward drag collapses the extra
  rows, or closes the drawer when there is nothing to collapse.
- **The drawer card uses the frosted cover.** Title, artist, album, and
  the format badge are centered over it, matching Now Playing. Transport
  controls sit along the bottom. Long titles wait to start scrolling until
  the drawer is actually open.
- **The status bar stays visible while the drawer is open,** including
  clock and battery, even if Now Playing had hidden the top bar. The
  drawer also scales on the shorter R3 Pro II and R3 II 2025 screens.
- **Long-press power now offers Power Off and Reboot.** A frosted overlay
  replaces the three-second countdown. Tap an action immediately, or tap
  elsewhere to dismiss. Reboot and Factory Reset actually restart instead
  of powering the device off.
- **Submenus dropped their colored row backgrounds.** Music, Settings,
  Books, DAC, Wireless, and similar screens no longer draw a per-tile
  backdrop, which had been making those lists feel sluggish, especially
  on R3 Pro II. Home still uses its tile cards.
- The built-in screen background is black again when no custom theme is
  active. The About screen now identifies the app as **Compás Player**.
- Fixed a gesture conflict where swiping on the Now Playing seek bar could
  trigger the back-swipe navigation instead of adjusting playback position.
- Swiping a song list no longer jumps to a letter if the gesture drifts
  onto the A-Z strip. The strip only takes a drag that started on it.
- At the largest font size, the lyrics empty message wraps instead of
  running off the screen, and the idle Power Off / Suspend choices no
  longer overlap the timeout slider.
- Changing Font Size or a custom font now updates every already-built
  Settings screen, so a submenu you had not opened yet no longer shows
  titles scrolling the wrong way.
- Toasts grow with their message and wrap long text instead of overflowing
  a fixed box.
- The Home indicator no longer stays visible on a cold boot until you
  leave Home and come back.
- The screen turns on and off more quickly from the power button. When it
  is off, the panel is fully blanked, which saves a little battery.

## Playback and everyday controls

- **Gapless playback can be turned off.** It stays on by default, from
  Music Settings -> Playback and from the quick drawer. Turning it off
  also turns Crossfade off, because a crossfade needs the next track
  ready; turning Crossfade on turns Gapless back on.
- **USB remotes now work.** Playback buttons on a USB headphone cable or
  dongle control play/pause, next, previous, and volume. A remote can be
  plugged in after the player has already started.
- Damaged FLAC files no longer send the player into a retry loop that
  reboots the device. Bad frames are skipped where possible, closer to
  how a desktop player treats a still-playable file.
- Fixed a boot-time case where missing sound-device nodes caused every
  track to report "Playback error: audio output failed."
- Using the player as a USB DAC no longer lets queued audio pile up as
  extra delay during a long session. While USB DAC mode is active, a
  reset-to-Home action stays on that overlay instead of returning to the
  normal Home screen.

## Library browsing

- **Large folders open promptly in Files.** Folder contents appear in pages
  instead of creating every visible row at once. Previous and Next let you
  move through a folder with many entries.
- **SD card hot swaps unload the old library and queue.** The player detects
  a replacement card even when the old filesystem mount or device node
  lingers, then loads the new card's saved library and queue without another
  scan. A card with no database still gets its first scan.
- **Music database updates reuse unchanged files.** A manual Update Music
  Database or an automatic rescan checks for changes instead of doing the
  work of a first scan again. Initial scans also spend less time waiting
  after each track's metadata is read.
- **Safer database recovery and migration.** If a saved database is damaged,
  the player can recover an earlier valid copy and tells you when to run
  Update Music Database to save it. A missing SD card or failed database
  load no longer causes an automatic rebuild that could replace the saved
  library. Migrating an older database keeps favorites and play history;
  the old copy remains available if migration fails.
- **Very large queues no longer exhaust player memory on resume.** For a
  huge All Songs queue, the player remembers the last track without saving
  every queued path. The exact queue and shuffle order may not survive a
  restart in this case.
- **An optional offline first-scan tool** can build the music database on a
  computer for a large SD library, then copy the validated result to a card
  that has no existing database. It supports MP3, AAC, M4A, FLAC, WAV, and
  the player's other supported music formats; install Python's `mutagen`
  package to read their tags.
- **Tracks with more than one artist are filed under each name.** A tag
  such as "Artist A;Artist B" (semicolon or slash) appears under both in
  Artists. Album Artist is left as a single grouping. Opening an album
  from one artist shows only that artist's tracks, not every track on the
  album.
- **Folders can be kept out of the music database.** Put a
  **database.ignore** file in a folder to exclude it and everything under
  it. A nested **database.unignore** brings a subfolder back.
- A library scan no longer stops at the first unreadable file or folder,
  which could previously finish with an empty library. Songs are removed
  only when the file is actually gone, so a folder the scan could not
  read no longer loses its tracks. If the card is not mounted, the scan
  is skipped instead of recording an empty library. The completion
  message tells you when some folders could not be read, or that there
  is no SD card.

## Bluetooth, USB, and wireless

- **Headphones stay connected when a track's sample rate differs from
  the wireless link.** The player converts the audio instead of tearing
  down the connection. Bluetooth no longer restarts its audio service on
  every boot, which had been dropping a headset that connected first.
- **Bluetooth -> Advanced holds the less-used controls.** Codec, sample
  rate, volume sync, unnamed-device hiding, and Speex resampling live
  there so the device list stays the focus. Sample Rate lists what the
  connected headset actually supports. Automatic is 44.1 kHz, which
  matches most CD-derived libraries. Each accessory remembers its own
  rate. Changing it may briefly disconnect the headset; reconnect if it
  does not come back. The list then shows the rate that actually took.
- A connected headset's row shows the codec **and** the negotiated
  sample rate, so you can see which link you actually got.
- Opening Bluetooth while music is playing no longer runs a full
  discovery scan, which had been making playback stutter. The known-device
  list still refreshes; Rescan is there when you want discovery.
- Your codec preference is applied when headphones connect, instead of
  waiting until a later track change. Changing the codec still asks you
  to turn Bluetooth off and on.
- Bluetooth DAC mode is no longer restored after a restart, which had
  left headphones unable to connect with no overlay on screen to exit.
- **ADB moved to Developer Options** (Settings -> About). It stays on
  across a restart. Other USB modes return to Storage on boot, so a
  leftover DAC mode cannot block local playback. Storage and DAC stay
  dimmed on the USB Mode screen while ADB has the port.
- The drawer's Wi-Fi toggle no longer drops a second tap while the radio
  is still switching. A double tap settles on the last choice.
- Developer Options now says **Enable debug logging** (was database
  logging). Crash and reload diagnostics write only when that toggle is
  on.

## Plugins

- Plugins can put an on/off tile in the quick drawer. Gain Mode (High/Low)
  and MSEB appear there when those plugins are installed.
- The example plugin set adds the **Obsidian Audio** theme and three EQ
  profiles in Sound Profiles: Harman IE 2017, V Shape, and U Shape.

## Before updating

Speex resampling and the Bluetooth boot-script fix need the full firmware
package; replacing only the player executable will not install them.
Updated plugin features also require the corresponding plugin files.
