#ifndef HOSTNAME_APPLY_H
#define HOSTNAME_APPLY_H

#include <stdbool.h>

/* Real-device bug report: the device's hostname and Bluetooth name are
 * baked into the read-only squashfs at /usr/resource/hostname and
 * /usr/resource/bt_name (build-time constants, no write access from this
 * app -- confirmed directly: `adb push` to anything under /usr/resource
 * fails with "Read-only file system"). wifi_on.sh reads the former for the
 * DHCP hostname it advertises, and bt_init reads the latter for the
 * Bluetooth adapter's Alias, both via a plain `cat` of the fixed path --
 * neither script is ours to edit either (same read-only rootfs).
 *
 * hostname_apply(hostname) works around this with a bind mount: writes
 * `hostname` into a real file on the writable /usr/data partition, then
 * `mount -o bind`s that file directly over BOTH /usr/resource/hostname and
 * /usr/resource/bt_name -- confirmed live on a real device that busybox's
 * mount supports file-to-file (not just directory) `-o bind`, and that
 * wifi_on.sh/bt_init's own plain `cat` of those paths transparently sees
 * the bind-mounted content instead, no changes needed to either script.
 * Also calls sethostname() for the kernel's own copy (affects the `hostname`
 * command and /proc/sys/kernel/hostname immediately, in this process and
 * system-wide).
 *
 * Must run once per boot, before anything else could read either path --
 * wifi_on.sh only runs when the user turns Wi-Fi on from Settings, and
 * bt_init only runs when the Bluetooth screen first opens (see bt_init's
 * own "not run automatically at boot" note in TESTING.md), so calling this
 * from gui_init() right after settings_load() -- long before the user can
 * reach either toggle -- is early enough. Both consumers only ever read
 * their file once per invocation, not on a live-reload path, which is why
 * changing Settings -> System -> Hostname takes a reboot to actually show
 * up, same as the real-device request that shipped this feature asked for
 * ("reboot and have it applied") rather than trying to force a live
 * re-apply into two scripts this app doesn't own.
 *
 * No-op (returns true immediately) if `hostname` is NULL or empty -- that's
 * how "use the stock name" is represented (player_settings_t.hostname
 * defaults to ""), not a special sentinel value. HOST_BUILD is a pure
 * no-op too (there's no real /usr/resource or /usr/data on a dev machine,
 * and definitely no bind-mounting over the host's own files). */
bool hostname_apply(const char * hostname);

#endif /* HOSTNAME_APPLY_H */
