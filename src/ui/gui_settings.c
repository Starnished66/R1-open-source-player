#include "gui.h"
#include "gui_settings.h"
#include "gui_text_input.h"
#include <math.h>
#include <sys/stat.h>
#include "gui_subsonic.h"
#include "gui_books.h"
#include "gui_network.h"
#include "subprocess.h"
#include "timezone_apply.h"
#include "gui_lyrics.h"
#include "gui_lyrics.h"
#include "screen_builders.h"
#include "settings.h"
#include "assets.h"
#include "metadata.h"
#include "audio.h"
#include "peq.h"
#include "device_config.h"
#include "usb_mode_control.h"
#include "timezone_data.h"
#include "firmware_update.h"
#include "plugin_manager.h"
#include <stdio.h>
#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PEQ_PROFILES_DIR MUSIC_ROOT_DIR "/PEQ_Profiles"
#include <stdatomic.h>
#include <pthread.h>

/* Extern references to screen pointers owned by this module (defined here) */
static lv_obj_t * settings_crossfade_toggle_img = NULL;
static lv_obj_t * settings_screen;
static lv_obj_t * settings_playback_screen;
static lv_obj_t * settings_display_screen;
static lv_obj_t * settings_power_screen;
static lv_obj_t * settings_system_screen;
static lv_obj_t * about_screen;
static lv_obj_t * accent_color_screen;
static lv_obj_t * screen_timeout_screen;
static lv_obj_t * startup_volume_screen;
static lv_obj_t * sleep_timer_screen;
static lv_obj_t * idle_shutdown_screen;
static lv_obj_t * eq_screen;
static lv_obj_t * eq_profiles_screen;

/* Externs to gui.c functions and state this module needs */
extern lv_obj_t * gui_library_get_music_screen();
extern lv_obj_t * stream_media_screen;
extern lv_obj_t * gui_network_get_wireless_screen();
extern lv_obj_t * gui_network_get_usb_dac_overlay();
extern lv_obj_t * gui_shell_get_dac_home_screen();
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode, lv_obj_t ** out_title, const char * body_text, const char * confirm_text, lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row, const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb, lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);
extern lv_color_t accent_lv_color(void);
extern lv_obj_t * add_pill_chevron_row(lv_obj_t * list, const char * text, lv_event_cb_t cb);
extern lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
extern lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text);
extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon);
extern void generic_back_cb(lv_event_t * e);
extern void start_library_rescan(void);

static lv_obj_t * screen_timeout_switch;
static lv_obj_t * screen_timeout_slider_card;
static lv_obj_t * screen_timeout_value_label;
static lv_obj_t * screen_timeout_slider;
static lv_obj_t * startup_volume_switch;
static lv_obj_t * startup_volume_slider_card;
static lv_obj_t * startup_volume_value_label;
static lv_obj_t * startup_volume_slider;
static lv_obj_t * sleep_timer_value_label;
static lv_obj_t * sleep_timer_slider;
static lv_obj_t * idle_shutdown_switch;
static lv_obj_t * idle_shutdown_slider_card;
static lv_obj_t * idle_shutdown_value_label;
static lv_obj_t * idle_shutdown_slider;
static lv_obj_t * idle_action_section;
static lv_obj_t * idle_action_poweroff_row;
static lv_obj_t * idle_action_suspend_row;
static lv_obj_t * eq_bypass_switch;
static lv_obj_t * eq_band_dropdown;
static lv_obj_t * eq_band_enabled_switch;
static lv_obj_t * eq_preamp_slider;
static lv_obj_t * eq_preamp_value_label;
static lv_obj_t * eq_freq_slider;
static lv_obj_t * eq_gain_slider;
static lv_obj_t * eq_q_slider;
static lv_obj_t * eq_type_dropdown;
static lv_obj_t * eq_freq_value_label;
static lv_obj_t * eq_gain_value_label;
static lv_obj_t * eq_q_value_label;
static int current_eq_band = 0;

#define EQ_FREQ_MIN_HZ 20.0
#define EQ_FREQ_MAX_HZ 20000.0
#define EQ_FREQ_SLIDER_MAX 1000

static int32_t freq_to_slider(double freq_hz) {
    if (freq_hz < EQ_FREQ_MIN_HZ) freq_hz = EQ_FREQ_MIN_HZ;
    if (freq_hz > EQ_FREQ_MAX_HZ) freq_hz = EQ_FREQ_MAX_HZ;
    double ratio = log(freq_hz / EQ_FREQ_MIN_HZ) / log(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ);
    return (int32_t) (ratio * EQ_FREQ_SLIDER_MAX);
}

static double slider_to_freq(int32_t slider_val) {
    double ratio = (double) slider_val / (double) EQ_FREQ_SLIDER_MAX;
    return EQ_FREQ_MIN_HZ * pow(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ, ratio);
}

/* Which numeric field a tap-to-edit label represents -- passed through
 * show_text_entry()'s user_data so one shared done-callback can update the
 * right slider/label/peq setter instead of needing four near-identical
 * callbacks. */
typedef enum {
    EQ_FIELD_PREAMP,
    EQ_FIELD_FREQ,
    EQ_FIELD_GAIN,
    EQ_FIELD_Q,
} eq_field_t;

static void eq_numeric_entry_done_cb(const char * text, void * user_data) {
    eq_field_t field = (eq_field_t) (intptr_t) user_data;
    double val = atof(text);
    const peq_band_t * band;

    switch (field) {
        case EQ_FIELD_PREAMP:
            if (val < -12.0) val = -12.0;
            if (val > 12.0) val = 12.0;
            peq_set_preamp_db(val);
            lv_slider_set_value(eq_preamp_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            lv_label_set_text_fmt(eq_preamp_value_label, "Pre-Amp: %+.2f dB", val);
            peq_save();
            break;
        case EQ_FIELD_FREQ:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < EQ_FREQ_MIN_HZ) val = EQ_FREQ_MIN_HZ;
            if (val > EQ_FREQ_MAX_HZ) val = EQ_FREQ_MAX_HZ;
            peq_set_band(current_eq_band, val, band->gain_db, band->q);
            lv_slider_set_value(eq_freq_slider, freq_to_slider(val), LV_ANIM_OFF);
            lv_label_set_text_fmt(eq_freq_value_label, "Frequency: %.0f Hz", val);
            peq_save();
            break;
        case EQ_FIELD_GAIN:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < -12.0) val = -12.0;
            if (val > 12.0) val = 12.0;
            peq_set_band(current_eq_band, band->freq_hz, val, band->q);
            lv_slider_set_value(eq_gain_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            lv_label_set_text_fmt(eq_gain_value_label, "Gain: %+.2f dB", val);
            peq_save();
            break;
        case EQ_FIELD_Q:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < 0.1) val = 0.1;
            if (val > 10.0) val = 10.0;
            peq_set_band(current_eq_band, band->freq_hz, band->gain_db, val);
            lv_slider_set_value(eq_q_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            lv_label_set_text_fmt(eq_q_value_label, "Q: %.2f", val);
            peq_save();
            break;
    }
}

/* Tap-to-edit: attached to each slider card's value label (see
 * create_eq_slider_card()) -- opens the shared numeric keypad pre-filled
 * with the field's current exact value, for typing a precise number
 * instead of only dragging a slider. */
static void eq_field_label_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_field_t field = (eq_field_t) (intptr_t) lv_event_get_user_data(e);
    const peq_band_t * band;
    char buf[32];
    const char * title;

    switch (field) {
        case EQ_FIELD_PREAMP:
            title = "Pre-Amp (dB, -12 to 12)";
            snprintf(buf, sizeof(buf), "%.2f", peq_get_preamp_db());
            break;
        case EQ_FIELD_FREQ:
            title = "Frequency (Hz, 20 to 20000)";
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.0f", band ? band->freq_hz : 0.0);
            break;
        case EQ_FIELD_GAIN:
            title = "Gain (dB, -12 to 12)";
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.2f", band ? band->gain_db : 0.0);
            break;
        case EQ_FIELD_Q:
        default:
            title = "Q (0.1 to 10)";
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.2f", band ? band->q : 0.0);
            break;
    }

    show_text_entry(title, buf, false, true, eq_numeric_entry_done_cb, (void *) (intptr_t) field);
}

static void eq_screen_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(eq_screen);
}

static void eq_bypass_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Switch shows "EQ Enabled" -- checked means NOT bypassed. */
    peq_set_bypass(!lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    peq_save();
}

static void refresh_eq_band_widgets(void) {
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;

    lv_dropdown_set_selected(eq_band_dropdown, (uint32_t) current_eq_band);

    if (band->enabled) lv_obj_add_state(eq_band_enabled_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(eq_band_enabled_switch, LV_STATE_CHECKED);

    lv_dropdown_set_selected(eq_type_dropdown, (uint32_t) band->type);

    lv_slider_set_value(eq_freq_slider, freq_to_slider(band->freq_hz), LV_ANIM_OFF);
    lv_slider_set_value(eq_gain_slider, (int32_t) (band->gain_db * 10.0), LV_ANIM_OFF);
    lv_slider_set_value(eq_q_slider, (int32_t) (band->q * 10.0), LV_ANIM_OFF);

    lv_label_set_text_fmt(eq_freq_value_label, "Frequency: %.0f Hz", band->freq_hz);
    lv_label_set_text_fmt(eq_gain_value_label, "Gain: %+.2f dB", band->gain_db);
    lv_label_set_text_fmt(eq_q_value_label, "Q: %.2f", band->q);
}

static void eq_band_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_eq_band = (int) lv_dropdown_get_selected(lv_event_get_target(e));
    refresh_eq_band_widgets();
}

static void eq_type_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    peq_set_band_type(current_eq_band, (peq_band_type_t) lv_dropdown_get_selected(lv_event_get_target(e)));
    peq_save();
}

static void eq_preamp_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    double db = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_preamp_db(db);
        lv_label_set_text_fmt(eq_preamp_value_label, "Pre-Amp: %+.2f dB", db);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_band_enabled_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    peq_set_band_enabled(current_eq_band, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    peq_save();
}

static void eq_freq_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double freq = slider_to_freq(lv_slider_get_value(lv_event_get_target(e)));

    /* Real-device bug report: this used to switch to a shorter, unlabeled
     * "228 Hz" format while actively dragging, only showing the full
     * "Frequency: 228 Hz" once released -- read as the label "hiding" its
     * name mid-drag. Same full format at all times now, matching
     * refresh_eq_band_widgets()'s own resting-state format exactly. */
    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, freq, band->gain_db, band->q);
        lv_label_set_text_fmt(eq_freq_value_label, "Frequency: %.0f Hz", freq);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_gain_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double gain = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, band->freq_hz, gain, band->q);
        lv_label_set_text_fmt(eq_gain_value_label, "Gain: %+.2f dB", gain);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_q_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double q = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, band->freq_hz, band->gain_db, q);
        lv_label_set_text_fmt(eq_q_value_label, "Q: %.2f", q);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static lv_obj_t * firmware_update_popup;
static lv_obj_t * firmware_update_popup_backdrop;
static lv_obj_t * firmware_update_popup_title;

static void hide_firmware_update_popup(void) {
    lv_obj_add_flag(firmware_update_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(firmware_update_popup, LV_OBJ_FLAG_HIDDEN);
}

static void firmware_update_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
}

static void firmware_update_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
}

static void firmware_update_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
    firmware_update_enter_recovery();
}

static void build_firmware_update_popup(void) {
    firmware_update_popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &firmware_update_popup_title, NULL, "Update & Reboot", lv_color_make(255, 120, 120),
        firmware_update_confirm_cb, NULL, "Cancel", accent_lv_color(), firmware_update_cancel_cb, NULL,
        firmware_update_popup_backdrop_cb, &firmware_update_popup_backdrop);
}

void firmware_update_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    char path[512];
    if (!firmware_update_scan(path, sizeof(path))) {
        show_error_toast("No .upt firmware file found on SD card");
        return;
    }

    const char * filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    lv_label_set_text_fmt(firmware_update_popup_title, "Update using %s?\nDevice will reboot into recovery mode.",
                           filename);

    lv_obj_remove_flag(firmware_update_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(firmware_update_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(firmware_update_popup_backdrop);
    lv_obj_move_foreground(firmware_update_popup);
}

static lv_obj_t * build_about_screen(void) {
    static pill_list_item_t items[3];
    /* TEST_BUILD_TAG: optional -D from the Makefile (see TEST_BUILD_TAG
     * there), e.g. `make target TEST_BUILD_TAG=test25_avrcp` -- a
     * friendly label for which of this session's many test builds is
     * actually running, without needing to check timestamps/MD5s by
     * hand. BUILD_STAMP is the fallback: always defined automatically by
     * the Makefile (a build date/time, no flag needed), for the flashed/
     * real-deployment path where TEST_BUILD_TAG is never explicitly set
     * but the same "which build is this" question still matters. */
    static char version_line[64];
#if defined(TEST_BUILD_TAG)
    snprintf(version_line, sizeof(version_line), "Beta 1 (%s)", TEST_BUILD_TAG);
#elif defined(BUILD_STAMP)
    snprintf(version_line, sizeof(version_line), "Beta 1 (%s)", BUILD_STAMP);
#else
    snprintf(version_line, sizeof(version_line), "Beta 1");
#endif
    items[0] = (pill_list_item_t){ "Open Source Player for HiBy OS", PILL_ACCESSORY_NONE, false, NULL, NULL, NULL };
    items[1] = (pill_list_item_t){ version_line, PILL_ACCESSORY_NONE, false, NULL, NULL, NULL };
    items[2] =
        (pill_list_item_t){ "Firmware Update", PILL_ACCESSORY_CHEVRON, false, firmware_update_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen("About", generic_back_cb, items, 3, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_accent_color_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Accent Color");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    /* Real-device feedback: the swatches were too small to comfortably tap
     * and the 8-color palette felt limited -- bumped from 36px to 64px
     * (matching this screen's own back_btn hitbox size) and doubled the
     * palette to 16. That no longer fits one 50px row, so this container
     * grows to LV_SIZE_CONTENT height and wraps across as many rows as it
     * needs -- pad_row/pad_column give the bigger circles even breathing
     * room in both directions, matching SPACE_EVENLY's own horizontal
     * distribution instead of just relying on it alone. */
    lv_obj_t * swatch_row = lv_obj_create(scr);
    lv_obj_set_size(swatch_row, lv_pct(92), LV_SIZE_CONTENT);
    lv_obj_align(swatch_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(swatch_row, 0, 0);
    lv_obj_set_style_border_width(swatch_row, 0, 0);
    lv_obj_remove_flag(swatch_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(swatch_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(swatch_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(swatch_row, 20, 0);
    lv_obj_set_style_pad_column(swatch_row, 16, 0);

    for (size_t i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        lv_obj_t * swatch = lv_obj_create(swatch_row);
        lv_obj_set_size(swatch, 64, 64);
        lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(accent_palette[i]), 0);
        lv_obj_set_style_border_width(swatch, current_settings.accent_color == accent_palette[i] ? 4 : 0, 0);
        lv_obj_set_style_border_color(swatch, lv_color_make(255, 255, 255), 0);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch, accent_swatch_event_cb, LV_EVENT_CLICKED, (void *) (intptr_t) accent_palette[i]);
        gui_theme_register_accent_swatch(i, swatch);
    }

    finalize_screen_navigation(scr);
    return scr;
}

static void accent_color_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(accent_color_screen);
}

/* "45s" below 1 minute, "1m"/"2m" on an exact minute, "1m 30s" otherwise --
 * matches the 30s-10min range (SCREEN_TIMEOUT_MIN/MAX_SECONDS) without ever
 * needing more than whole minutes+seconds. */
static void format_screen_timeout(char * buf, size_t buf_size, int seconds) {
    if (seconds < 60) {
        snprintf(buf, buf_size, "%ds", seconds);
        return;
    }
    int minutes = seconds / 60;
    int remainder = seconds % 60;
    if (remainder == 0) snprintf(buf, buf_size, "%dm", minutes);
    else snprintf(buf, buf_size, "%dm %ds", minutes, remainder);
}

/* Index of the closest shared timeout preset. */
static int screen_timeout_seconds_to_step_index(int seconds) {
    int best = 0;
    int best_diff = abs(seconds - SCREEN_TIMEOUT_STEPS[0]);
    for (int i = 1; i < SCREEN_TIMEOUT_STEP_COUNT; i++) {
        int diff = abs(seconds - SCREEN_TIMEOUT_STEPS[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

static void screen_timeout_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_timeout_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.screen_timeout_enabled) {
        lv_obj_remove_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Same VALUE_CHANGED-updates-live/RELEASED-persists split as the EQ sliders
 * (eq_preamp_slider_event_cb etc.) -- writing settings.c's file on every
 * drag tick would be needless disk I/O for a value that only matters once
 * the user lets go. The slider itself moves over step indices (0..
 * SCREEN_TIMEOUT_STEP_COUNT-1), mapped onto the shared preset table. */
static void screen_timeout_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int seconds = SCREEN_TIMEOUT_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.screen_timeout_seconds = seconds;
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), seconds);
        lv_label_set_text(screen_timeout_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_screen_timeout_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Screen Timeout");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    /* Real-device bug report: at bigger system text sizes, "Turn off screen
     * automatically" overlapped the switch -- the row was a fixed 50px-tall
     * box with both children absolute-positioned (label pinned LEFT_MID,
     * switch pinned RIGHT_MID), so nothing reserved space for the switch or
     * let the label wrap when its rendered width grew with the font. Same
     * fix as eq_band_enabled_switch's own row in build_eq_screen(): flex row
     * + SPACE_BETWEEN with the label given flex_grow so it wraps within
     * whatever width is left after the switch instead of running under it,
     * and SIZE_CONTENT height so a wrapped 2nd line still fits. The card
     * below is repositioned relative to this row's actual (now variable)
     * bottom edge rather than a hardcoded offset, so it can't end up
     * overlapping a taller wrapped row either. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Turn off screen automatically");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    screen_timeout_switch = lv_switch_create(enable_row);
    lv_obj_add_style(screen_timeout_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.screen_timeout_enabled) lv_obj_add_state(screen_timeout_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(screen_timeout_switch, screen_timeout_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Reuses the EQ card look (rounded dark card, live numeric readout +
     * slider, style_theme_card_bg -- the same shared "card surface" style
     * every popup/EQ-card/slider-card in the app uses) for a consistent
     * "settings with a live readout" feel. Taller than the EQ card (170 vs 82),
     * with the slider near the top and the value label centered well below
     * it (rather than the EQ card's label-top-left/slider-along-the-bottom
     * layout) -- real-device feedback: the original thin-track/small-text/
     * top-left-label version was hard to reliably grab, and a later
     * bottom-anchored label still visually overlapped the slider's knob
     * (its default theme style draws it noticeably larger than the 36px
     * track height it's centered on), needing extra clearance below the
     * slider rather than just under it. */
    screen_timeout_slider_card = lv_obj_create(scr);
    lv_obj_set_size(screen_timeout_slider_card, lv_pct(90), 170);
    lv_obj_align_to(screen_timeout_slider_card, enable_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_add_style(screen_timeout_slider_card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(screen_timeout_slider_card, 0, 0);
    lv_obj_set_style_radius(screen_timeout_slider_card, 10, 0);
    if (!current_settings.screen_timeout_enabled) lv_obj_add_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);

    screen_timeout_slider = lv_slider_create(screen_timeout_slider_card);
    lv_obj_set_width(screen_timeout_slider, lv_pct(94));
    lv_obj_set_height(screen_timeout_slider, 36);
    lv_obj_align(screen_timeout_slider, LV_ALIGN_TOP_MID, 0, 18);
    lv_slider_set_range(screen_timeout_slider, 0, SCREEN_TIMEOUT_STEP_COUNT - 1);
    lv_slider_set_value(screen_timeout_slider, screen_timeout_seconds_to_step_index(current_settings.screen_timeout_seconds), LV_ANIM_OFF);
    lv_obj_add_style(screen_timeout_slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(screen_timeout_slider, gui_theme_accent_style(), LV_PART_KNOB);
    lv_obj_add_event_cb(screen_timeout_slider, screen_timeout_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(screen_timeout_slider, 20);

    screen_timeout_value_label = lv_label_create(screen_timeout_slider_card);
    lv_obj_add_style(screen_timeout_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(screen_timeout_value_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align(screen_timeout_value_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    char initial_buf[32];
    format_screen_timeout(initial_buf, sizeof(initial_buf), current_settings.screen_timeout_seconds);
    lv_label_set_text(screen_timeout_value_label, initial_buf);

    finalize_screen_navigation(scr);
    /* Unlike every other plain container on this screen, this card's own
     * background must NOT bubble drags up as an app-wide swipe:
     * finalize_screen_navigation() -> enable_gesture_bubble_recursive() just
     * gave it LV_OBJ_FLAG_GESTURE_BUBBLE like any other non-slider
     * container (blank space on any screen is meant to swipe-navigate).
     * Real-device feedback: a drag anywhere in this particular card that
     * missed the (thin, bottom-aligned) slider track landed on that
     * background instead and bubbled to screen_gesture_event_cb() as a
     * swipe (down = jump to player, up = back) rather than moving the
     * slider. Removing the flag here, after it was added, makes any drag
     * starting in this card go nowhere instead of misfiring navigation,
     * regardless of whether it lands on the slider or the card around it.
     * Also registered as a player-swipe dead zone (register_swipe_dead_zone()'s
     * own comment) -- that's a separate raw-polling path this
     * GESTURE_BUBBLE removal doesn't reach. */
    lv_obj_remove_flag(screen_timeout_slider_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(screen_timeout_slider_card);
    return scr;
}

static void screen_timeout_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(screen_timeout_screen);
}

static void startup_volume_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.startup_volume_fixed_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.startup_volume_fixed_enabled) {
        lv_obj_remove_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Same VALUE_CHANGED-updates-live/RELEASED-persists split as
 * screen_timeout_slider_event_cb -- this only sets what the NEXT launch
 * starts at, not the current session's live volume, so there's no reason
 * to call audio_set_volume() here at all. */
static void startup_volume_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.startup_volume_fixed_percent = (int) percent;
        lv_label_set_text_fmt(startup_volume_value_label, "%d%%", (int) percent);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

/* Mirrors build_screen_timeout_screen()'s own layout (toggle row + a
 * dark card holding a live-readout slider, shown/hidden with the toggle) --
 * see that function's comments for the reasoning behind the specific
 * measurements reused here. */
static lv_obj_t * build_startup_volume_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Startup Volume");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    /* Same real-device overlap bug/fix as build_screen_timeout_screen()'s
     * own enable_row -- see its comment. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Launch at a fixed volume");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    startup_volume_switch = lv_switch_create(enable_row);
    lv_obj_add_style(startup_volume_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.startup_volume_fixed_enabled) lv_obj_add_state(startup_volume_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(startup_volume_switch, startup_volume_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    startup_volume_slider_card = lv_obj_create(scr);
    lv_obj_set_size(startup_volume_slider_card, lv_pct(90), 170);
    lv_obj_align_to(startup_volume_slider_card, enable_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_add_style(startup_volume_slider_card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(startup_volume_slider_card, 0, 0);
    lv_obj_set_style_radius(startup_volume_slider_card, 10, 0);
    if (!current_settings.startup_volume_fixed_enabled) lv_obj_add_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);

    startup_volume_slider = lv_slider_create(startup_volume_slider_card);
    lv_obj_set_width(startup_volume_slider, lv_pct(94));
    lv_obj_set_height(startup_volume_slider, 36);
    lv_obj_align(startup_volume_slider, LV_ALIGN_TOP_MID, 0, 18);
    lv_slider_set_range(startup_volume_slider, 0, 100);
    lv_slider_set_value(startup_volume_slider, current_settings.startup_volume_fixed_percent, LV_ANIM_OFF);
    lv_obj_add_style(startup_volume_slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(startup_volume_slider, gui_theme_accent_style(), LV_PART_KNOB);
    lv_obj_add_event_cb(startup_volume_slider, startup_volume_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(startup_volume_slider, 20);

    startup_volume_value_label = lv_label_create(startup_volume_slider_card);
    lv_obj_add_style(startup_volume_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(startup_volume_value_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align(startup_volume_value_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text_fmt(startup_volume_value_label, "%d%%", current_settings.startup_volume_fixed_percent);

    finalize_screen_navigation(scr);
    /* Same reasoning as screen_timeout_slider_card's identical line: don't
     * let a drag that misses the slider bubble up as an app-wide swipe.
     * Also registered as a player-swipe dead zone -- see
     * register_swipe_dead_zone()'s own comment. */
    lv_obj_remove_flag(startup_volume_slider_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(startup_volume_slider_card);
    return scr;
}

static void startup_volume_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(startup_volume_screen);
}

/* Index into SLEEP_TIMER_STEPS closest to `minutes' -- same reasoning as
 * screen_timeout_seconds_to_step_index() above. */
static int sleep_timer_minutes_to_step_index(int minutes) {
    int best = 0;
    int best_diff = abs(minutes - SLEEP_TIMER_STEPS[0]);
    for (int i = 1; i < SLEEP_TIMER_STEP_COUNT; i++) {
        int diff = abs(minutes - SLEEP_TIMER_STEPS[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

/* This only sets the duration the quick-drawer sleep icon uses NEXT time
 * it's armed -- unlike screen_timeout_slider_event_cb's sibling, there's no
 * enable toggle here and no live countdown to interrupt, since arming
 * itself only ever happens from the drawer icon (quick_drawer_sleep_event_cb),
 * never from this screen. */
static void sleep_timer_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = SLEEP_TIMER_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.sleep_timer_minutes = minutes;
        lv_label_set_text_fmt(sleep_timer_value_label, "%d min", minutes);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

/* Mirrors build_screen_timeout_screen()'s layout minus the enable toggle
 * (a plain duration picker -- see sleep_timer_slider_event_cb's own
 * comment for why arming doesn't belong on this screen). */
static lv_obj_t * build_sleep_timer_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Sleep Timer");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    lv_obj_t * slider_card = lv_obj_create(scr);
    lv_obj_set_size(slider_card, lv_pct(90), 170);
    lv_obj_align(slider_card, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_add_style(slider_card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(slider_card, 0, 0);
    lv_obj_set_style_radius(slider_card, 10, 0);

    sleep_timer_slider = lv_slider_create(slider_card);
    lv_obj_set_width(sleep_timer_slider, lv_pct(94));
    lv_obj_set_height(sleep_timer_slider, 36);
    lv_obj_align(sleep_timer_slider, LV_ALIGN_TOP_MID, 0, 18);
    lv_slider_set_range(sleep_timer_slider, 0, SLEEP_TIMER_STEP_COUNT - 1);
    lv_slider_set_value(sleep_timer_slider, sleep_timer_minutes_to_step_index(current_settings.sleep_timer_minutes), LV_ANIM_OFF);
    lv_obj_add_style(sleep_timer_slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(sleep_timer_slider, gui_theme_accent_style(), LV_PART_KNOB);
    lv_obj_add_event_cb(sleep_timer_slider, sleep_timer_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(sleep_timer_slider, 20);

    sleep_timer_value_label = lv_label_create(slider_card);
    lv_obj_add_style(sleep_timer_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(sleep_timer_value_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align(sleep_timer_value_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text_fmt(sleep_timer_value_label, "%d min", current_settings.sleep_timer_minutes);

    finalize_screen_navigation(scr);
    /* Same reasoning as screen_timeout_slider_card's identical line: don't
     * let a drag that misses the slider bubble up as an app-wide swipe.
     * Also registered as a player-swipe dead zone -- see
     * register_swipe_dead_zone()'s own comment. */
    lv_obj_remove_flag(slider_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(slider_card);
    return scr;
}

static void sleep_timer_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(sleep_timer_screen);
}

/* All IDLE_SHUTDOWN_STEPS values are whole minutes, so unlike
 * format_screen_timeout() this never needs a seconds remainder -- "10m" /
 * "1h" / "2h", not "1h 30m", since 90 isn't one of the steps. */
static void format_idle_shutdown(char * buf, size_t buf_size, int minutes) {
    if (minutes < 60) {
        snprintf(buf, buf_size, "%dm", minutes);
        return;
    }
    snprintf(buf, buf_size, "%dh", minutes / 60);
}

static int idle_shutdown_minutes_to_step_index(int minutes) {
    int best = 0;
    int best_diff = abs(minutes - IDLE_SHUTDOWN_STEPS[0]);
    for (int i = 1; i < IDLE_SHUTDOWN_STEP_COUNT; i++) {
        int diff = abs(minutes - IDLE_SHUTDOWN_STEPS[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

static void idle_shutdown_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.idle_shutdown_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.idle_shutdown_enabled) {
        lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Mutually-exclusive idle-action choice (Power Off vs Suspend to RAM) --
 * replaces the old single "Suspend instead of power off" switch, which
 * modeled this same 2-way choice as a toggle acting on an implicit
 * opposite (plain poweroff being "off"). Same accent-border-highlight
 * pill-row selection language as populate_usb_mode_screen()'s own Storage/
 * USB DAC rows (add_pill_row_base()), but inline on this same screen
 * rather than a separate list screen, since it stays right above the
 * slider it's a sibling setting of. */
static void idle_action_choice_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool suspend = (bool) (intptr_t) lv_event_get_user_data(e);
    current_settings.idle_suspend_enabled = suspend;
    settings_save(&current_settings);
    lv_obj_set_style_border_width(idle_action_poweroff_row, suspend ? 0 : 3, 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, suspend ? 3 : 0, 0);
}

static void idle_shutdown_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = IDLE_SHUTDOWN_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.idle_shutdown_minutes = minutes;
        char buf[32];
        format_idle_shutdown(buf, sizeof(buf), minutes);
        lv_label_set_text(idle_shutdown_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_idle_shutdown_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Idle Shutdown");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    /* Same real-device overlap bug/fix as build_screen_timeout_screen()'s
     * own enable_row -- see its comment. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Automatically go idle");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    idle_shutdown_switch = lv_switch_create(enable_row);
    lv_obj_add_style(idle_shutdown_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.idle_shutdown_enabled) lv_obj_add_state(idle_shutdown_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(idle_shutdown_switch, idle_shutdown_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Mutually-exclusive idle-action choice, ahead of the duration slider
     * below (real-device feedback: "just like the USB mode selector, 2
     * choices with the slider below") -- full poweroff (idle_shutdown_now(),
     * the default, better-tested path) or real suspend-to-RAM
     * (power_suspend_now(), quick resume instead of a reboot). Shown/hidden
     * together with the slider card, toggled together in
     * idle_shutdown_switch_event_cb(). */
    /* lv_pct(100), not (90) -- add_pill_row_base()'s rows are a fixed
     * 448px wide (same asset/size as build_subsonic_list_screen()'s own
     * Storage/USB DAC/Font Size rows, which live inside that builder's
     * full lv_pct(100)-wide list). A narrower lv_pct(90) parent (~432px on
     * this 480px-wide display) clipped them -- real-device feedback: "not
     * rendered inside the screen". Left at the default theme "card" pad_all
     * (not zeroed) for the same reason: that padding is what insets the
     * full-width row snugly within its parent on every other screen that
     * hosts these same rows, rather than each row filling flush edge-to-edge.
     * pad_top/pad_bottom are zeroed here though -- left at PAD_DEF, the
     * section's own children (label + 2 rows, using hardcoded y offsets
     * that already assume a zero top pad) sit that much lower than the
     * section's fixed 292px height accounts for, so the bottom row's
     * true bottom edge exceeds the section's own bounds and gets clipped
     * there -- real-device feedback: "lower rectangle is not rendered
     * completely, cut off". Zeroing only top/bottom (not left/right)
     * keeps the width fit from the fix above while eliminating the
     * vertical overflow. */
    idle_action_section = lv_obj_create(scr);
    lv_obj_set_size(idle_action_section, lv_pct(100), 292);
    /* Relative to enable_row's actual (now variable, see its own comment)
     * bottom edge rather than a hardcoded offset from the title, so a
     * wrapped 2nd line in enable_row at bigger text sizes can't push this
     * section up into overlapping it. */
    lv_obj_align_to(idle_action_section, enable_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_set_style_bg_opa(idle_action_section, 0, 0);
    lv_obj_set_style_border_width(idle_action_section, 0, 0);
    lv_obj_set_style_pad_top(idle_action_section, 0, 0);
    lv_obj_set_style_pad_bottom(idle_action_section, 0, 0);
    lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_SCROLLABLE);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_action_explain_label = lv_label_create(idle_action_section);
    lv_label_set_text(idle_action_explain_label, "Choose what happens when idle:");
    lv_obj_add_style(idle_action_explain_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_action_explain_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_action_explain_label, LV_ALIGN_TOP_LEFT, 0, 0);

    idle_action_poweroff_row = add_pill_row_base(idle_action_section, "Power Off");
    lv_obj_align(idle_action_poweroff_row, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_border_color(idle_action_poweroff_row, accent_lv_color(), 0);
    lv_obj_set_style_border_width(idle_action_poweroff_row, current_settings.idle_suspend_enabled ? 0 : 3, 0);
    lv_obj_add_flag(idle_action_poweroff_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(idle_action_poweroff_row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) false);

    idle_action_suspend_row = add_pill_row_base(idle_action_section, "Suspend to RAM");
    lv_obj_align(idle_action_suspend_row, LV_ALIGN_TOP_MID, 0, 34 + 124 + 10);
    lv_obj_set_style_border_color(idle_action_suspend_row, accent_lv_color(), 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, current_settings.idle_suspend_enabled ? 3 : 0, 0);
    lv_obj_add_flag(idle_action_suspend_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(idle_action_suspend_row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) true);

    /* Same card/slider look as the screen-timeout screen above, for
     * consistency between the two "enable switch + discrete-step slider"
     * settings screens. Positioned below idle_action_section now, not
     * directly under the enable switch -- same real-device feedback as
     * that section's own comment. idle_action_section now zeroes its own
     * pad_top/pad_bottom (see that section's comment), so its declared
     * 292px height is exact and a plain +20 gap is enough.
     * 30px taller than screen_timeout_slider_card (200 vs 170) to fit an
     * explanatory caption above the slider -- real-device feedback: once
     * idle_action_section (with its own "Choose what happens when idle:"
     * caption) sat between the enable switch and this slider, what the
     * slider itself actually controls was no longer obvious just from the
     * screen title. */
    idle_shutdown_slider_card = lv_obj_create(scr);
    lv_obj_set_size(idle_shutdown_slider_card, lv_pct(90), 200);
    /* Relative to idle_action_section's own (now possibly-shifted, see its
     * comment) position rather than a hardcoded offset from the title. */
    lv_obj_align_to(idle_shutdown_slider_card, idle_action_section, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_add_style(idle_shutdown_slider_card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(idle_shutdown_slider_card, 0, 0);
    lv_obj_set_style_radius(idle_shutdown_slider_card, 10, 0);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_shutdown_slider_caption = lv_label_create(idle_shutdown_slider_card);
    lv_label_set_text(idle_shutdown_slider_caption, "Idle timeout:");
    lv_obj_add_style(idle_shutdown_slider_caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_shutdown_slider_caption, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_shutdown_slider_caption, LV_ALIGN_TOP_LEFT, 24, 14);

    idle_shutdown_slider = lv_slider_create(idle_shutdown_slider_card);
    lv_obj_set_width(idle_shutdown_slider, lv_pct(94));
    lv_obj_set_height(idle_shutdown_slider, 36);
    lv_obj_align(idle_shutdown_slider, LV_ALIGN_TOP_MID, 0, 48);
    lv_slider_set_range(idle_shutdown_slider, 0, IDLE_SHUTDOWN_STEP_COUNT - 1);
    lv_slider_set_value(idle_shutdown_slider, idle_shutdown_minutes_to_step_index(current_settings.idle_shutdown_minutes), LV_ANIM_OFF);
    lv_obj_add_style(idle_shutdown_slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(idle_shutdown_slider, gui_theme_accent_style(), LV_PART_KNOB);
    lv_obj_add_event_cb(idle_shutdown_slider, idle_shutdown_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(idle_shutdown_slider, 20);

    idle_shutdown_value_label = lv_label_create(idle_shutdown_slider_card);
    lv_obj_add_style(idle_shutdown_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(idle_shutdown_value_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align(idle_shutdown_value_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    char initial_buf[32];
    format_idle_shutdown(initial_buf, sizeof(initial_buf), current_settings.idle_shutdown_minutes);
    lv_label_set_text(idle_shutdown_value_label, initial_buf);

    finalize_screen_navigation(scr);
    /* Same reasoning as screen_timeout_slider_card's identical line: don't
     * let a drag that misses the slider bubble up as an app-wide swipe.
     * Also registered as a player-swipe dead zone -- see
     * register_swipe_dead_zone()'s own comment (this is the exact card
     * the real-device report that led to that function came from). */
    lv_obj_remove_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(idle_shutdown_slider_card);
    return scr;
}

static void idle_shutdown_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(idle_shutdown_screen);
}

/* ---- Time Zone picker ----------------------------------------------------
 * Region -> City, matching the stock firmware's own selection flow (per
 * user direction: "just a region -> country selector that sets up the
 * current time" -- the hardware's RTC already ships with a reasonable
 * date/time, confirmed live, so there's no separate manual date/time entry
 * here, only the zone). TIMEZONE_TABLE (timezone_data.h, ~370 entries) is
 * generated offline from the stock firmware's own city-name list,
 * cross-checked against this device's real /usr/share/zoneinfo tree -- see
 * timezone_data.c's own header comment; it's already sorted by UTC offset
 * then city name, which stays a sensible order even after filtering down to
 * one region (most regions span a limited, roughly west-to-east offset
 * range). Region names below are exactly the top-level IANA region
 * components actually present in TIMEZONE_TABLE (confirmed by inspection),
 * not a hand-typed guess. */
static const char * const TIMEZONE_REGIONS[] = {
    "Africa", "America", "Antarctica", "Arctic", "Asia", "Atlantic", "Australia", "Europe", "Indian", "Pacific"
};
#define TIMEZONE_REGION_COUNT (sizeof(TIMEZONE_REGIONS) / sizeof(TIMEZONE_REGIONS[0]))

static lv_obj_t * timezone_region_screen;
static lv_obj_t * timezone_city_screen;
/* Maps a City-screen row index (as passed to timezone_city_row_click_cb)
 * back to its real TIMEZONE_TABLE index -- the City screen only ever shows
 * one region's subset, so row index != table index. */
static int timezone_city_indices[TIMEZONE_TABLE_COUNT];
static int timezone_city_count;

static void timezone_city_row_click_cb(int row_index) {
    if (row_index < 0 || row_index >= timezone_city_count) return;
    const timezone_entry_t * entry = &TIMEZONE_TABLE[timezone_city_indices[row_index]];
    timezone_apply(entry->iana_id);
    snprintf(current_settings.timezone, sizeof(current_settings.timezone), "%s", entry->iana_id);
    settings_save(&current_settings);
    refresh_clock_label(); /* topbar clock reflects the new zone immediately, not just on the next periodic refresh */
    nav_pop(); /* City -> Region */
    nav_pop(); /* Region -> Settings -- picking a leaf city is a completed action, not a place to linger browsing further */
}

/* Rebuilt (delete + rebuild, same idiom as gui_library_get_all_songs_screen() after a library
 * rescan -- see poll_library_rescan()) every time a region is opened,
 * filtered down to just that region's entries, rather than built once --
 * needed both because the region changes on every open and so the
 * "(current)" marker always reflects current_settings.timezone as of the
 * most recent selection. Cheap even at TIMEZONE_TABLE_COUNT entries since
 * build_compact_list_screen is virtualized (screen_builders.h) -- only a
 * small pool of row widgets actually gets created, not one per entry.
 * `labels` is this function's own long-lived backing array for the
 * "(current)"-suffixed copy build_compact_list_screen's own doc comment
 * requires (label pointers must outlive the screen, not just this call) --
 * previous generation's entries are freed up to prev_count (the region
 * filter means the count varies per open, unlike a fixed-size table) right
 * before this rebuild, once the previous screen holding those pointers has
 * already been deleted by the caller. */
static void build_timezone_city_screen_items(compact_list_item_t * items, const char * region) {
    static char * labels[TIMEZONE_TABLE_COUNT];
    static int prev_count = 0;
    for (int i = 0; i < prev_count; i++) {
        free(labels[i]);
        labels[i] = NULL;
    }

    size_t region_len = strlen(region);
    timezone_city_count = 0;
    for (int i = 0; i < TIMEZONE_TABLE_COUNT; i++) {
        const char * id = TIMEZONE_TABLE[i].iana_id;
        if (strncmp(id, region, region_len) != 0 || id[region_len] != '/') continue;

        int row = timezone_city_count;
        bool is_current = strcmp(current_settings.timezone, id) == 0;
        if (is_current) {
            size_t len = strlen(TIMEZONE_TABLE[i].display_name) + 16;
            labels[row] = malloc(len);
            snprintf(labels[row], len, "%s (current)", TIMEZONE_TABLE[i].display_name);
        } else {
            labels[row] = strdup(TIMEZONE_TABLE[i].display_name);
        }
        timezone_city_indices[row] = i;
        items[row] = (compact_list_item_t){ labels[row] };
        timezone_city_count++;
    }
    prev_count = timezone_city_count;
}

static lv_obj_t * build_timezone_city_screen(const char * region) {
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) TIMEZONE_TABLE_COUNT);
    build_timezone_city_screen_items(items, region);
    lv_obj_t * scr = build_compact_list_screen(region, generic_back_cb, items, timezone_city_count, timezone_city_row_click_cb, NULL, NULL, NULL, LIST_ROW_WIDTH, false, lv_color_black());
    free(items);
    finalize_screen_navigation(scr);
    return scr;
}

static void open_timezone_city_screen(const char * region) {
    if (timezone_city_screen) lv_obj_delete(timezone_city_screen);
    timezone_city_screen = build_timezone_city_screen(region);
    nav_push(timezone_city_screen);
}

static void timezone_region_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    open_timezone_city_screen(TIMEZONE_REGIONS[index]);
}

/* Small (10 rows) and fixed -- built once at startup like every other
 * screen, unlike the per-region City screen above. */
static lv_obj_t * build_timezone_region_screen(void) {
    static pill_list_item_t items[TIMEZONE_REGION_COUNT];
    for (size_t i = 0; i < TIMEZONE_REGION_COUNT; i++) {
        items[i] = (pill_list_item_t){ TIMEZONE_REGIONS[i], PILL_ACCESSORY_CHEVRON, false, timezone_region_row_cb, NULL,
                                        (void *) (intptr_t) i };
    }
    lv_obj_t * scr = build_pill_list_screen("Time Zone", generic_back_cb, items, (int) TIMEZONE_REGION_COUNT, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

static void timezone_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(timezone_region_screen);
}

/* Defined later, alongside its confirmation popup (build_factory_reset_popup(),
 * right after build_eq_reset_popup() -- same hand-built top-layer overlay
 * shape). */
static void factory_reset_btn_cb(lv_event_t * e);

/* Alphabetical by label -- real-device feedback: the previous ad-hoc
 * ordering (roughly "added in this order") made a specific setting hard
 * to find in a 17-row list. Keep new entries sorted in too rather than
 * appended at the end. */
/* ---- Settings category sub-screens -- the flat 16-item System list grew
 * unwieldy enough (real-device feedback) that it's now a menu of 4 category
 * screens plus About, rather than one long scroll. Each sub-screen reuses
 * the exact same items/callbacks the old flat list had, just grouped. ---- */

/* Shared click handler for every plugin-registered Playback list row below
 * -- same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_playback_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_playback_list_item_clicked(index);
}

static lv_obj_t * build_settings_playback_screen(void) {
    static pill_list_item_t items[8 + PLUGIN_MAX_PLAYBACK_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Car Mode", PILL_ACCESSORY_TOGGLE,
                                    current_settings.car_mode_enabled, NULL, car_mode_switch_event_cb, NULL };
    items[1] = (pill_list_item_t){ "Crossfade", PILL_ACCESSORY_TOGGLE,
                                    current_settings.crossfade_enabled, NULL, crossfade_switch_event_cb, NULL,
                                    &settings_crossfade_toggle_img };
    items[2] = (pill_list_item_t){ "Equalizer", PILL_ACCESSORY_CHEVRON, false, eq_screen_btn_event_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Play/Pause Button", PILL_ACCESSORY_CHEVRON, false, play_pause_button_mode_settings_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "ReplayGain", PILL_ACCESSORY_CHEVRON, false, replaygain_mode_settings_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ "Resume Last Track", PILL_ACCESSORY_CHEVRON, false, resume_mode_settings_row_cb, NULL, NULL };
    items[6] = (pill_list_item_t){ "Sleep Timer", PILL_ACCESSORY_CHEVRON, false, sleep_timer_row_cb, NULL, NULL };
    items[7] = (pill_list_item_t){ "Startup Volume", PILL_ACCESSORY_CHEVRON, false, startup_volume_row_cb, NULL, NULL };

    int count = 8;
    int plugin_count = plugin_manager_get_playback_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_PLAYBACK_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_playback_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_playback_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_playback_list_item_options(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Playback", generic_back_cb, items, count, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Display list row below --
 * same index-not-object shape plugin_settings_list_item_click_cb() (defined
 * further down, alongside build_settings_screen()) already uses. Forward-
 * declared static since build_settings_display_screen() is defined earlier
 * in the file than that sibling. */
static void plugin_display_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_display_list_item_clicked(index);
}

static lv_obj_t * build_settings_display_screen(void) {
    static pill_list_item_t items[7 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Accent Color", PILL_ACCESSORY_CHEVRON, false, accent_color_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Font Size", PILL_ACCESSORY_CHEVRON, false, font_size_settings_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Lyrics Text Size", PILL_ACCESSORY_CHEVRON, false, lyrics_font_size_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Screen Timeout", PILL_ACCESSORY_CHEVRON, false, screen_timeout_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "Screen Dimming", PILL_ACCESSORY_TOGGLE,
                                    current_settings.screen_dimming_enabled, NULL, screen_dimming_switch_event_cb, NULL };
    items[5] = (pill_list_item_t){ "Swipe Up for Home", PILL_ACCESSORY_TOGGLE,
                                    current_settings.swipe_up_home_enabled, NULL, swipe_up_home_switch_event_cb, NULL };
    items[6] = (pill_list_item_t){ "Hide Player/Lyrics Top Bar", PILL_ACCESSORY_TOGGLE,
                                    current_settings.hide_player_topbar, NULL,
                                    hide_player_topbar_switch_event_cb, NULL };

    int count = 7;
    int plugin_count = plugin_manager_get_display_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_DISPLAY_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_display_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_display_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_display_list_item_options(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Display", generic_back_cb, items, count, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Power list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_power_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_power_list_item_clicked(index);
}

static lv_obj_t * build_settings_power_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_POWER_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Charge Limiter (85%)", PILL_ACCESSORY_TOGGLE,
                                    current_settings.charge_limiter_enabled, NULL, charge_limiter_switch_event_cb, NULL };
    items[1] = (pill_list_item_t){ "Safe Charging (500mA)", PILL_ACCESSORY_TOGGLE,
                                    current_settings.safe_charging_enabled, NULL, safe_charging_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ "Idle Shutdown", PILL_ACCESSORY_CHEVRON, false, idle_shutdown_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "LED charge indicator", PILL_ACCESSORY_TOGGLE,
                                    current_settings.led_indicator_enabled, NULL, led_indicator_switch_event_cb, NULL };
    items[4] = (pill_list_item_t){ "Battery Percentage", PILL_ACCESSORY_TOGGLE,
                                    current_settings.show_battery_percent, NULL, battery_percent_switch_event_cb, NULL };

    int count = 5;
    int plugin_count = plugin_manager_get_power_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_POWER_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_power_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_power_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_power_list_item_options(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Power", generic_back_cb, items, count, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered System list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_system_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_system_list_item_clicked(index);
}

/* Defined later, alongside the reboot-confirm popup this opens into (see
 * hostname_entry_done_cb()) -- needed here first, for build_settings_
 * system_screen()'s own row list below. */
static void hostname_row_cb(lv_event_t * e);

static lv_obj_t * build_settings_system_screen(void) {
    static pill_list_item_t items[6 + PLUGIN_MAX_SYSTEM_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "24-Hour Clock", PILL_ACCESSORY_TOGGLE,
                                    current_settings.clock_24h, NULL, clock_24h_switch_event_cb, NULL };
    items[1] = (pill_list_item_t){ "Time Zone", PILL_ACCESSORY_CHEVRON, false, timezone_settings_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "USB Mode", PILL_ACCESSORY_CHEVRON, false, usb_mode_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Hostname", PILL_ACCESSORY_CHEVRON, false, hostname_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "Update Music Database", PILL_ACCESSORY_NONE, false, update_music_database_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ "Factory Reset", PILL_ACCESSORY_NONE, false, factory_reset_btn_cb, NULL, NULL };

    int count = 6;
    int plugin_count = plugin_manager_get_system_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_SYSTEM_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_system_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_system_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_system_list_item_options(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("System", generic_back_cb, items, count, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_category_playback_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_playback_screen);
}

static void settings_category_display_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_display_screen);
}

static void settings_category_power_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_power_screen);
}

static void settings_category_system_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_system_screen);
}

static void settings_about_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(about_screen);
}

/* "Auto-resume on launch" deliberately has no row anywhere in Settings --
 * real-device incident: auto-resume-on-launch is entirely disabled at
 * compile time (AUTO_RESUME_ON_LAUNCH_ENABLED, gui_init()) after a
 * crash-reboot loop report, but this toggle itself was left fully
 * interactive and defaulting to checked -- functionally inert either way,
 * but showing a live-looking control for a feature that silently does
 * nothing is actively misleading. Removed until the feature itself is
 * re-enabled; current_settings.auto_resume_enabled and
 * auto_resume_switch_event_cb are both still there, just unreferenced. */
/* Shared click handler for every plugin-registered Settings list row below
 * -- same index-not-object shape plugin_books_list_item_click_cb() already
 * uses for the Books screen. */
static void plugin_settings_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_settings_list_item_clicked(index);
}

static lv_obj_t * build_settings_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_SETTINGS_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Playback", PILL_ACCESSORY_CHEVRON, false, settings_category_playback_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Display", PILL_ACCESSORY_CHEVRON, false, settings_category_display_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Power", PILL_ACCESSORY_CHEVRON, false, settings_category_power_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "System", PILL_ACCESSORY_CHEVRON, false, settings_category_system_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "About", PILL_ACCESSORY_CHEVRON, false, settings_about_row_cb, NULL, NULL };

    int count = 5;
    int plugin_count = plugin_manager_get_settings_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_SETTINGS_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_settings_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_settings_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_settings_list_item_options(i, &item.icon_asset, &item.row_height, &item.row_width, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Settings", generic_back_cb, items, count, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

static void music_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_library_get_music_screen());
}

static void stream_media_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(stream_media_screen);
}

static void wireless_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_network_get_wireless_screen());
}



static void system_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_screen);
}

/* ---- DAC home screen -- direct shortcut to enabling USB DAC or Bluetooth
 * DAC without going through Settings > System / Wireless > Bluetooth first.
 * Reuses the exact same underlying entry points those screens already use
 * (start_usb_mode_switch()/open_bt_dac_screen()) rather than duplicating
 * any of that state machine. Replaced the home screen's old "About" tile --
 * About is still reachable, just moved into Settings (see
 * settings_about_row_cb()). ---- */

static void dac_home_usb_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Same real-state correction as open_usb_mode_screen()'s own comment --
     * current_settings.usb_mode is only a UI hint, never re-applied to
     * hardware on startup, so it can't be trusted after a reboot or a
     * change made outside this app. If USB DAC is genuinely already active,
     * go straight to its overlay instead of re-running the switch. */
    usb_mode_t detected;
    bool have_detected = usb_mode_control_detect_current(&detected);
    if (have_detected && detected == USB_MODE_DAC) {
        current_settings.usb_mode = (int) USB_MODE_DAC;
        nav_push(gui_network_get_usb_dac_overlay());
        return;
    }
    /* Real-device incident: this shortcut used to call start_usb_mode_switch()
     * unconditionally, which crashed the app when the device was in ADB
     * mode -- an untested path. The USB Mode screen itself never allows
     * this: populate_usb_mode_screen() dims the Storage/DAC rows and makes
     * them non-clickable whenever ADB is active, only letting the user
     * switch away from ADB via its own explicit toggle first. This shortcut
     * has no dimmed-row UI to lean on, so it enforces the same rule with an
     * explanatory toast instead of silently attempting a switch nothing
     * else in this app has ever exercised safely. */
    if (have_detected && detected == USB_MODE_ADB) {
        show_info_toast("Turn off ADB first (Settings > System > USB Mode), then enable USB DAC from here.");
        return;
    }
    start_usb_mode_switch(USB_MODE_DAC);
}

lv_obj_t * build_dac_home_screen(void) {
    static pill_list_item_t items[2];
    items[0] = (pill_list_item_t){ "USB DAC", PILL_ACCESSORY_CHEVRON, false, dac_home_usb_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Bluetooth DAC", PILL_ACCESSORY_CHEVRON, false, bt_dac_settings_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen("DAC", generic_back_cb, items, 2, gui_theme_accent_style(), 6);
    finalize_screen_navigation(scr);
    return scr;
}

static void dac_home_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_shell_get_dac_home_screen());
}


lv_obj_t * build_home_screen(void) {
    static icon_grid_item_t items[6];
    items[0] = (icon_grid_item_t){ "launcher/music.png", "launcher/music_s.png", "Music", music_tile_cb, NULL };
    items[1] = (icon_grid_item_t){ "launcher/stream_media.png", "launcher/stream_media_s.png", "Stream Media", stream_media_tile_cb, NULL };
    items[2] = (icon_grid_item_t){ "launcher/wireless.png", "launcher/wireless_s.png", "Wireless", wireless_tile_cb, NULL };
    items[3] = (icon_grid_item_t){ "launcher/book.png", "launcher/book_s.png", "Books", gui_books_home_tile_cb, NULL };
    items[4] = (icon_grid_item_t){ "launcher/sys_set.png", "launcher/sys_set_s.png", "System", system_tile_cb, NULL };
    items[5] = (icon_grid_item_t){ "launcher/dac.png", "launcher/dac_s.png", "DAC", dac_home_tile_cb, NULL };
    /* No back_btn_cb -- this is the true root, nothing to go back to. No
     * title either -- matches the real stock launcher, which has no header
     * text above its icon grid. */
    lv_obj_t * scr = build_icon_grid_screen(NULL, NULL, items, 6, 100, false, 0);
    finalize_screen_navigation(scr);
    return scr;
}

/* Bigger than the old 82px-tall/default-font cards (real-device feedback:
 * "sliders and text should be a little bigger and not overlap") -- value
 * label gets its own clear band at the top in a larger font, then a solid
 * gap before a taller slider track, instead of a small label crammed above
 * a thin slider in the same tight card. Card is now a plain flex-flow
 * child (no y/lv_obj_align -- see build_eq_screen()'s content container)
 * so screen-wide spacing lives in one place (the container's pad_gap)
 * rather than being hand-tuned per card. */
static lv_obj_t * create_eq_slider_card(lv_obj_t * parent, eq_field_t field, lv_obj_t ** out_value_label,
                                        lv_obj_t ** out_slider, int32_t range_min, int32_t range_max) {
    lv_obj_t * card = lv_obj_create(parent);
    /* 96%, not the original 92% -- real-device bug report: with wider
     * dropdowns and bigger BlindMF text now needing the room, the old
     * side margin looked like unused wasted space. Widened here and on
     * every other row/card in this screen (band_row/type_row/enable_row/
     * profile_row) for consistency. */
    lv_obj_set_width(card, lv_pct(96));
    /* Real-device bug report: at the BlindMF font tier, gui_theme_font(GUI_FONT_ROLE_ROW)'s much
     * taller glyphs (34px vs 22px at Small) pushed the value label's own
     * rendered height past where the slider below it was fixed-positioned
     * (card height 132px, slider anchored 18px off the card's bottom --
     * both tuned for the small/medium tiers' shorter label, with no
     * margin against a bigger one), overlapping them. Flex column with
     * LV_SIZE_CONTENT height fixes this structurally instead of just
     * re-tuning the fixed offsets for one more tier: the card's own height
     * -- and the gap between label and slider -- now follows the label's
     * actual rendered height at whatever font tier is active, so there's
     * no fixed number left to go stale if a future tier adds an even
     * bigger font. */
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    /* Clickable (tap-to-edit, see eq_field_label_click_cb()) -- the whole
     * label area is the tap target, not just the text glyphs, via
     * lv_obj_set_ext_click_area() below. */
    lv_obj_t * value_label = lv_label_create(card);
    lv_obj_add_style(value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_add_flag(value_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(value_label, 16);
    lv_obj_add_event_cb(value_label, eq_field_label_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) field);

    /* Real-device bug report: at lv_pct(100), the knob's own radius
     * overshoots past the slider's nominal track bounds at the min/max
     * ends -- with the track already flush against the card's 14px
     * padding, the knob there touched/clipped against the card's own
     * rounded corners. Inset instead of full-width so the knob has room
     * to sit fully inside the card at both extremes. */
    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_width(slider, lv_pct(88));
    lv_obj_set_height(slider, 34);
    lv_slider_set_range(slider, range_min, range_max);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 20);

    *out_value_label = value_label;
    *out_slider = slider;
    return card;
}

/* ---- PEQ named profiles (save/load to/from the SD card) ----
 *
 * peq.c's own peq_load()/peq_save() always target one fixed, always-current
 * file (loaded at startup, saved on every change) -- these profiles are a
 * separate, explicit "keep this exact setup around under a name" action,
 * living in their own SD card folder rather than mixed in with music. */
#define PEQ_PROFILES_DIR MUSIC_ROOT_DIR "/PEQ_Profiles"

static lv_obj_t * eq_profiles_list;
/* Name of the named profile whose values are currently loaded.  The PEQ
 * engine deliberately knows only about values and its always-current
 * autosave file, so the UI owns this bit of presentation state.  Saving
 * with this unchanged overwrites that profile; editing it creates a new
 * profile and makes the new name current. */
static char eq_current_profile_name[256];

static void eq_set_current_profile_from_path(const char * path) {
    const char * name = basename_of(path);
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", name ? name : "");
    char * dot = strrchr(eq_current_profile_name, '.');
    if (dot && strcmp(dot, ".peq") == 0) *dot = '\0';
}

/* Refreshes every widget on the EQ screen from peq.c's current state --
 * shared by the initial build and by "Load Profile" (which changes
 * everything at once, unlike the individual per-field setters that only
 * ever touch one widget). */
static void refresh_all_eq_widgets(void) {
    if (peq_get_bypass()) lv_obj_clear_state(eq_bypass_switch, LV_STATE_CHECKED);
    else lv_obj_add_state(eq_bypass_switch, LV_STATE_CHECKED);
    lv_slider_set_value(eq_preamp_slider, (int32_t) (peq_get_preamp_db() * 10.0), LV_ANIM_OFF);
    lv_label_set_text_fmt(eq_preamp_value_label, "Pre-Amp: %+.2f dB", peq_get_preamp_db());
    refresh_eq_band_widgets();
}

/* ---- Reset PEQ to defaults, with a confirmation popup -- same hand-built
 * top-layer overlay shape as bt_dac_leave_popup (this codebase doesn't use
 * LVGL's lv_msgbox anywhere), since a factory reset of every band's
 * freq/gain/Q/type plus the preamp is not something a stray tap should be
 * able to trigger by accident. ---- */
static lv_obj_t * eq_reset_popup;
static lv_obj_t * eq_reset_popup_backdrop;

static void hide_eq_reset_popup(void) {
    lv_obj_add_flag(eq_reset_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(eq_reset_popup, LV_OBJ_FLAG_HIDDEN);
}

static void eq_reset_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
}

static void eq_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
}

static void eq_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
    peq_reset_to_defaults();
    eq_current_profile_name[0] = '\0';
    refresh_all_eq_widgets();
    peq_save();
    show_error_toast("PEQ reset to defaults");
}

static void eq_reset_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_remove_flag(eq_reset_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(eq_reset_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(eq_reset_popup_backdrop);
    lv_obj_move_foreground(eq_reset_popup);
}

static void build_eq_reset_popup(void) {
    eq_reset_popup = build_confirm_popup("Reset PEQ to defaults?", LV_LABEL_LONG_WRAP, NULL, NULL, "Reset",
                                          lv_color_make(255, 120, 120), eq_reset_confirm_cb, NULL, "Cancel",
                                          accent_lv_color(), eq_reset_cancel_cb, NULL, eq_reset_popup_backdrop_cb,
                                          &eq_reset_popup_backdrop);
}

/* ---- Factory Reset, with a confirmation popup -- same hand-built
 * top-layer overlay shape as eq_reset_popup right above (this codebase
 * doesn't use LVGL's lv_msgbox anywhere). Wiping every app setting is
 * exactly the kind of thing a stray tap must never be able to trigger.
 * Reboots immediately on confirm (settings_factory_reset() deletes the
 * settings file and returns -- see its own comment in settings.h for why
 * nothing here tries to hot-apply the reset settings instead of just
 * rebooting into them fresh). ---- */
static lv_obj_t * factory_reset_popup;
static lv_obj_t * factory_reset_popup_backdrop;

static void hide_factory_reset_popup(void) {
    lv_obj_add_flag(factory_reset_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(factory_reset_popup, LV_OBJ_FLAG_HIDDEN);
}

static void factory_reset_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
}

static void factory_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
}

static void factory_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
    settings_factory_reset(); /* deletes the settings file and reboots -- see its own comment in settings.h/.c */
}

static void factory_reset_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_remove_flag(factory_reset_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(factory_reset_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(factory_reset_popup_backdrop);
    lv_obj_move_foreground(factory_reset_popup);
}

static void build_factory_reset_popup(void) {
    factory_reset_popup = build_confirm_popup(
        "Reset all settings and reboot?", LV_LABEL_LONG_WRAP, NULL, NULL, "Reset", lv_color_make(255, 120, 120),
        factory_reset_confirm_cb, NULL, "Cancel", accent_lv_color(), factory_reset_cancel_cb, NULL,
        factory_reset_popup_backdrop_cb, &factory_reset_popup_backdrop);
}

/* ---- Font Size restart prompt, same hand-built confirmation-popup shape
 * as eq_reset_popup/factory_reset_popup above -- real-device feedback: the
 * old plain toast ("Restart the app for the new font size to take effect")
 * left the user to go find a way to restart themselves; this offers to do
 * it immediately instead. Not destructive (no data lost, just a restart),
 * so "Restart Now" uses the normal accent color rather than factory
 * reset's warning red. Same /sbin/reboot subprocess_run() call as
 * settings_factory_reset()/idle_shutdown_now()/firmware_update_enter_
 * recovery() -- reboot is how this hardware's own font-size-tier startup
 * path (apply_font_size_tier(), gui_init()) actually gets re-run, there's
 * no live re-style path (see font_size_tier's own doc comment in
 * settings.h). ---- */
static lv_obj_t * font_size_reboot_popup;
static lv_obj_t * font_size_reboot_popup_backdrop;

static void hide_font_size_reboot_popup(void) {
    lv_obj_add_flag(font_size_reboot_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(font_size_reboot_popup, LV_OBJ_FLAG_HIDDEN);
}

static void font_size_reboot_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_font_size_reboot_popup();
}

static void font_size_reboot_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_font_size_reboot_popup();
}

static void font_size_reboot_now_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_font_size_reboot_popup();
    char * reboot_argv[] = { (char *) "/sbin/reboot", NULL };
    subprocess_run(reboot_argv, NULL, 0);
}

void show_font_size_reboot_popup(void) {
    lv_obj_remove_flag(font_size_reboot_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(font_size_reboot_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(font_size_reboot_popup_backdrop);
    lv_obj_move_foreground(font_size_reboot_popup);
}

void build_font_size_reboot_popup(void) {
    font_size_reboot_popup = build_confirm_popup(
        "Restart now to apply the new font size?", LV_LABEL_LONG_WRAP, NULL, NULL, "Restart Now", accent_lv_color(),
        font_size_reboot_now_cb, NULL, "Later", lv_color_make(160, 160, 160), font_size_reboot_later_cb, NULL,
        font_size_reboot_popup_backdrop_cb, &font_size_reboot_popup_backdrop);
}

/* Settings -> System -> Hostname: same "change takes a reboot" shape as
 * font size above, reusing the same shared build_confirm_popup() helper --
 * see hostname_apply()'s own comment for why a reboot is genuinely required
 * here (wifi_on.sh/bt_init each only read their file once, on demand). */
static lv_obj_t * hostname_reboot_popup;
static lv_obj_t * hostname_reboot_popup_backdrop;

static void hide_hostname_reboot_popup(void) {
    lv_obj_add_flag(hostname_reboot_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hostname_reboot_popup, LV_OBJ_FLAG_HIDDEN);
}

static void hostname_reboot_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
}

static void hostname_reboot_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
}

static void hostname_reboot_now_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
    char * reboot_argv[] = { (char *) "/sbin/reboot", NULL };
    subprocess_run(reboot_argv, NULL, 0);
}

static void show_hostname_reboot_popup(void) {
    lv_obj_remove_flag(hostname_reboot_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(hostname_reboot_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(hostname_reboot_popup_backdrop);
    lv_obj_move_foreground(hostname_reboot_popup);
}

static void build_hostname_reboot_popup(void) {
    hostname_reboot_popup = build_confirm_popup(
        "Restart now to apply the new hostname?", LV_LABEL_LONG_WRAP, NULL, NULL, "Restart Now", accent_lv_color(),
        hostname_reboot_now_cb, NULL, "Later", lv_color_make(160, 160, 160), hostname_reboot_later_cb, NULL,
        hostname_reboot_popup_backdrop_cb, &hostname_reboot_popup_backdrop);
}

/* RFC 952/1123 hostname-label charset -- letters/digits/hyphen only, no
 * leading/trailing hyphen. Real bug caught in review: nothing validated
 * this before it was written straight to /usr/data/hostname_override.txt
 * and handed to sethostname() (see hostname_apply.c) -- a space or
 * punctuation typed on the on-screen keyboard would produce an invalid
 * WiFi/BT broadcast name, or corrupt whatever naive parsing a downstream
 * consumer of those two bind-mounted files does. Empty is exempted --
 * that's hostname_entry_done_cb()'s own "reset to stock" sentinel below,
 * not a real hostname. */
static bool hostname_is_valid(const char * text) {
    size_t len = strlen(text);
    if (len == 0) return true;
    if (len > 63) return false;
    if (text[0] == '-' || text[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static void hostname_entry_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (!hostname_is_valid(text)) {
        show_error_toast("Hostname can only use letters, numbers, and hyphens");
        return;
    }
    /* Empty submission means "reset to the stock name" -- hostname_apply()
     * itself treats an empty string as a no-op (leaves whatever's already
     * in effect from a previous boot alone), so resetting to stock also
     * needs a reboot back to the un-overridden squashfs file, same as
     * setting a new one does. */
    snprintf(current_settings.hostname, sizeof(current_settings.hostname), "%s", text);
    settings_save(&current_settings);
    show_hostname_reboot_popup();
}

static void hostname_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Hostname", current_settings.hostname, false, false, hostname_entry_done_cb, NULL);
}

static void eq_profile_row_cb(lv_event_t * e) {
    const char * path = (const char *) lv_event_get_user_data(e);
    if (!peq_load_from_path(path)) {
        show_error_toast("Failed to load profile");
        return;
    }
    eq_set_current_profile_from_path(path);
    refresh_all_eq_widgets();
    peq_save(); /* the loaded profile becomes the new always-current state too */
    nav_pop();
    show_error_toast("Profile loaded");
}

static void populate_eq_profiles_screen(void) {
    lv_obj_clean(eq_profiles_list);

    char ** paths;
    int count;
    if (!peq_scan_profiles(PEQ_PROFILES_DIR, &paths, &count)) {
        lv_obj_t * label = lv_label_create(eq_profiles_list);
        lv_label_set_text(label, "No saved profiles");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t * row = lv_obj_create(eq_profiles_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char display[64];
        snprintf(display, sizeof(display), "%s", basename_of(paths[i]));
        char * dot = strrchr(display, '.');
        if (dot) *dot = '\0'; /* drop the .peq extension for display */

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, display);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, eq_profile_row_cb, LV_EVENT_CLICKED, paths[i]);
    }
    free(paths);
}

static lv_obj_t * build_eq_profiles_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen("Load Profile", &title_label, &eq_profiles_list);
}

static void eq_load_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_eq_profiles_screen();
    nav_push(eq_profiles_screen);
}

static void eq_save_profile_name_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (text[0] == '\0') return;

    mkdir(PEQ_PROFILES_DIR, 0755); /* no-op (EEXIST) if it's already there */

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.peq", PEQ_PROFILES_DIR, text);
    if (peq_save_to_path(path)) {
        snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", text);
        show_error_toast("Profile saved");
    } else {
        show_error_toast("Failed to save profile");
    }
}

static void eq_save_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Profile Name", eq_current_profile_name, false, false, eq_save_profile_name_done_cb, NULL);
}

static lv_obj_t * build_eq_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* Standard back button + title, matching every other sub-screen
     * (build_subsonic_list_screen et al) instead of the old screen's own
     * one-off green "< Back" button -- the other half of "styling should
     * use the same as all the other screens". */
    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, generic_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "PEQ");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);

    /* "EQ Enabled" switch lives in the title row itself (top-right), same
     * spot other screens put a title-row action (e.g. the Wi-Fi screen's
     * "Rescan"). */
    eq_bypass_switch = lv_switch_create(scr);
    lv_obj_align(eq_bypass_switch, LV_ALIGN_TOP_RIGHT, -16, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(eq_bypass_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* Reset to defaults -- sits right next to the enable switch per the
     * bug report ask, aligned relative to the switch itself (not a fixed
     * x offset) so it stays correctly placed regardless of the switch's
     * actual rendered width. Confirmation required (build_eq_reset_popup())
     * since this clobbers every band's freq/gain/Q/type plus the preamp. */
    lv_obj_t * reset_btn = lv_label_create(scr);
    lv_label_set_text(reset_btn, "Reset");
    lv_obj_set_style_text_color(reset_btn, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(reset_btn, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align_to(reset_btn, eq_bypass_switch, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    lv_obj_add_flag(reset_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(reset_btn, 16);
    lv_obj_add_event_cb(reset_btn, eq_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Everything else lives in one scrollable flex-column container below
     * the title row -- pad_gap gives every card the same, generous spacing
     * (the "sliders more spaced out" ask) from one place instead of
     * hand-tuned per-element y-offsets that made it easy for things to end
     * up too close together or overlapping as fields got bigger. */
    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_set_size(content, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    /* Real-device bug report: same root cause as build_compact_list_widget()'s
     * own fix (see that function's comment) -- this container never zeroed
     * its own padding, so it carried LVGL's default object theme padding on
     * top of every child's own pct-width/pad math, shifting the whole PEQ
     * screen's cards right and clipping them against the screen edge. */
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    /* Main-place (1st arg) must be START, not CENTER -- this column's
     * children overflow the container height on purpose (that's what makes
     * it scrollable), and CENTER main-place centers that whole oversized
     * stack instead of anchoring it to the top. That pushed the first card
     * (Pre-Amp) above the reachable top of the scroll range entirely --
     * real-device bug report: "can't see the preamp part, can't scroll up
     * enough". START anchors the stack's top to the container's top, same
     * as every other scrollable list in this file (e.g.
     * build_subsonic_list_screen's list, which never sets this at all and
     * gets START by default). */
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, 16, 0);
    lv_obj_set_style_pad_top(content, 12, 0);
    lv_obj_set_style_pad_bottom(content, 24, 0);

    /* Pre-Amp: an overall gain applied before the per-band filters, so
     * boosted bands can be pulled back down without touching every band's
     * own gain -- same role as the reference DAP's "Pre AMP" control. */
    lv_obj_t * eq_preamp_card = create_eq_slider_card(content, EQ_FIELD_PREAMP, &eq_preamp_value_label, &eq_preamp_slider, -120, 120);

    /* Band picker: a dropdown rather than the original 5x2 grid of
     * tappable band-number buttons -- real-device bug report, the grid's
     * buttons were a fixed 80x44 with no allowance for a bigger font tier
     * (Settings > System > Font Size), so BlindMF's much larger glyphs
     * overlapped both within a button and between adjacent buttons. A
     * dropdown sidesteps this entirely (only ever renders one band's
     * label at a time, closed or open) and matches eq_type_dropdown's own
     * already-working shape just below, including its explicit gui_theme_font(GUI_FONT_ROLE_BODY)
     * font so this one also actually scales with the font tier setting,
     * unlike eq_type_dropdown which was never explicitly given one. */
    /* Dropdown width scales with the font tier -- real-device bug report:
     * fixed at 170px regardless of tier, BlindMF's much wider "Low Shelf"/
     * "High Shelf" text ran into the dropdown's own arrow glyph, which
     * occupies a roughly fixed pixel area on the right regardless of font
     * size. Shared by both dropdowns below rather than computed via
     * LV_SIZE_CONTENT, which would resize the box to whichever option is
     * currently selected and make it visibly jump width every time the
     * user picks a different one. */
    int32_t eq_dropdown_width = 170;
    if (current_settings.font_size_tier == 1) eq_dropdown_width = 200;
    else if (current_settings.font_size_tier == 2) eq_dropdown_width = 240;

    /* Band picker: a dropdown rather than the original 5x2 grid of
     * tappable band-number buttons -- real-device bug report, the grid's
     * buttons were a fixed 80x44 with no allowance for a bigger font tier
     * (Settings > System > Font Size), so BlindMF's much larger glyphs
     * overlapped both within a button and between adjacent buttons. This
     * row (and type_row/enable_row just below) is now LV_SIZE_CONTENT-
     * height and flex-laid-out rather than a fixed 64px with hand-placed
     * children, same "structural, not hand-tuned-per-tier" reasoning as
     * create_eq_slider_card's own redesign above -- a fixed 64px row could
     * still clip a dropdown/switch whose own natural height grows with a
     * bigger font. */
    lv_obj_t * band_row = lv_obj_create(content);
    lv_obj_set_width(band_row, lv_pct(96));
    lv_obj_set_height(band_row, LV_SIZE_CONTENT);
    lv_obj_add_style(band_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(band_row, 0, 0);
    lv_obj_set_style_radius(band_row, 12, 0);
    lv_obj_set_style_pad_all(band_row, 14, 0);
    lv_obj_remove_flag(band_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(band_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(band_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * band_label = lv_label_create(band_row);
    lv_label_set_text(band_label, "Band");
    lv_obj_add_style(band_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(band_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

    eq_band_dropdown = lv_dropdown_create(band_row);
    lv_dropdown_set_options(eq_band_dropdown, "Band0\nBand1\nBand2\nBand3\nBand4\nBand5\nBand6\nBand7\nBand8\nBand9");
    lv_obj_set_style_text_font(eq_band_dropdown, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    /* The closed box and the opened list are two separate LVGL objects
     * (lv_dropdown_create() builds dropdown->list eagerly, as a child of
     * the screen, not of this dropdown) -- styling only the box above left
     * the opened list's text at LVGL's own default (unscaled) font. */
    lv_obj_set_style_text_font(lv_dropdown_get_list(eq_band_dropdown), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(eq_band_dropdown, eq_dropdown_width);

    lv_obj_t * eq_freq_card = create_eq_slider_card(content, EQ_FIELD_FREQ, &eq_freq_value_label, &eq_freq_slider, 0, EQ_FREQ_SLIDER_MAX);
    lv_obj_t * eq_gain_card = create_eq_slider_card(content, EQ_FIELD_GAIN, &eq_gain_value_label, &eq_gain_slider, -120, 120);
    lv_obj_t * eq_q_card = create_eq_slider_card(content, EQ_FIELD_Q, &eq_q_value_label, &eq_q_slider, 1, 100);

    lv_obj_t * type_row = lv_obj_create(content);
    lv_obj_set_width(type_row, lv_pct(96));
    lv_obj_set_height(type_row, LV_SIZE_CONTENT);
    lv_obj_add_style(type_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(type_row, 0, 0);
    lv_obj_set_style_radius(type_row, 12, 0);
    lv_obj_set_style_pad_all(type_row, 14, 0);
    lv_obj_remove_flag(type_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(type_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(type_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * type_label = lv_label_create(type_row);
    lv_label_set_text(type_label, "Type");
    lv_obj_add_style(type_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(type_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

    eq_type_dropdown = lv_dropdown_create(type_row);
    lv_dropdown_set_options(eq_type_dropdown, "Peaking\nLow Shelf\nHigh Shelf");
    /* Same gap as eq_band_dropdown just above -- box and opened list both
     * need the tier-aware font explicitly, LVGL's own default isn't. */
    lv_obj_set_style_text_font(eq_type_dropdown, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(eq_type_dropdown), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(eq_type_dropdown, eq_dropdown_width);

    lv_obj_t * enable_row = lv_obj_create(content);
    lv_obj_set_width(enable_row, lv_pct(96));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_add_style(enable_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_set_style_radius(enable_row, 12, 0);
    lv_obj_set_style_pad_all(enable_row, 14, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * enabled_label = lv_label_create(enable_row);
    lv_label_set_text(enabled_label, "Enable");
    lv_obj_add_style(enabled_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enabled_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

    eq_band_enabled_switch = lv_switch_create(enable_row);
    lv_obj_add_style(eq_band_enabled_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* Save/Load Profile row -- the new SD card persistence ask, sitting at
     * the end of the same scrollable list as everything else rather than
     * a separate screen of its own. */
    /* Real-device bug report: at BlindMF, "Save Profile"/"Load Profile"
     * didn't fit their fixed lv_pct(46) x 56 buttons -- same class of bug
     * as create_eq_slider_card's above, fixed the same structural way:
     * LV_SIZE_CONTENT height (grows to fit however many lines the label
     * needs) and the label itself set to wrap within its button's width
     * rather than overflow it, instead of a fixed pixel box sized for the
     * small/medium tiers' shorter rendered text. flex_grow(1) on both
     * buttons (rather than the old fixed lv_pct(46) each) keeps them
     * evenly split regardless of how wide either one's content ends up. */
    lv_obj_t * profile_row = lv_obj_create(content);
    lv_obj_set_width(profile_row, lv_pct(96));
    lv_obj_set_height(profile_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(profile_row, 0, 0);
    lv_obj_set_style_border_width(profile_row, 0, 0);
    lv_obj_remove_flag(profile_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(profile_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(profile_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(profile_row, 12, 0);

    lv_obj_t * save_btn = lv_obj_create(profile_row);
    lv_obj_set_flex_grow(save_btn, 1);
    lv_obj_set_height(save_btn, LV_SIZE_CONTENT);
    lv_obj_add_style(save_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_radius(save_btn, 12, 0);
    lv_obj_set_style_pad_all(save_btn, 14, 0);
    lv_obj_remove_flag(save_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(save_btn, eq_save_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save Profile");
    lv_obj_set_style_text_font(save_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_style(save_label, gui_theme_accent_style(), 0);
    lv_label_set_long_mode(save_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(save_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(save_label, lv_pct(100));
    lv_obj_center(save_label);

    lv_obj_t * load_btn = lv_obj_create(profile_row);
    lv_obj_set_flex_grow(load_btn, 1);
    lv_obj_set_height(load_btn, LV_SIZE_CONTENT);
    lv_obj_add_style(load_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(load_btn, 0, 0);
    lv_obj_set_style_radius(load_btn, 12, 0);
    lv_obj_set_style_pad_all(load_btn, 14, 0);
    lv_obj_remove_flag(load_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(load_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(load_btn, eq_load_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * load_label = lv_label_create(load_btn);
    lv_label_set_text(load_label, "Load Profile");
    lv_obj_set_style_text_font(load_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_style(load_label, gui_theme_accent_style(), 0);
    lv_label_set_long_mode(load_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(load_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(load_label, lv_pct(100));
    lv_obj_center(load_label);

    /* Establish correct initial visual state for every widget BEFORE
     * binding any event callbacks -- lv_obj_add_state()/clear_state() can
     * itself fire LV_EVENT_VALUE_CHANGED, which would otherwise trigger a
     * spurious peq_save() during screen construction (observed empirically:
     * band 0 ended up saved as enabled=1 on a fresh run with no user
     * interaction at all). */
    refresh_all_eq_widgets();

    lv_obj_add_event_cb(eq_bypass_switch, eq_bypass_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_band_dropdown, eq_band_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_preamp_slider, eq_preamp_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_band_enabled_switch, eq_band_enabled_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_type_dropdown, eq_type_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_freq_slider, eq_freq_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_gain_slider, eq_gain_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_q_slider, eq_q_slider_event_cb, LV_EVENT_ALL, NULL);

    finalize_screen_navigation(scr);
    /* Same reasoning as every other scrollable-content screen in this
     * file: a drag over a card that isn't the slider itself should scroll
     * the list, not bubble up as an app-wide swipe. */
    lv_obj_remove_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* Real-device bug report: dragging a PEQ slider (Pre-Amp/Freq/Gain/Q)
     * right-to-left could still get hijacked into the app-wide "swipe to
     * player" transition mid-drag, abandoning the slider adjustment --
     * the exact same real-device incident already fixed for the Idle
     * Shutdown/Screen Timeout/Startup Volume sliders (see
     * active_press_is_over_drag_adjust_widget()'s and
     * register_swipe_dead_zone()'s own comments): the swipe-to-player
     * gesture is detected by a separate raw-indev-polling path that
     * doesn't go through LVGL's GESTURE_BUBBLE/event system at all, so
     * removing content's own GESTURE_BUBBLE flag above doesn't reach it.
     * These 4 cards were never registered as dead zones for that path,
     * unlike those other sliders' own cards -- fixing that omission here. */
    lv_obj_remove_flag(eq_preamp_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_preamp_card);
    lv_obj_remove_flag(eq_freq_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_freq_card);
    lv_obj_remove_flag(eq_gain_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_gain_card);
    lv_obj_remove_flag(eq_q_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_q_card);
    return scr;
}

void gui_settings_init(void) {
    about_screen = build_about_screen();
    accent_color_screen = build_accent_color_screen();
    screen_timeout_screen = build_screen_timeout_screen();
    startup_volume_screen = build_startup_volume_screen();
    sleep_timer_screen = build_sleep_timer_screen();
    idle_shutdown_screen = build_idle_shutdown_screen();
    timezone_region_screen = build_timezone_region_screen();
    settings_playback_screen = build_settings_playback_screen();
    settings_display_screen = build_settings_display_screen();
    settings_power_screen = build_settings_power_screen();
    settings_system_screen = build_settings_system_screen();
    settings_screen = build_settings_screen();
    eq_screen = build_eq_screen();
    eq_profiles_screen = build_eq_profiles_screen();
    build_firmware_update_popup();
    build_eq_reset_popup();
    build_factory_reset_popup();
    build_font_size_reboot_popup();
    build_hostname_reboot_popup();
}

/* Externs for callbacks defined in gui.c */

/* Extern widget var in gui.c */




lv_obj_t * gui_settings_get_screen(void) { return settings_screen; }
lv_obj_t * gui_settings_get_playback_screen(void) { return settings_playback_screen; }
lv_obj_t * gui_settings_get_display_screen(void) { return settings_display_screen; }
lv_obj_t * gui_settings_get_power_screen(void) { return settings_power_screen; }
lv_obj_t * gui_settings_get_system_screen(void) { return settings_system_screen; }
lv_obj_t * gui_settings_get_about_screen(void) { return about_screen; }
lv_obj_t * gui_settings_get_accent_screen(void) { return accent_color_screen; }
lv_obj_t * gui_settings_get_eq_screen(void) { return eq_screen; }



void gui_settings_sync_crossfade_toggle(void) {
    if (!settings_crossfade_toggle_img) return;
    lv_image_set_src(settings_crossfade_toggle_img,
                     asset_path(current_settings.crossfade_enabled ? "settings/on.png" : "settings/off.png"));
    if (current_settings.crossfade_enabled) lv_obj_add_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
}
