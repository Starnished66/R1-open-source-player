#ifndef GUI_H
#define GUI_H

#include "lvgl/lvgl.h"
#include <stdint.h>

/* Initialize the user interface elements and callbacks */
void gui_init(uint32_t screen_width, uint32_t screen_height);

/* Shows a minimal boot-settle splash screen -- call once, as early as
 * possible after display setup, before gui_init(). See gui.c's own
 * comment above the definition for why. Target-only: HOST_BUILD callers
 * don't have a real cold-boot race to guard against. */
#ifndef HOST_BUILD
void gui_show_boot_splash(void);
#endif

#endif /* GUI_H */
