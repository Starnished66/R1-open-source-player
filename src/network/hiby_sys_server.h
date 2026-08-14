#ifndef HIBY_SYS_SERVER_H
#define HIBY_SYS_SERVER_H

#include <stdbool.h>

/* Real-device finding (task #44, 2026-08-13): the stock firmware's own
 * hiby_player reports its playback state to a separate system service,
 * sys_server, over a plaintext line protocol on a Unix domain socket at
 * /var/run/sys_server -- confirmed live via strace against the stock
 * binary (BT:PLAYER_STATUS:playing/paused, BT:METADATA:title\tartist\t
 * album\tgenre\tlength_ms, BT:POSITION:ms, BT:ABSVOL:<mac> <level>, each a
 * one-shot connect+send+recv("OK")+close). sys_server relays these out as
 * standard D-Bus PropertiesChanged signals for any AVRCP-querying remote
 * (a car display, a headphone with a screen, etc). This has NOTHING to do
 * with AVRCP button *dispatch* -- see bt_media_player.c's own top-of-file
 * comment for that (a completely different, evdev-based mechanism) -- this
 * is purely outbound status reporting, matching stock's own behavior.
 *
 * Target-only: /var/run/sys_server only exists on real hardware. Callers
 * must guard with #ifndef HOST_BUILD, matching every other Bluetooth-only
 * code path in this codebase. Each call opens a short-lived connection and
 * is safe to call from any thread; failures (sys_server not running, or
 * this device isn't a HiBy build with sys_server at all) are silently
 * ignored -- this is a nice-to-have compatibility signal, not something
 * any of this app's own functionality depends on. */

void hiby_sys_server_report_playback_status(bool playing);

/* length_ms may be 0 if unknown (e.g. duration not read yet) -- sent as-is,
 * matching what was observed live (this app's own duration comes from
 * audio_get_duration_seconds(), not track_metadata_t). */
void hiby_sys_server_report_metadata(const char * title, const char * artist,
                                      const char * album, const char * genre, long length_ms);

void hiby_sys_server_report_position(long position_ms);

/* percent: 0-100, this app's own volume scale (audio_get_volume() * 100) --
 * converted internally to the 0-127 AVRCP absolute-volume range, matching
 * BT_SOURCE_VOLUME_MAX's own scale in bluetooth_control.c. No-ops if no
 * accessory is currently connected (bt_control_get_connected_device_mac()
 * fails), since BT:ABSVOL needs a target MAC address. */
void hiby_sys_server_report_volume(int percent);

#endif /* HIBY_SYS_SERVER_H */
