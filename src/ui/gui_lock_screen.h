#ifndef GUI_LOCK_SCREEN_H
#define GUI_LOCK_SCREEN_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

typedef enum {
    LOCK_SCREEN_MODE_OFF = 0,
    LOCK_SCREEN_MODE_ALBUM_ART,
    LOCK_SCREEN_MODE_IMAGE,
    LOCK_SCREEN_MODE_CLOCK,
} gui_lock_screen_mode_t;

typedef struct {
    gui_lock_screen_mode_t mode;
    char image_path[256];
    bool clock_24h;
} gui_lock_screen_options_t;

/* Returns the lock screen LVGL object, or NULL if not currently created. */
lv_obj_t * gui_lock_screen_get_screen(void);

/* Returns true if the lock screen is currently visible/active on screen. */
bool gui_lock_screen_is_showing(void);

/* Shows the lock screen with the specified options. Returns true on success. */
bool gui_lock_screen_show(const gui_lock_screen_options_t * options);

/* Hides the lock screen by popping it from the navigation stack. */
void gui_lock_screen_hide(void);

/* Lifecycle functions for gui_reload.c */
void gui_lock_screen_init(void);
void gui_lock_screen_teardown(void);

#endif /* GUI_LOCK_SCREEN_H */
