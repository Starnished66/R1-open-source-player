#ifndef IDLE_SHUTDOWN_H
#define IDLE_SHUTDOWN_H

/* Full poweroff, the same mechanism the stock firmware's own "Idle
 * shutdown" setting uses (confirmed via `strings` on the stock binary:
 * /sbin/poweroff, driven by its power_save_shutdown_timer). Real
 * suspend-to-RAM exists at the kernel level on this hardware but its
 * display/WiFi resume path is broken (confirmed on a real device: the jzfb
 * driver spins forever on "pan display wait timeout" after resume, leaving
 * the screen blank while the backlight stays on), so a full poweroff is the
 * only reliable way to actually cut standby power draw -- see gui.c's
 * idle-shutdown timer for the inactivity gating (screen off, not playing,
 * not charging) before this gets called. Replaces this process with
 * busybox `poweroff` (then the reboot(RB_POWER_OFF) syscall if exec
 * fails) so it cannot return on the device. Must not use subprocess_run():
 * that helper SIGKILLs its child after 15s, which cancelled poweroff and
 * left the countdown looking finished while the device stayed on. No-op
 * on host. */
void idle_shutdown_now(void);

/* Same mechanism as idle_shutdown_now() (busybox first, raw reboot()
 * syscall as fallback, never returns on the device) but for a normal
 * restart instead of a full poweroff -- same /sbin/reboot + RB_AUTOBOOT
 * pattern settings.c's factory reset already uses. No-op on host. */
void idle_shutdown_reboot_now(void);

#endif /* IDLE_SHUTDOWN_H */
