#ifndef BT_MEDIA_PLAYER_H
#define BT_MEDIA_PLAYER_H

#include <stdbool.h>

/* AVRCP transport-button support (headphones' own play/pause/next/previous
 * controlling this app) -- see bt_media_player.c's own top-of-file comment
 * for the full story on why this needs a real D-Bus service (registering
 * an org.mpris.MediaPlayer2.Player object with BlueZ) rather than the
 * one-shot bluetoothctl/dbus-send/bluealsa-cli calls everything else in
 * bluetooth_control.c uses.
 *
 * Target-only (needs the vendored libdbus, see Makefile's DBUS_DIR
 * section) -- callers must guard with #ifndef HOST_BUILD, matching every
 * other Bluetooth-output-only code path in this codebase (e.g.
 * audio_set_bt_output()'s own callers in gui.c). */

/* Connects to the system bus, registers this app's D-Bus object, and
 * starts the background dispatch thread that runs for the app's whole
 * lifetime (same shape as audio_init(): call once from main.c, safe to
 * call more than once, only the first call does anything). Does NOT yet
 * tell BlueZ about the player -- see bt_media_player_set_active() below --
 * so this is safe to call unconditionally at startup regardless of
 * whether Bluetooth is even on yet. */
void bt_media_player_init(void);

/* Registers/unregisters this app as BlueZ's active media player
 * (org.bluez.Media1.RegisterPlayer/UnregisterPlayer on the hci0 adapter).
 * Call whenever Bluetooth output is actually in use, mirroring
 * audio_set_bt_output()'s own gating exactly -- gui.c calls both
 * together. Cheap to call repeatedly with the same value (no-ops if
 * already in the requested state). */
void bt_media_player_set_active(bool active);

/* Tells BlueZ (and therefore the connected accessory, if it displays
 * play/pause state) this app's current playback state -- call whenever
 * audio_is_playing() might have changed (play/pause/track change/stop),
 * same "state changed, push it out" shape as everything else that
 * mirrors playback state into the UI. No-op if not currently registered
 * as the active player. */
void bt_media_player_notify_playback_state(bool playing);

/* Each returns true (and clears the flag) exactly once when the connected
 * accessory's own play/pause/next/previous button was pressed since the
 * last check -- meant to be polled from the GUI thread's own periodic
 * timer (update_timer_cb), the exact same "background thread sets a flag,
 * the one thread allowed to touch LVGL widgets consumes it" pattern
 * hw_buttons_consume_play_pause()/_next()/_prev() already use for this
 * device's own physical buttons, since the D-Bus dispatch thread below
 * must not call into gui.c/LVGL directly. */
bool bt_media_player_consume_play_pause(void);
bool bt_media_player_consume_next(void);
bool bt_media_player_consume_prev(void);

#endif /* BT_MEDIA_PLAYER_H */
