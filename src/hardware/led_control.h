#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <stdbool.h>

typedef enum {
    LED_CONTROL_RED = 0,
    LED_CONTROL_BLUE
} led_control_color_t;

typedef enum {
    LED_CONTROL_MODE_OFF = 0,
    LED_CONTROL_MODE_ON,
    LED_CONTROL_MODE_BLINK,
    LED_CONTROL_MODE_BREATHE,
    LED_CONTROL_MODE_STATUS
} led_control_mode_t;

/* R1 Pro charge-status LEDs (/sys/class/leds/{red,blue}). Normal charge
 * indication keeps its existing raw brightness of 50 for both colors and
 * only lights while connected to external power. */
void led_control_apply(bool enabled);
bool led_control_available(void);

/* Plugin override operations. User levels are 0..100 and are mapped to each
 * color's hardware-safe range in led_control.c. Effects use LVGL timers, so
 * these functions and their timer callbacks must run on the UI thread. */
void led_control_set_override(led_control_color_t color, int level);
bool led_control_blink(led_control_color_t color, int on_ms, int off_ms, int level);
bool led_control_breathe(led_control_color_t color, int period_ms, int level);
void led_control_set_status(led_control_color_t color);
void led_control_set_all_status(void);
void led_control_clear_override(void);
void led_control_suspend(void);
void led_control_resume(bool enabled);
led_control_mode_t led_control_get_mode(led_control_color_t color);
int led_control_get_level(led_control_color_t color);

/* Call on every control tick. Updates normal status colors and colors that a
 * plugin explicitly returned to status mode, while leaving other plugin
 * effects alone. */
void led_control_poll(bool enabled);

#endif /* LED_CONTROL_H */
