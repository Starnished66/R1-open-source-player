#ifndef POWER_SUSPEND_H
#define POWER_SUSPEND_H

/* Initiates suspend-to-RAM via /sys/power/state. Wraps suspension with
 * framebuffer blanking (FB_BLANK_POWERDOWN / FB_BLANK_UNBLANK) to prevent
 * display driver timeouts on resume.
 *
 * Tears down active Wi-Fi and Bluetooth radios before suspend, and restores
 * previously active radios in a background thread upon waking so the GUI
 * can unblank the display immediately without waiting for radio re-initialization. */
void power_suspend_now(void);

#endif /* POWER_SUSPEND_H */
