#ifndef POWER_SUSPEND_H
#define POWER_SUSPEND_H

/* Real suspend-to-RAM, feature-matching stock -- the alternative to a full
 * idle_shutdown_now() poweroff, with the advantage of a near-instant resume
 * instead of a full reboot. Found by reading the stock hiby_player binary's
 * own strings: it wraps /sys/power/state with FB_BLANK_POWERDOWN (via its
 * own hgl_fb layer) before suspending and FB_BLANK_UNBLANK after, plus a
 * matched /usr/bin/bt_suspend + /usr/bin/bt_resume pair around the
 * Bluetooth chip (bt_resume is already used by bt_control_init_chip()).
 * Confirmed live on a real R1 Pro: without the fb blank/unblank sequencing
 * around it, /sys/power/state itself suspends and resumes fine at the
 * kernel level, but the jzfb display driver's own resume path is broken
 * (spins forever on "pan display wait timeout", leaving the screen black
 * with the backlight still on) -- doing the blank/unblank dance around it
 * avoids that broken path entirely instead of hitting and recovering from
 * it.
 *
 * Blocks until the device actually wakes back up -- the write to
 * /sys/power/state doesn't return until then, and the whole process
 * (indeed the whole system) is genuinely paused for that whole span, not
 * just this thread, so there's no reason to call this anywhere but
 * directly inline wherever the decision to suspend is made. WiFi/Bluetooth
 * are torn down before suspending (matching bt_suspend/wlan0 down, the
 * same real-device testing that found the fb blank fix) and restored
 * after -- but only if they were actually on right beforehand, same
 * "never force a radio back on the user had off" rule gui.c's own
 * radios_suspended logic already follows (in the common case, that same
 * logic has usually already suspended them by the time this fires, since
 * idle_shutdown_minutes is normally longer than RADIO_SUSPEND_DELAY_MS --
 * this just double-checks rather than assuming that always holds).
 *
 * The restore itself happens in a detached background thread, not inline
 * before this function returns -- real-device bug report: with Bluetooth
 * on, the inline version blocked the caller (the main GUI thread) for the
 * ~10-13s bt_control_init_chip() firmware reflash takes, leaving the
 * screen dark that whole time even though the kernel itself had already
 * resumed. Callers can treat this function as returning right after the
 * kernel wakes up; the radios catch up shortly after on their own. */
void power_suspend_now(void);

#endif /* POWER_SUSPEND_H */
