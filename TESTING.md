# On-device testing workflow

Read this before every real-device test session. It exists because skipping
steps here caused a full device freeze that needed a hard reboot (2026-07-30),
and because skipping step 1's "kill both together" detail caused a whole
session (2026-08-07) of the device auto-rebooting on every single test
launch, initially misread as a crash in the app under test.

## Remaining real-device regression tests

- [ ] **Volume/brightness slider drags no longer hijack the quick drawer:**
  press a hardware volume button to show the volume popup and drag its
  slider with deliberately imperfect (slightly diagonal) finger motion,
  confirm it tracks the finger smoothly end to end and the drawer never
  animates/peeks during the drag. Open the quick drawer and repeat with its
  brightness slider. The knob and displayed percentage must follow every
  touch sample. Volume and brightness hardware may follow by up to 50 ms;
  the drawer snapshot and settings persistence apply once on release. Confirm
  fast drags that leave the slider bounds cannot trigger Home, Back, Now
  Playing, or drawer open/close gestures before the finger is released. Confirm
  ordinary drawer open/close drags (started off any slider) and taps on
  drawer buttons/switches still work normally.

- [ ] **Repeated slider releases do not block the UI or lose settings:**
  drag and release each volume slider and the brightness slider repeatedly
  in quick succession. Confirm every re-grab responds immediately, wait for
  the final save to complete, reboot, and confirm the last values persist.

- [ ] **Network streams reject seeks without stopping:** play both a plain
  radio URL and a remote/Subsonic track with a catalog duration. Tap and
  drag the progress slider, trigger any available lyrics/remote-control seek,
  and confirm playback continues from the live stream without reconnecting,
  reporting a decoder error, or stopping. Repeat immediately after opening
  the stream to cover the pre-format-publication race.

- [ ] **Repeated progress-slider seeks across long tracks:** use at least
  three local, seekable songs longer than 30 minutes, preferably including
  a format that takes noticeable time to open or build its seek index. One
  test file must be a 90+ minute local MP3 audiobook: seek repeatedly both
  forward and backward. On initial resume, confirm no audio from the start of
  the file is emitted while the bounded lazy index builds; on a mid-track
  seek, confirm playback holds at the confirmed position instead of running
  on and jumping later. Confirm the latest requested position is applied with
  a smooth fade-in once ready. Press Stop and skip to
  another track while a fresh index is building; both actions must respond
  immediately. On separate runs, pause and simulate Car Mode external-power
  removal before the index completes, then restart and confirm the intended
  resume/seek target was preserved rather than being replaced with the old
  decoder position. Verify later seeks on that same track complete promptly rather
  than brute-force decoding from the beginning. A permanently invalid index
  must leave that file unseekable for the session rather than brute-force
  walking it on the playback thread. Play
  track 1 and tap the progress slider several times; immediately advance
  to track 2 and repeat, continuing through at least five tracks. Confirm
  every seek lands in the selected track at approximately the requested
  percentage, including the third seek, and that playback never continues
  unchanged from the pre-seek position. Repeat the full sequence several
  times and include taps made immediately after each track change, while
  its decoder may still be opening. Stress the exact reboot report by
  alternating rapid skips with several progress-bar taps for multiple
  minutes; confirm RSS stays bounded, the device does not reboot, and a
  subsequent normal reboot does not loop while resuming the last track.
  Also confirm the slider may temporarily
  resume normal polling after its UI wait timeout without canceling a slow
  underlying seek. Test more than one codec when suitable long files are
  available. This validates the playback-generation-bound percentage seek
  and the single-decoder, in-place seek path introduced for the reported
  failure and memory-exhaustion reboot on 30+ minute tracks.

- [ ] **Embedded-art thumbnail warm-up cost:** after clearing the generated
  thumbnail cache, scan a library containing many albums whose artwork exists
  only as embedded cover data. Compare the `ART_CACHE worker_end elapsed_ms`
  and `ART_LAZY decode_end elapsed_ms` diagnostics with a sidecar-art library.
  Confirm warm-up remains acceptably paced, playback suspends it, opening the
  Albums screen remains responsive, and malformed artwork is contained rather
  than rebooting the player. The warmer intentionally handles one representative
  song per album, stops after 512 albums, and yields between albums.
  Confirm a complete warm-up reaches `ART_CACHE worker_end`, only one
  `--metadata-artwork-helper` exists at a time, and a deliberately timed-out
  or killed helper retries after the temporary backoff instead of becoming a
  permanent missing-cover entry. Run a rescan afterward and verify helper
  failures cannot turn existing tags into Unknown Artist/Album rows.

## 1. Kill the stock player first, always -- BOTH processes, together

The stock firmware auto-launches its own player at boot via a wrapper script
(`hiby_player.sh` -> `open_hiby_player`). **The wrapper script's own name
never changed even after the app binary itself was renamed from
`hiby_player` to `open_hiby_player`** -- confirmed on-device by reading
`/usr/bin/hiby_player.sh` directly (2026-08-21): it still runs
`/usr/bin/open_hiby_player` in its foreground, so this doc (and the
`adb_device_testing` memory) went stale on the child process's name and
caused a repeat of the exact reboot-on-kill incident below. Two separate
problems if you don't kill both before deploying and launching a test
binary:

- **Two players fighting over the same Bluetooth/audio/D-Bus state** -- the
  leading suspect for the 2026-07-30 freeze (a `bluetoothctl show` call hung
  forever with both players active, and since it's called from the UI
  thread's periodic poll, that hung the whole device).
- **An unwanted reboot** -- `hiby_player.sh`'s own logic is "run
  `/usr/bin/open_hiby_player`; when it exits for *any* reason, `sleep 1;
  reboot`". Killing only the child (`open_hiby_player`) leaves the wrapper
  script alive and still blocked waiting on it -- the instant the child
  dies, the wrapper proceeds straight to its own `sleep 1; reboot`, and that
  reboot lands right in the middle of (or just before) whatever you launch
  next. This looks *exactly* like your new test binary crashing the device,
  and cost a whole session of misdiagnosis (2026-08-07) before the actual
  cause (the *stock, already-proven-stable* binary rebooted the device
  identically under the same kill-then-launch sequence, which is what
  proved it wasn't the app under test) was found -- and cost another real
  device reboot (2026-08-21) when this doc's own stale process name
  (`hiby_player` instead of `open_hiby_player`) was trusted without
  double-checking against the actual running process first.

Kill both in **one** command, so there's no window for the wrapper to react
to the child dying before you get to it too:

```
adb shell "killall -9 hiby_player.sh open_hiby_player 2>/dev/null; sleep 1; ps | grep -iE 'hiby|player'"
```

Confirm the `ps` output shows neither `hiby_player.sh` nor `open_hiby_player`
still listed, **and** that `adb devices` still shows the device connected
afterward (a reboot in flight shows up as the device dropping off `adb
devices` for ~10-20s) before deploying/launching anything.

**Never kill `open_hiby_player` without also killing `hiby_player.sh` in the
same breath** -- killing the wrapper alone, or the child alone, both leave a
window for the reboot-on-exit behavior to fire. If in doubt about either
process's current real name, `cat /usr/bin/hiby_player.sh` and cross-check
`ps` before trusting this doc's own examples.

## 2. Check the Bluetooth chip is actually initialized

`bt_init` (the script that loads Bluetooth chip firmware and starts
`bluetoothd` and `bluealsa`) **is run automatically at boot** on this
firmware. The executable `/etc/init.d/S80_bt_init` starts
`/usr/bin/bt_init` in the background before
`S92_03_start_music_player` launches `hiby_player.sh`; with the bootloader
installed, the resulting order is therefore `S80_bt_init` -> background
`bt_init` -> `S92_03_start_music_player` -> bootloader -> selected player.
The S80 script does not wait for `bt_init` to finish, so Bluetooth can still
be flashing firmware or starting its daemons while the bootloader and player
are starting. Open Player does not delay the rest of the UI for that job and
does not mask a queried value: Bluetooth status remains explicitly not ready,
with no `bluetoothctl` status subprocess launched, until `/tmp/bt_init_ok`
exists. The existing 500ms UI timer notices the marker and immediately starts
the first normal authoritative background refresh. Persisted Bluetooth-DAC
startup is held behind the same gate, while an early manual enable remains
queued by the existing pending-enable path.

After suspend-to-RAM with Bluetooth initially off, enable Bluetooth and then
tap it off again as soon as the adapter becomes usable. The second tap must be
accepted immediately (optimistic icon off) and executed as the queued final
state when the enable worker releases the chip mutex; it must not be ignored
for ~30 seconds. Repeat with several rapid taps and verify the final physical
adapter state matches the last requested UI state, with no on/off/on icon
bounce from an intermediate status refresh.

The bootloader supervises the selected player by exit status. A clean exit
(status 0, used by both players' `/sbin/poweroff` handoff) completes a real
power-off. A nonzero exit, signal, launch failure, or wait failure is treated
as a crash and reboots after one second. Device validation must cover both:
let the normal shutdown countdown finish and confirm the unit stays off, then
force-kill each player and confirm it still reboots.

Open Player no longer registers its AVRCP media player during early boot.
Registration starts only after the existing authoritative status poll confirms
an app-driven Bluetooth enable. On a fresh boot, confirm no AVRCP worker is
started while Bluetooth remains off; enable Bluetooth once, connect the
headphones, and verify Play/Pause, Next, Previous, metadata/status, disable/
re-enable, and a suspend/resume cycle. Repeat after tapping Bluetooth before
`/tmp/bt_init_ok` exists so the queued-enable path is covered, and with
Bluetooth DAC persisted on so its startup-enable path is covered.

Killing the player in step 1 does not stop or restart that boot-time job.
Check its completion marker and resulting processes before interpreting a
Bluetooth failure as a problem in the test player:

```
adb shell "ls -l /tmp/bt_init_ok 2>/dev/null; pidof bluetoothd bluealsa; hciconfig 2>&1"
```

`/tmp/bt_init_ok` is written by `bt_init` only after its UART firmware flash,
`bluetoothd`, pairing agent, and `bluealsa` setup have run. Its absence during
the first several seconds of boot can be normal because the script runs in
the background and has taken roughly 5-16 seconds on real boots. If the marker
appears, `hci0` exists, and the expected daemons are running, continue with
the test; Bluetooth is deliberately left powered off until the user enables
it in the UI.

**Do not manually run or re-run `bt_init` during the same boot.** Re-running
it after the automatic S80 launch has reliably failed in testing (confirmed
twice) because the chip does not tolerate being patched again while already
fully or partially initialized. If `/tmp/bt_init_ok` never appears, `hci0`
is absent, or initialization otherwise fails, capture the available boot log
and process state, then perform a full device reboot rather than retrying the
script in place.

## 3. Always tag test builds so the About screen proves what's running

```
make target TEST_BUILD_TAG=<short-descriptive-name>
```

The About screen shows "Alpha 1 (`<tag>`)" when `TEST_BUILD_TAG` is set, vs.
just a generic `BUILD_STAMP` timestamp when it isn't -- easy to glance past
and mistake for an earlier build. Skipping this once (2026-08-07) meant a
freshly-deployed test build's own version string didn't visibly change from
the previous one, even though the binary genuinely was new. Always pass a
tag that describes *what's being tested* (e.g. `timezone_region_city`, not
a generic `test1`), so a screenshot of the About screen alone tells you
which revision it came from.

## 4. Deploy and launch

For a bootloader-discovered SD update (rather than a temporary `/usr/data`
test launch), the exact destination is:

```
/data/mnt/sd_0/.open_hiby_player/open_hiby_player
```

`.open_hiby_player` is an existing application-data directory; copy the
binary *inside* it and never replace the directory itself. Push directly to
the final bootloader-visible filename:

```
adb push open_hiby_player_target /data/mnt/sd_0/.open_hiby_player/open_hiby_player
adb shell "chmod 755 /data/mnt/sd_0/.open_hiby_player/open_hiby_player && sync"
```

Verify the local and device SHA-256 hashes match before rebooting. The
bootloader scanner treats this exact executable path as the SD update build.

Push to `/usr/data/` (the writable partition) under a name that describes
what's being tested -- never try to overwrite `/usr/bin/open_hiby_player`
itself for a quick test (it's read-only squashfs, confirmed directly:
`adb push` to it fails with "Read-only file system" -- only a full repack +
recovery-mode flash ever replaces that file; this is also how a stray test
binary once ballooned to 24MB of a 35.8MB partition and nearly filled it,
see step 6).

```
adb push open_hiby_player_target_stripped /usr/data/open_hiby_player_test_<tag>
adb shell "chmod +x /usr/data/open_hiby_player_test_<tag>"
```

Launch via the Bash tool's own `run_in_background: true` on a plain
`adb shell "/usr/data/open_hiby_player_test_<tag>"` (no `setsid`, no manual `&`,
no manual output redirection needed) -- this reliably keeps the remote
process alive for as long as needed and streams output back for `Read`ing,
which plain `&`/`setsid` backgrounding inside a single `adb shell` string
did not reliably survive on this device. After a few seconds, `Read` the
task's output file and check for `Entering main event loop...` (clean
start) vs. an error/crash. Separately confirm with `ps` that the process is
actually running and `adb devices` still shows the device connected.

## 4a. Launching a test binary that must survive a physical USB unplug

Step 4's plain-foreground launch only stays alive because the adb *client*
stays connected the whole time (via the Bash tool's `run_in_background:
true`) -- the remote process is still a descendant of that one shell
session. If the test itself requires physically unplugging the device (e.g.
testing what happens when a suspend/idle timer fires while unplugged), that
launch method is the wrong tool: confirmed on a real device (2026-08-14)
that a real USB disconnect kills it every time, and that neither `setsid
PROG &` nor `nohup PROG &` nor a double-backgrounded subshell (`(setsid
PROG &) &`) survive it either, even though `setsid` alone genuinely does
give the process its own session (confirmed via `/proc/<pid>/stat`) --
session/process-group separation isn't what matters here; **parentage**
is. All of those leave the process a descendant of the adb shell's own
process tree, and something in that teardown path kills the whole tree,
session or no session, once the connection genuinely drops (this device has
no cgroups mounted, so it isn't a cgroup-based kill).

What actually works: `start-stop-daemon` (present at `/sbin/start-stop-daemon`,
a real double-forking daemonizer, not a shell trick) reparents the process
directly to init (confirmed `ppid=1` via `/proc/<pid>/stat`, vs. remaining a
child of the shell for every method above) -- genuinely outside the
adb-launched process tree, not just in a different session within it.

```
adb shell "start-stop-daemon -S -b -m -p /usr/data/hiby_test.pid -x /bin/sh -- -c 'exec /usr/data/open_hiby_player_test_<tag> > /usr/data/hiby_test.log 2>&1'"
```

- `-b` backgrounds and double-forks; `-m -p <pidfile>` records the real
  child's PID (needed since `-x /bin/sh` means the immediate exec target is
  the wrapper shell, not the player itself -- reading the pidfile is more
  reliable than grepping `ps` for the binary name).
- Wrapping in `/bin/sh -c 'exec ... > logfile 2>&1'` is necessary because
  `start-stop-daemon -x EXECUTABLE` execs the target directly with no shell
  involved, so there's no other way to redirect its stdout/stderr
  (DBG_LOG's output) to a file you can `cat` afterward.
- Verify real detachment before trusting it, don't just assume the recipe
  above is enough on faith: `cat /proc/$(cat /usr/data/hiby_test.pid)/stat
  | awk '{print $4}'` should print `1`.

Kill it the same way any other test binary is killed (`killall -9
open_hiby_player_test_<tag>`) -- `start-stop-daemon`'s own `-K` mode is only
needed if you want to match by pidfile/executable instead of by name.

## 5. Verify stability before handing back for interactive testing

Don't assume "it launched" means "it's fine" -- watch it idle for at least
30-60s and check memory isn't climbing unbounded:

```
adb shell "sleep 30; ps | grep open_hiby_player_test_<tag> | grep -v grep; cat /proc/<pid>/status | grep VmRSS; free -m"
```

A few MB of RSS growth right after launch is normal (the image cache
filling up to its ~4MB cap, LV_CACHE_DEF_SIZE in lv_conf.h). It should
plateau, not keep climbing. If `free -m`'s `available` column keeps
dropping every time you check, stop and investigate before doing anything
else -- don't let the user interact with a device that's actively leaking
memory, it can freeze the whole thing (not just the app).

## 6. Watch `/usr/data` free space -- it's a 35.8MB partition

`df -h /usr/data` before pushing anything. Test binaries (~6MB stripped,
~24MB unstripped) accumulate fast if not cleaned up, and `killall
open_hiby_player` only matches that exact process name -- it will **not**
kill a differently-named test binary left over from an earlier session, so
orphaned test binaries silently pile up across sessions unless deleted by
hand. An unstripped leftover binary once ate 24MB of the 35.8MB partition by
itself (2026-08-07), and a near-full partition risks write failures on the
next push/settings-save.

Clean up your own test binaries and stray logs (`open_hiby_player_test_*`,
`*.log`, `test_log.txt`) when done with them, and run `sync` after any
push/delete you care about surviving a reboot -- this device's UBIFS
partition has been observed to lose recently-written metadata (e.g. a file
deletion) across an *unclean* shutdown/reboot if it hadn't committed yet,
which otherwise looks like your cleanup "undid itself" after a crash.

## 7. Restore the stock player when done testing

Don't leave a raw test binary running as the device's final state --
relaunch the real thing via its normal init.d path (not the raw binary) so
the device is left exactly as a real user would find it after a normal
boot:

```
adb shell "killall -9 hiby_player.sh open_hiby_player 2>/dev/null; sleep 1"
adb shell "/usr/bin/hiby_player.sh"
```

(Bash tool `run_in_background: true` again -- same reasoning as step 4.)
Confirm with `ps` that `hiby_player.sh` and `hiby_player` are both back and
running before considering the session done.

## 8. If ADB stops responding mid-session

`adb devices` still showing the device but every `adb shell` command
failing with `error: closed` means the **device** is in trouble, not the
adb client -- restarting `adb kill-server && adb start-server` will not
fix it. This is what a frozen device looks like over adb. Ask the user to
check the physical screen and power-cycle if needed.

If `adb devices` shows nothing at all for more than ~20-30s after a kill/
launch command, that's consistent with an actual reboot in progress (not
just a hung shell) -- see step 1's reboot-on-exit trap first before assuming
something is wrong with the app under test.

**Never run `adb kill-server` (or the kill-server/nodaemon/start-server
reconnect dance) to try to recover a dropped connection on this project** --
confirmed directly by the user (2026-08-21): it forces *them* to manually
reconnect the physical device every time, and it cannot fix anything on the
device side anyway (see above -- the local host daemon has no bearing on
the device's own USB/reboot state). Just plain-poll `adb devices -l` and
wait; if it hasn't come back after a reasonable wait, report that plainly
and ask the user to check the physical device/cable rather than retrying
blindly.
