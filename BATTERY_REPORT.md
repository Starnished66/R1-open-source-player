# Battery Consumption Report

Date: 2026-08-19  
Device: HiBy R1  
Source data: `battery_profile.csv`

## Test coverage

- 49 samples at approximately one-minute intervals.
- 60 minutes 47 seconds between the first and last timestamps.
- One 13-minute 40-second sampling gap, consistent with the logger being
  suspended while the device slept or transitioned state.
- USB/AC power was disconnected for 48 of 49 samples.

## Important measurement limitation

The battery gauge recalibrated upward after USB was removed: it began at 62%,
rose gradually to 68% while voltage fell from 4.178 V to about 3.996 V, and
only then resumed normal downward movement. Therefore, the full-window
percentage change is not a valid consumption measurement. The kernel's
`time_to_empty_now` reading was also unstable (mostly 8191 seconds before
jumping to 1299 and 1515 seconds) and should not be used for runtime estimates.

The most reliable portion is the final uninterrupted 27-minute segment after
the gauge stabilized at 68%.

## Stable discharge segment

| Metric | Result |
|---|---:|
| Duration | 27 min 5 sec |
| Battery gauge | 68% → 67% |
| Voltage | 3.996 V → 3.964 V |
| Observed gauge rate | ~2.2 percentage points/hour |
| Player average CPU | ~3.5% of one CPU |
| Average system load | 1.26 |
| Average player RSS | 18.0 MB |
| Player RSS range | 17.2–18.9 MB |
| Screen-on samples | 6 of 28 |
| Bluetooth-on samples | 27 of 28 |
| Wi-Fi-on samples | 13 of 28 |

The observed 2.2 percentage-points/hour rate would arithmetically imply about
45 hours per 100 percentage points, but this is **not a defensible battery-life
claim**: the window is short, the gauge resolution is only 1%, and it followed
a large recalibration. A two-to-four-hour uninterrupted discharge run is needed
for a useful runtime estimate.

## Findings

1. **No player memory leak is visible.** RSS fluctuated within a narrow range
   and ended below its initial stable-segment value.
2. **Player CPU use was modest but continuous.** Roughly 3.5% of one CPU during
   the stable Bluetooth-heavy segment is low, although it leaves room to reduce
   periodic wakeups further.
3. **The display was mostly off.** Only 21% of stable samples had non-zero
   brightness, so this run primarily measures screen-off playback behavior.
4. **Bluetooth dominated the stable window.** It was enabled for 96% of stable
   samples. Wi-Fi was enabled for roughly 46%, including a continuous 13-minute
   interval. This is a mixed-radio workload rather than a clean local-playback
   baseline.
5. **Battery sysfs telemetry is inadequate for instantaneous power analysis.**
   The useful `battery` supply exposes voltage and capacity but no current;
   `axp_battery/current_now` is known to be frozen and cannot support wattage
   calculations.

## Recommended follow-up

Run separate uninterrupted tests lasting at least two hours each:

1. Local playback, screen off, Wi-Fi off, Bluetooth off.
2. Bluetooth playback, screen off, Wi-Fi off.
3. Local playback with representative screen-on brightness.

Start each run only after unplugging and waiting several minutes for the gauge
to settle. Record starting/ending capacity and voltage, and avoid rebooting or
suspending the logger during the window. These controlled runs will isolate the
cost of Bluetooth and the display and produce a meaningful battery-life range.
