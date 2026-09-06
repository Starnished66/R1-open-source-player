#ifndef BT_MEDIA_PLAYER_H
#define BT_MEDIA_PLAYER_H

#include <stdbool.h>

/* AVRCP transport-button and playback status support.
 * Uses a D-Bus MediaPlayer2 player object to answer AVRCP metadata/status
 * queries, and monitors the virtual evdev keyboard device created by BlueZ
 * ("<name> (AVRCP)") to receive remote button presses. */
void bt_media_player_init(void);

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
