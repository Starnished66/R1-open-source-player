# On-device testing workflow

Read this before every real-device test session. It exists because skipping
steps here caused a full device freeze that needed a hard reboot (2026-07-30),
and because skipping step 1's "kill both together" detail caused a whole
session (2026-08-07) of the device auto-rebooting on every single test
launch, initially misread as a crash in the app under test.

## 1. Kill the stock player first, always -- BOTH processes, together

The stock firmware auto-launches its own player at boot via a wrapper script
(`hiby_player.sh` -> `hiby_player`). Two separate problems if you don't kill
both before deploying and launching a test binary:

- **Two players fighting over the same Bluetooth/audio/D-Bus state** -- the
  leading suspect for the 2026-07-30 freeze (a `bluetoothctl show` call hung
  forever with both players active, and since it's called from the UI
  thread's periodic poll, that hung the whole device).
- **An unwanted reboot** -- `hiby_player.sh`'s own logic is "run
  `/usr/bin/hiby_player`; when it exits for *any* reason, `sleep 1; reboot`".
  Killing only the child (`hiby_player`) leaves the wrapper script alive and
  still blocked waiting on it -- the instant the child dies, the wrapper
  proceeds straight to its own `sleep 1; reboot`, and that reboot lands
  right in the middle of (or just before) whatever you launch next. This
  looks *exactly* like your new test binary crashing the device, and cost a
  whole session of misdiagnosis (2026-08-07) before the actual cause (the
  *stock, already-proven-stable* binary rebooted the device identically
  under the same kill-then-launch sequence, which is what proved it wasn't
  the app under test) was found.

Kill both in **one** command, so there's no window for the wrapper to react
to the child dying before you get to it too:

```
adb shell "killall -9 hiby_player.sh hiby_player 2>/dev/null; sleep 1; ps | grep -iE 'hiby|player'"
```

Confirm the `ps` output shows neither `hiby_player.sh` nor `hiby_player`
still listed, **and** that `adb devices` still shows the device connected
afterward (a reboot in flight shows up as the device dropping off `adb
devices` for ~10-20s) before deploying/launching anything.

**Never kill `hiby_player` without also killing `hiby_player.sh` in the same
breath** -- killing the wrapper alone, or the child alone, both leave a
window for the reboot-on-exit behavior to fire.

## 2. Check the Bluetooth chip is actually initialized

`bt_init` (the script that loads Bluetooth chip firmware and starts
`bluetoothd`) is **not run automatically at boot** on this firmware -- the
stock player triggers it on demand when its own Bluetooth screen opens.
Since we killed the stock player in step 1, nothing else will do this for
us. Check first:

```
adb shell "hciconfig 2>&1"
```

If this prints nothing (no `hci0`), Bluetooth hasn't been brought up yet.
Bring it up **once**, via `setsid` so it survives past this adb shell
invocation ending:

```
adb shell "setsid /usr/bin/bt_init > /usr/data/bt_init.log 2>&1 & sleep 10; cat /usr/data/bt_init.log; hciconfig 2>&1"
```

Look for `UP RUNNING` in the `hciconfig` output. If `bt_init` instead prints
`Can't get device info: No such device`, the chip firmware flash
(`brcm_patchram_plus`, real UART flashing) failed.

**Do not just re-run `bt_init` to retry.** Re-running it back-to-back
without a real reboot in between has reliably failed in testing (confirmed
twice) -- the chip doesn't tolerate being re-patched while already in a
patched state. If it fails, the fix is a full device reboot, then run
`bt_init` exactly once from that fresh boot.

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

Push to `/usr/data/` (the writable partition) under a name that describes
what's being tested -- never overwrite `/usr/bin/hiby_player` itself for a
quick test (it's read-only squashfs anyway, and this is also how a stray
test binary once ballooned to 24MB of a 35.8MB partition and nearly filled
it, see step 6).

```
adb push open_hiby_player_target_stripped /usr/data/hiby_player_<tag>_test
adb shell "chmod +x /usr/data/hiby_player_<tag>_test"
```

Launch via the Bash tool's own `run_in_background: true` on a plain
`adb shell "/usr/data/hiby_player_<tag>_test"` (no `setsid`, no manual `&`,
no manual output redirection needed) -- this reliably keeps the remote
process alive for as long as needed and streams output back for `Read`ing,
which plain `&`/`setsid` backgrounding inside a single `adb shell` string
did not reliably survive on this device. After a few seconds, `Read` the
task's output file and check for `Entering main event loop...` (clean
start) vs. an error/crash. Separately confirm with `ps` that the process is
actually running and `adb devices` still shows the device connected.

## 5. Verify stability before handing back for interactive testing

Don't assume "it launched" means "it's fine" -- watch it idle for at least
30-60s and check memory isn't climbing unbounded:

```
adb shell "sleep 30; ps | grep hiby_player_<tag>_test | grep -v grep; cat /proc/<pid>/status | grep VmRSS; free -m"
```

A few MB of RSS growth right after launch is normal (the image cache
filling up to its ~4MB cap, LV_CACHE_DEF_SIZE in lv_conf.h). It should
plateau, not keep climbing. If `free -m`'s `available` column keeps
dropping every time you check, stop and investigate before doing anything
else -- don't interact with a device that's actively leaking
memory, it can freeze the whole thing (not just the app).

## 6. Watch `/usr/data` free space -- it's a 35.8MB partition

`df -h /usr/data` before pushing anything. Test binaries (~6MB stripped,
~24MB unstripped) accumulate fast if not cleaned up, and `killall hiby_player`
only matches that exact process name -- it will **not** kill a differently-
named test binary left over from an earlier session, so orphaned test
binaries silently pile up across sessions unless deleted by hand. An
unstripped leftover binary once ate 24MB of the 35.8MB partition by itself
(2026-08-07), and a near-full partition risks write failures on the next
push/settings-save.

Clean up your own test binaries and stray logs (`hiby_player_*_test`,
`*.log`, `test_log.txt`) when done with them, and run `sync` after any
push/delete you care about surviving a reboot -- this device's UBIFS
partition has been observed to lose recently-written metadata (e.g. a file
deletion) across an *unclean* shutdown/reboot if it hadn't committed yet,
which otherwise looks like your cleanup "undid itself" after a crash.

## 7. Restore the stock player when done testing

Don't leave a raw test binary running as the device's final state --
relaunch the real thing via its normal init.d path (not the raw binary) so
the device is left exactly as you would find it after a normal
boot:

```
adb shell "killall -9 hiby_player.sh hiby_player 2>/dev/null; sleep 1"
adb shell "/usr/bin/hiby_player.sh"
```

(Bash tool `run_in_background: true` again -- same reasoning as step 4.)
Confirm with `ps` that `hiby_player.sh` and `hiby_player` are both back and
running before considering the session done.

## 8. If ADB stops responding mid-session

`adb devices` still showing the device but every `adb shell` command
failing with `error: closed` means the **device** is in trouble, not the
adb client -- restarting `adb kill-server && adb start-server` will not
fix it. This is what a frozen device looks like over adb. Power cycle the
device in this case.

If `adb devices` shows nothing at all for more than ~20-30s after a kill/
launch command, that's consistent with an actual reboot in progress (not
just a hung shell) -- see step 1's reboot-on-exit trap first before assuming
something is wrong with the app under test. Re-enable ADB after reboot.
