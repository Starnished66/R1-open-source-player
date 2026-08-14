#ifndef BLUETOOTH_CONTROL_H
#define BLUETOOTH_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char mac[18]; /* "XX:XX:XX:XX:XX:XX" + NUL */
    char name[64];
    bool paired;
    bool connected;
} bt_device_t;

/* bluetoothd, bluealsa, and a NoInputNoOutput pairing agent are already
 * running on this device; everything here just drives bluetoothctl's
 * non-interactive CLI mode (`bluetoothctl <command> [args]`), which
 * produces clean output with no ANSI/prompt junk to strip. */

bool bt_control_is_powered(void);

/* Deliberately no startup-time cleanup call here (there used to be one).
 * Running it during boot -- killing/respawning the D-Bus daemons that
 * S30dbus and S80_bt_init leave running, which other still-starting
 * init.d services depend on -- reliably hung the app before it reached
 * the main loop; skipping it entirely boots cleanly and reliably. The
 * D-Bus split-brain condition it addressed is instead handled reactively:
 * see ensure_single_dbus_daemon() further down, called only once a real
 * symptom (bluetoothctl hanging, an agent registered on the wrong bus) is
 * observed during normal use. */

/* Brings up the Bluetooth chip if it isn't already (no hci0 this boot) by
 * running the real firmware's own /usr/bin/bt_resume script -- the same
 * effective bring-up the stock hiby_player triggers on demand when its own
 * Bluetooth screen opens, since chip init isn't automatic at boot on this
 * firmware. No-op (returns true immediately) if hci0 already exists:
 * re-running chip bring-up without an intervening reboot reliably fails
 * ("Can't get device info"), confirmed on a real device.
 *
 * Uses bt_resume rather than the similar /usr/bin/bt_init: bt_init
 * unconditionally starts its own dbus-daemon with no check for one already
 * running (confirmed by reading the script directly), which would add a
 * THIRD one on top of the two S30dbus and S80_bt_init already leave running
 * every boot (see the comment above bt_control_init_chip's declaration
 * above) -- bt_resume's own dbus-daemon-starting
 * lines exist in the script but are commented out, so it just uses whatever
 * bus is already there instead.
 *
 * Slow (~10-13s: chip firmware flash plus several sleeps baked into the
 * script itself) -- always call this off the UI thread. */
bool bt_control_init_chip(void);

/* Each blocks for about a second (bluetoothctl's own controller-power round
 * trip) -- call off the UI thread, same as everything else here. */
void bt_control_enable(void);
void bt_control_disable(void);

/* Task #44 fix: kill + relaunch bluetoothd (adapter reset, power restore,
 * output profile reapply), same recipe the reactive wedge-recovery in
 * bt_control_is_powered() already uses, but called unconditionally rather
 * than only after a detected wedge symptom -- see bluetooth_control.c's
 * own comment on this function for the real-device diagnosis that led
 * here (a cold-boot-only AVRCP passthrough bug bluetoothctl-based wedge
 * detection never observes, since the daemon answers every other query
 * normally the whole time). Blocks for a few seconds (kill/reset/respawn/
 * re-enable, each its own subprocess call) -- NOT the "no startup-time
 * cleanup" case warned about just above bt_control_init_chip(): that
 * warning is specifically about running daemon cleanup before the main
 * loop starts; this is safe to call any time AFTER that, from any
 * thread that isn't the UI thread. bt_media_player.c's dispatch thread
 * (already running well past app startup by the time BT first connects)
 * is the intended caller, once, right before its own first real
 * RegisterPlayer this boot. */
void bt_control_restart_daemon(void);

/* True if any paired device currently has an active connection. Cheap
 * relative to bt_control_scan() (no discovery window): just `info` on each
 * already-paired device, same query bt_control_scan() already does per
 * device, without the scan step. Still forks one process per paired device,
 * so callers should poll this at a throttled cadence, not every frame. */
bool bt_control_is_connected(void);

/* Like bt_control_is_connected(), but returns the full per-device
 * paired/connected breakdown instead of collapsing it into a single bool --
 * see the .c file for why (avoids paying for the same per-device query loop
 * twice per poll cycle). Returns how many entries were written into out[]
 * (capped at max_count, same convention as bt_control_scan()), or -1 if the
 * underlying query itself failed -- see the .c file's own comment on why
 * that's kept distinct from a genuine empty list. Blocking; call off the
 * UI thread. */
int bt_control_list_paired_states(bt_device_t * out, int max_count);

/* True if a real A2DP-source PCM (this device -> a connected headphone/
 * speaker) is currently registered with bluealsa -- a stronger, audio-
 * specific signal than bt_control_is_connected()/bt_control_list_paired_states()
 * (those report ANY paired device with an active connection, not
 * necessarily one that actually supports/negotiated A2DP audio). Forks a
 * process (`bluealsa-cli list-pcms`); call off the UI thread. */
bool bt_control_is_a2dp_source_connected(void);

/* Writes the connected A2DP-source accessory's Bluetooth MAC address (e.g.
 * "DC:69:E2:99:43:06") into out, extracted from the same bluealsa PCM path
 * bt_control_is_a2dp_source_connected() already checks for
 * ("/org/bluealsa/hci0/dev_XX_XX_XX_XX_XX_XX/a2dpsrc/sink" -- underscores
 * swapped back to colons). Returns false (out left untouched) if nothing's
 * connected. Same subprocess cost as the other bt_control_* calls here;
 * call off the UI thread. */
bool bt_control_get_connected_device_mac(char * out, size_t out_size);

/* Writes the ACTUAL negotiated A2DP codec the connected accessory is
 * currently streaming with (e.g. "AAC", "LDAC", "SBC" -- whatever
 * `bluealsa-cli info` reports as "Selected codec") into out. This is the
 * real, live-negotiated codec, NOT current_settings.bt_codec (this app's
 * own PREFERRED codec setting, Settings > Bluetooth > Codec) -- those can
 * differ if the accessory doesn't support the preferred one and bluealsa
 * fell back to something else. Same subprocess cost as the other
 * bt_control_* calls here; call off the UI thread. Returns false (out left
 * untouched) if nothing's connected. */
bool bt_control_get_connected_device_codec(char * out, size_t out_size);

/* Blocking: scans for `seconds` (bluetoothctl's own --timeout), then reads
 * back the combined paired+discovered device list via `info` on each one
 * for Paired/Connected state. Call off the UI thread. Returns how many
 * devices were written into out[] (capped at max_count). */
int bt_control_scan(int seconds, bt_device_t * out, int max_count);

/* Pair (if not already) + trust + connect, in that order -- trusting first
 * means a future reconnect (e.g. after the device is turned off and back
 * on) won't need to go through this flow again, matching normal phone/OS
 * Bluetooth UX. The already-running NoInputNoOutput agent auto-accepts
 * "Just Works" pairing with no PIN prompt needed from this app. Blocking;
 * call off the UI thread. */
bool bt_control_connect(const char * mac);

bool bt_control_disconnect(const char * mac);

/* Unpairs and removes the saved device entry (bluetoothctl remove) --
 * disconnects it first if currently connected, as part of the same
 * command. Blocking; call off the UI thread. */
bool bt_control_forget(const char * mac);

/* Output settings, confirmed against the real firmware's bt_init script,
 * which launches bluealsa as `bluealsa -p a2dp-source --a2dp-volume` --
 * a2dp-source only (this device sending audio OUT to headphones/speakers),
 * with AVRCP absolute-volume sync on by default. Both settings below are
 * flags on that SAME bluealsa process, so changing either kills and
 * relaunches it with the full combination implied by both current values
 * (never just one flag in isolation) -- callers pass both, not just the
 * one that changed. This is a real running-service restart, briefly
 * interrupting any in-progress Bluetooth audio; blocking, call off the UI
 * thread.
 *
 * dac_mode_enabled adds the a2dp-sink profile (so another device can
 * stream audio TO this one, using it as an external DAC) and, when
 * turning it on, also starts bluealsa-aplay to actually route that
 * incoming audio to the hardware output and makes the adapter
 * discoverable+pairable so a phone can find and connect to it as a sink
 * target; turning it off stops bluealsa-aplay and discoverability again.
 * volume_sync_enabled maps directly to the --a2dp-volume flag. */
bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled);

/* Regenerates /usr/data/alsa.conf's bt_alsa_sink stanza -- the same
 * file/stanza the stock bt_init script creates once if missing (real
 * content read directly off a real device, default codec "ldac" /
 * ldac_eqmid "LDAC_ABR"). `codec` is one of "auto"/"ldac_hq"/"ldac_sq"/
 * "aptx"/"aac"/"sbc"; "auto" omits the codec line entirely so bluealsa
 * negotiates automatically instead of this file forcing one. LDAC_HQ/
 * LDAC_SQ are this project's best-effort mapping of "LDAC quality"/"LDAC
 * Standard" to LDAC's own quality-mode naming -- the stock script's only
 * confirmed real value is LDAC_ABR (adaptive), so these two specifically
 * need on-device confirmation that bluealsa's LDAC codec plugin actually
 * accepts them. Blocking (just a file write); call off the UI thread for
 * consistency with everything else here. */
bool bt_control_set_codec(const char * codec);

/* Keeps this app's own playback volume and a connected a2dp-source
 * accessory's (headphones/speaker this device streams TO) AVRCP volume in
 * sync, in both directions -- the headphones' own volume buttons update
 * this app's volume, and this app's own volume slider/hardware buttons
 * update whatever level the headphones show. See the .c file's own comment
 * above bt_control_source_volume_sync_start() for why this doesn't
 * introduce a second, compounding gain stage. Call start whenever
 * Bluetooth output is actually in use (mirror audio_set_bt_output()'s own
 * gating -- gui.c calls both together) and stop when it isn't; both are
 * cheap/safe to call repeatedly with the same effective state (idempotent,
 * matching every other start/stop pair in this file). */
void bt_control_source_volume_sync_start(void);
void bt_control_source_volume_sync_stop(void);

/* Fast disconnect detection for that same a2dp-source output PCM. Real-
 * device bug report: a genuine BT headphone disconnect took up to ~17s to
 * even be noticed (gui.c's own ~5s poll cadence plus its 12s debounce --
 * see that debounce's own comment for why it can't just be shortened; a
 * real A2DP renegotiation blip looks identical to a genuine drop over a
 * window that short). bluealsa itself knows the instant BlueZ tears the
 * transport down and broadcasts it over D-Bus -- `bluealsa-cli monitor`
 * (confirmed via `strings` on the real binary) surfaces that as a
 * "PCMRemoved <path>" line in well under a second. This is a fast-path
 * NOTIFICATION only, not a replacement for the polled/debounced check --
 * that one stays as the fallback in case this monitor subprocess dies or
 * bluealsa doesn't emit the signal for some reason.
 *
 * Same start/stop lifecycle convention as bt_control_source_volume_sync_start()/
 * _stop() just above -- start whenever Bluetooth output is actually in use,
 * stop when it isn't, both idempotent. bt_control_output_disconnect_consume()
 * is edge-triggered: true (and clears itself) the first poll after a real
 * removal was observed, false otherwise -- callers don't need to know the
 * PCM path themselves. */
void bt_control_output_disconnect_watch_start(void);
void bt_control_output_disconnect_watch_stop(void);
bool bt_control_output_disconnect_consume(void);

#endif /* BLUETOOTH_CONTROL_H */
