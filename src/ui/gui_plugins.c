#include "gui_plugins.h"
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "gui_queue.h"
#include "gui_player.h"
#include "gui_text_input.h"
#include "screen_builders.h"
#include "metadata.h"
#include "audio.h"
#include "settings.h"
#include "assets.h"
#include "device_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


extern lv_style_t list_row_style;
extern lv_style_t list_row_pressed_style;
extern lv_style_t style_theme_text_muted;
extern lv_style_t style_theme_screen_bg;
extern player_settings_t current_settings;

extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern lv_obj_t * build_subsonic_list_screen(const char * title_text, lv_obj_t ** out_title_label, lv_obj_t ** out_list);
extern void row_label_enable_marquee(lv_obj_t * label);
extern void register_swipe_dead_zone(lv_obj_t * obj);
extern void unregister_swipe_dead_zone(lv_obj_t * obj);
extern void configure_native_slider_rail(lv_obj_t * slider);
extern void play_track_at(int target);
extern void audio_engine_toggle_play_pause(void);
extern void audio_engine_next_track(void);
extern void audio_engine_prev_track(void);

extern bool plugin_now_playing_loaded;
extern char plugin_now_playing_title[256];
extern char plugin_now_playing_artist[256];
extern char plugin_now_playing_album[256];
extern double plugin_now_playing_duration;


/* ---- Plugin list screens (src/plugins/plugin_manager.c's gui_plugin_show_list()
 * bridge) ----
 *
 * A small pool of reusable screens, not one shared screen, because a plugin
 * can chain plugin.show_list() calls (e.g. Audiobooks: pick a book -> pick
 * a chapter) -- nav_push() treats pushing the screen already on top of the
 * stack as a no-op reload rather than a real push (see its own comment), so
 * a single shared screen would make Back skip the first list entirely once
 * a second was opened on top of it. Four levels covers any realistic
 * plugin nesting depth with headroom under NAV_STACK_MAX (16); reusing a
 * still-on-the-stack slot beyond that is a known, accepted bound rather
 * than something plugins are expected to hit. */
static lv_obj_t * plugin_list_screens[PLUGIN_LIST_SCREEN_POOL_SIZE];
static lv_obj_t * plugin_list_title_labels[PLUGIN_LIST_SCREEN_POOL_SIZE];
static lv_obj_t * plugin_list_lists[PLUGIN_LIST_SCREEN_POOL_SIZE];
static int plugin_list_pool_next = 0;

static void plugin_list_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    intptr_t packed = (intptr_t) lv_event_get_user_data(e);
    int slot = (int) (packed >> 16);
    int index = (int) (packed & 0xFFFF);
    plugin_manager_list_item_selected(slot, index);
}

/* A fixed single-line box is what makes LVGL's circular long mode a marquee
 * rather than allowing the label itself to grow over adjacent UI. The mode
 * has no visible effect when the text already fits. */
void configure_scrolling_row_label(lv_obj_t * label, int32_t width) {
    if (width < 40) width = 40;
    lv_obj_set_width(label, width);
    const lv_font_t * font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    /* Not a bare lv_obj_set_height(label, line_height) -- see that
     * function's own comment (screen_builders.c) for why the font's line
     * height alone still clipped descenders and misfired
     * LV_LABEL_LONG_SCROLL_CIRCULAR's vertical scroll below. */
    row_label_apply_bounded_height(label, font);
    row_label_enable_marquee(label);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}

int gui_plugin_show_list(const char * title, const char * const * labels, const char * const * icon_paths,
                          const char * const * text_sizes, int32_t height, int32_t width, int count) {
    int slot = plugin_list_pool_next;
    plugin_list_pool_next = (plugin_list_pool_next + 1) % PLUGIN_LIST_SCREEN_POOL_SIZE;

    lv_label_set_text(plugin_list_title_labels[slot], title);
    lv_obj_t * list = plugin_list_lists[slot];
    lv_obj_clean(list);

    if (count <= 0) {
        lv_obj_t * label = lv_label_create(list);
        lv_label_set_text(label, "Nothing here");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }

    /* Any icon anywhere in this call, or an explicit height, means every row
     * in it builds as a small icon+label container instead of the original
     * bare list_row_style label -- a call with neither anywhere keeps
     * today's exact fast, plain-label path untouched. */
    bool any_icon = false;
    for (int i = 0; i < count && icon_paths; i++) {
        if (icon_paths[i]) { any_icon = true; break; }
    }
    bool use_container_rows = any_icon || height > 0 || width > 0;

    int32_t row_h = LIST_ROW_HEIGHT;
    if (height > 0) {
        row_h = height;
        if (row_h < PILL_ROW_HEIGHT_MIN) row_h = PILL_ROW_HEIGHT_MIN;
        if (row_h > PILL_ROW_HEIGHT_MAX) row_h = PILL_ROW_HEIGHT_MAX;
    }
    int32_t row_w = LIST_ROW_WIDTH;
    if (width > 0) {
        row_w = width;
        if (row_w < PILL_ROW_WIDTH_MIN) row_w = PILL_ROW_WIDTH_MIN;
        if (row_w > PILL_ROW_WIDTH_MAX) row_w = PILL_ROW_WIDTH_MAX;
    }

    for (int i = 0; i < count; i++) {
        const char * icon = icon_paths ? icon_paths[i] : NULL;
        const char * text_size = text_sizes ? text_sizes[i] : NULL;

        if (!use_container_rows) {
            lv_obj_t * row = lv_label_create(list);
            lv_obj_add_style(row, &list_row_style, 0);
            lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_label_set_text(row, labels[i]);
            /* NULL text_size leaves list_row_style's own default font
             * (LIST_ROW_FONT, already fallback-capable) untouched -- see
             * pill_row_resolve_text_size()'s own comment on why a genuine
             * NULL only ever reaches it from a truly-unset call like this. */
            if (text_size) lv_obj_set_style_text_font(row, pill_row_resolve_text_size(text_size), 0);
            row_label_enable_marquee(row);
            if (row_w != LIST_ROW_WIDTH) lv_obj_set_style_width(row, row_w, 0);
            lv_obj_set_height(row, row_h);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            intptr_t packed = ((intptr_t) slot << 16) | (intptr_t) (i & 0xFFFF);
            lv_obj_add_event_cb(row, plugin_list_row_click_cb, LV_EVENT_CLICKED, (void *) packed);
            continue;
        }

        lv_obj_t * row = lv_obj_create(list);
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, labels[i]);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        /* "medium" default -- matches LIST_ROW_FONT (app_font_22), today's
         * existing show_list() row font, so a row without an explicit
         * text_size still renders at its previous size. */
        lv_obj_set_style_text_font(label, pill_row_resolve_text_size(text_size ? text_size : "medium"), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
        pill_row_apply_icon(row, label, icon, PILL_ROW_ICON_PX_DEFAULT, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
        int32_t label_left = LIST_ROW_LABEL_INSET + (icon ? PILL_ROW_ICON_PX_DEFAULT + 12 : 0);
        configure_scrolling_row_label(label, row_w - label_left - LIST_ROW_LABEL_INSET);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        intptr_t packed = ((intptr_t) slot << 16) | (intptr_t) (i & 0xFFFF);
        lv_obj_add_event_cb(row, plugin_list_row_click_cb, LV_EVENT_CLICKED, (void *) packed);
    }

    nav_push(plugin_list_screens[slot]);
    return slot;
}

void gui_plugin_play_paths(const char * const * paths, int count, int start_index) {
    if (count <= 0) return;
    if (start_index < 0) start_index = 0;
    if (start_index >= count) start_index = count - 1;

    char ** new_playlist = malloc(sizeof(char *) * count);
    if (!new_playlist) return;
    for (int i = 0; i < count; i++) {
        new_playlist[i] = paths[i] ? strdup(paths[i]) : strdup("");
    }
    on_file_selected(new_playlist, count, start_index);
}

/* Same shape as gui_plugin_play_paths() above, for a remote-provider queue
 * (plugin.play_remote()/queue_remote_list()): builds one synthetic
 * "remote://<provider>/<track_id>" playlist[] entry per track (see
 * remote_track.h's own comment on why this, not the real stream_url, is
 * what playlist[]/last_track/Favorites ever see) and publishes the
 * matching descriptor table before handing off to the same on_file_
 * selected() every other play-launch path uses. plugin_manager.c has
 * already validated every field (bounded lengths, non-empty provider/
 * track_id) before calling this -- this function re-validates anyway
 * (every remote_track_make_key() result checked, no unconditional malloc)
 * rather than trusting a caller-supplied invariant, since a NULL/failed
 * allocation or an unvalidated key here would otherwise reach strdup()
 * with an uninitialized buffer.
 *
 * Nothing is published until the ENTIRE new queue (both the playlist[]
 * strings and the remote_track_meta_t table) is ready to go: new_playlist
 * is fully built (and freed again on any failure) before
 * remote_track_meta_set_all() is even called, and that call's own
 * all-or-nothing publish (see its own comment) means a malformed or OOM
 * failure at either step leaves whatever was already playing completely
 * untouched, never a playlist[] that references a since-replaced (or
 * never-published) remote_track_meta_t table. */
void gui_plugin_play_remote_tracks(const remote_track_meta_t * tracks, int count, int start_index) {
    if (count <= 0) return;
    if (start_index < 0) start_index = 0;
    if (start_index >= count) start_index = count - 1;

    char ** new_playlist = malloc(sizeof(char *) * (size_t) count);
    if (!new_playlist) return;

    int built = 0;
    for (; built < count; built++) {
        char key[256];
        if (!remote_track_make_key(tracks[built].provider, tracks[built].track_id, key, sizeof(key))) break;
        new_playlist[built] = strdup(key);
        if (!new_playlist[built]) break;
    }
    if (built != count) {
        for (int j = 0; j < built; j++) free(new_playlist[j]);
        free(new_playlist);
        return;
    }

    if (!remote_track_meta_set_all(tracks, count)) {
        for (int j = 0; j < count; j++) free(new_playlist[j]);
        free(new_playlist);
        return;
    }

    clear_player_source(); /* a remote queue has no on-device list to go back to -- same as a Subsonic stream queue */
    on_file_selected(new_playlist, count, start_index);
}

void gui_plugin_show_toast(const char * msg) {
    show_info_toast(msg);
}

void gui_plugin_set_background_color(const char * slot, uint32_t rgb) {
    lv_color_t color = lv_color_hex(rgb);

    if (strcmp(slot, "screen") == 0) {
        lv_style_set_bg_color(&style_theme_screen_bg, color);
        lv_obj_report_style_change(&style_theme_screen_bg);
    } else if (strcmp(slot, "card") == 0) {
        lv_style_set_bg_color(&style_theme_card_bg, color);
        lv_obj_report_style_change(&style_theme_card_bg);
    } else if (strcmp(slot, "list_row") == 0) {
        lv_style_set_bg_color(&list_row_style, color);
        lv_obj_report_style_change(&list_row_style);
        /* pill_row_bg_style is the same "list_row" slot for pill rows --
         * see its own comment in screen_builders.h for why it's a separate
         * style object rather than reusing list_row_style directly. */
        lv_style_set_bg_color(&pill_row_bg_style, color);
        lv_obj_report_style_change(&pill_row_bg_style);
    }
    /* Else: unknown slot -- plugin_manager.c's l_plugin_set_background_color()
     * already validates against the three known names and raises a Lua
     * error before ever reaching here, so this is unreachable in practice;
     * silently ignored rather than asserting, matching this file's own
     * "degraded but working" tolerance elsewhere. */
}

/* Same shape as gui_plugin_set_background_color() above, for text color --
 * see screen_builders.h's own comment on style_theme_text_primary/
 * style_theme_text_muted for scope (destructive-red and accent-tinted text
 * are deliberately not covered). "primary" also mutates list_row_style's
 * own text_color so list rows -- which attach list_row_style directly, not
 * style_theme_text_primary -- stay in sync with the rest of the app's
 * primary text. */
void gui_plugin_set_text_color(const char * slot, uint32_t rgb) {
    lv_color_t color = lv_color_hex(rgb);

    if (strcmp(slot, "primary") == 0) {
        lv_style_set_text_color(&style_theme_text_primary, color);
        lv_obj_report_style_change(&style_theme_text_primary);
        lv_style_set_text_color(&list_row_style, color);
        lv_obj_report_style_change(&list_row_style);
    } else if (strcmp(slot, "muted") == 0) {
        lv_style_set_text_color(&style_theme_text_muted, color);
        lv_obj_report_style_change(&style_theme_text_muted);
    }
    /* Else: unreachable -- see gui_plugin_set_background_color()'s own
     * comment on l_plugin_set_text_color() validating first. */
}

/* Backing storage for plugin.set_home_layout() -- gui_settings.c's
 * build_home_screen() reads this directly (extern via home_layout.h), same
 * "shared mutable state declared in the owning header, written here"
 * pattern this file already uses for list_row_style/style_theme_* above.
 * Unlike those, this is never touched by any live LVGL call -- Home is
 * built once at startup and never rebuilt, so there is nothing to redraw or
 * lv_obj_report_style_change() here. */
home_layout_config_t home_layout_config = { 0 };

/* plugin_manager.c's l_plugin_set_home_layout() has already validated every
 * enum-like field (key -> array index, mode, align, text_size) before
 * building *config, so this is a plain copy -- see home_layout.h's own
 * comment for why a call made after boot only affects the NEXT app start. */
void gui_plugin_set_home_layout(const home_layout_config_t * config) {
    home_layout_config = *config;
}

/* ---- Playback control bridges -- see gui.h's own comment on why these
 * can't just call audio_toggle_pause()/audio_stop()/audio_set_volume()
 * directly from plugin_manager.c the way plugin.eq_*() calls peq_* directly.
 * Each of these calls the exact same local helper the native UI itself uses
 * for that action, so a plugin-driven change looks identical to a
 * button/remote-control-driven one. ---- */

void gui_plugin_toggle_pause(void) {
    toggle_play_pause();
}

void gui_plugin_stop(void) {
    if (audio_is_playing()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
}

void gui_plugin_next_track(void) {
    gui_player_step_manual(1);
}

void gui_plugin_prev_track(void) {
    gui_player_step_manual(-1);
}

void gui_plugin_seek(double seconds) {
    audio_seek(seconds);
}

void gui_plugin_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    gui_player_set_volume_percent(percent);
    audio_set_volume((float) percent / 100.0f);
    current_settings.volume = (float) percent / 100.0f;
    settings_save(&current_settings);
    show_volume_popup(percent);
    refresh_volume_topbar(percent);
}

bool gui_plugin_is_playing(void) {
    return audio_is_playing();
}

bool gui_plugin_is_paused(void) {
    return audio_is_paused();
}

double gui_plugin_get_position_seconds(void) {
    return audio_get_position_seconds();
}

double gui_plugin_get_duration_seconds(void) {
    return audio_get_duration_seconds();
}

bool gui_plugin_get_now_playing(char * title_out, size_t title_size, char * artist_out, size_t artist_size,
                                 char * album_out, size_t album_size, double * out_duration_seconds) {
    if (!plugin_now_playing_loaded) return false;

    snprintf(title_out, title_size, "%s", plugin_now_playing_title);
    snprintf(artist_out, artist_size, "%s", plugin_now_playing_artist);
    snprintf(album_out, album_size, "%s", plugin_now_playing_album);
    *out_duration_seconds = plugin_now_playing_duration;
    return true;
}

/* ---- plugin.set_interval()/clear_interval() -- a small fixed pool of
 * lv_timer_t*, same repeating-timer mechanism update_timer_cb()'s own
 * 500ms polling loop already uses (gui_init(), near the end of this file).
 * One shared timer callback for every slot -- its own slot index is read
 * back out via lv_timer_get_user_data() rather than needing one distinct
 * callback function per slot. ---- */
static lv_timer_t * plugin_interval_timers[PLUGIN_MAX_INTERVALS];

static void plugin_interval_timer_cb(lv_timer_t * timer) {
    int slot = (int) (intptr_t) lv_timer_get_user_data(timer);
    plugin_manager_interval_fired(slot);
}

void gui_plugin_set_interval(int slot, uint32_t period_ms) {
    if (slot < 0 || slot >= PLUGIN_MAX_INTERVALS) return;
    if (plugin_interval_timers[slot]) lv_timer_delete(plugin_interval_timers[slot]); /* defensive -- shouldn't happen, l_plugin_set_interval() only hands out a free slot */
    plugin_interval_timers[slot] = lv_timer_create(plugin_interval_timer_cb, period_ms, (void *) (intptr_t) slot);
}

void gui_plugin_clear_interval(int slot) {
    if (slot < 0 || slot >= PLUGIN_MAX_INTERVALS || !plugin_interval_timers[slot]) return;
    lv_timer_delete(plugin_interval_timers[slot]);
    plugin_interval_timers[slot] = NULL;
}

/* plugin.show_text_input() bridge -- wraps this file's own show_text_entry()
 * singleton screen (forward-declared above, defined further down alongside
 * text_entry_screen's own construction). numeric is always false here -- a
 * plugin wanting numeric-only input can validate/convert the returned
 * string itself, not worth a second Lua-facing parameter for. */
void plugin_text_entry_done_cb(const char * text, void * user_data) {
    (void) user_data;
    plugin_manager_text_input_submitted(text);
}

void gui_plugin_show_text_input(const char * title, const char * initial_text, bool is_password) {
    show_text_entry(title, initial_text, is_password, false, plugin_text_entry_done_cb, NULL);
}



typedef struct {
    int type; /* PLUGIN_SETTINGS_ROW_TAP/_TOGGLE/_SLIDER, plugin_manager.h */
    char label[96];
    bool toggle_value;
    int slider_min, slider_max, slider_value;
    char icon_path[256]; /* "" = none */
    int32_t row_height;  /* 0 = default, ignored for a slider row */
    int32_t row_width;   /* 0 = default; supported by all row types */
    char text_size[8];   /* "" = this row type's own default -- see populate_plugin_settings_list_screen() */
} plugin_settings_list_row_state_t;

static plugin_settings_list_row_state_t
    plugin_settings_list_row_state[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE][PLUGIN_SETTINGS_LIST_MAX_ROWS];
static int plugin_settings_list_row_state_count[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];

static lv_obj_t * plugin_settings_list_screens[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];
static lv_obj_t * plugin_settings_list_title_labels[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];
static lv_obj_t * plugin_settings_list_lists[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];
static int plugin_settings_list_pool_next = 0;

/* Which slider cards THIS slot registered as swipe dead zones on its last
 * populate -- unregistered (see unregister_swipe_dead_zone()'s own comment)
 * right before lv_obj_clean() frees them on the next populate of this same
 * slot, so swipe_dead_zones[] never holds a dangling pointer into a card
 * this pool slot already deleted. */
static lv_obj_t * plugin_settings_list_slider_cards[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE][PLUGIN_SETTINGS_LIST_MAX_SLIDERS];
static int plugin_settings_list_slider_card_count[PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE];

/* Slider-row variant of add_pill_toggle_row()/add_pill_chevron_row() above --
 * no existing helper covers this (native sliders are each their own
 * full-screen card, screen_timeout_slider_card and neighbors, never embedded
 * as one row inside a scrollable list). Same card-with-live-readout shape as
 * those, sized to fit as a list row instead of its own screen: label
 * top-left, live numeric value top-right, slider along the bottom. Caller
 * (populate_plugin_settings_list_screen() below) handles the swipe-dead-zone
 * registration and LV_OBJ_FLAG_GESTURE_BUBBLE removal, same split of
 * responsibility screen_timeout_slider_card's own construction uses. */
static lv_obj_t * add_pill_slider_row(lv_obj_t * parent, const char * label_text, int min, int max, int value,
                                       lv_event_cb_t slider_event_cb, void * user_data, const char * icon_path,
    const char * text_size) {
    lv_obj_t * card = lv_obj_create(parent);
    int32_t row_width = pill_row_default_width();
    lv_obj_set_size(card, row_width, 130);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(card); /* child 0 */
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    /* text_size is never NULL here -- populate_plugin_settings_list_screen()
     * already defaults it to "small" (matching this row's own previous
     * hardcoded gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) before calling in. */
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, 12);
    lv_obj_set_style_text_font(label, pill_row_resolve_text_size(text_size), 0);
    pill_row_apply_icon(card, label, icon_path, PILL_ROW_ICON_PX_DEFAULT, LV_ALIGN_TOP_LEFT, 20, 12);
    configure_scrolling_row_label(label, row_width - (icon_path ? 212 : 136));

    lv_obj_t * value_label = lv_label_create(card); /* child 1 -- see plugin_settings_slider_event_cb()'s lookup */
    lv_obj_add_style(value_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(value_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -20, 12);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    lv_label_set_text(value_label, buf);

    lv_obj_t * slider = lv_slider_create(card); /* child 2 */
    lv_obj_set_width(slider, lv_pct(88));
    lv_obj_set_height(slider, 32);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -14);
    if (max <= min) max = min + 1; /* lv_slider_set_range requires min < max */
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 20);
    lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_ALL, user_data);

    return card;
}

/* Packs a pool slot + row index into one lv_event_cb_t user_data pointer --
 * PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE and PLUGIN_SETTINGS_LIST_MAX_ROWS
 * are both tiny (2 and 24), so 16 bits each leaves enormous headroom. */
static void * pack_plugin_settings_slot_row(int slot, int row) {
    return (void *) (intptr_t) (((intptr_t) slot << 16) | (intptr_t) (row & 0xFFFF));
}
static int unpack_plugin_settings_slot(void * packed) { return (int) (((intptr_t) packed) >> 16); }
static int unpack_plugin_settings_row(void * packed) { return (int) (((intptr_t) packed) & 0xFFFF); }

static void plugin_settings_tap_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    void * packed = lv_event_get_user_data(e);
    plugin_manager_settings_list_row_selected(unpack_plugin_settings_slot(packed), unpack_plugin_settings_row(packed));
}

/* Applies a plugin row's optional `height` -- no-op if unset (<=0). Only
 * used for toggle/tap rows (add_pill_row_base()-based, PNG pill background)
 * -- a slider row's own card already has its own fixed 130px layout with no
 * spare room to grow into, per plugin.show_settings_list()'s own documented
 * "height ignored for slider" rule. Switches the PNG sprite for a plain
 * rounded-rect fill -- see PILL_ROW_HEIGHT_MIN's own comment in
 * screen_builders.h for why a resized row can't just keep the PNG. */
static void apply_plugin_pill_row_resize(lv_obj_t * row_obj, int32_t row_height, int32_t row_width) {
    if (row_height <= 0 && row_width <= 0) return;
    if (row_height > 0) {
        int32_t height = row_height;
        if (height < PILL_ROW_HEIGHT_MIN) height = PILL_ROW_HEIGHT_MIN;
        if (height > PILL_ROW_HEIGHT_MAX) height = PILL_ROW_HEIGHT_MAX;
        lv_obj_set_height(row_obj, height);
    }
    if (row_width > 0) {
        int32_t width = row_width;
        if (width < PILL_ROW_WIDTH_MIN) width = PILL_ROW_WIDTH_MIN;
        if (width > PILL_ROW_WIDTH_MAX) width = PILL_ROW_WIDTH_MAX;
        lv_obj_set_width(row_obj, width);
    }
    lv_obj_set_style_bg_image_src(row_obj, NULL, 0);
    lv_obj_set_style_radius(row_obj, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row_obj, LIST_ROW_BG_COLOR, 0);
}

static void populate_plugin_settings_list_screen(int slot); /* forward -- toggle click rebuilds its own slot */

static void plugin_settings_toggle_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    void * packed = lv_event_get_user_data(e);
    int slot = unpack_plugin_settings_slot(packed);
    int row = unpack_plugin_settings_row(packed);

    plugin_settings_list_row_state_t * st = &plugin_settings_list_row_state[slot][row];
    st->toggle_value = !st->toggle_value;
    plugin_manager_settings_list_toggled(slot, row, st->toggle_value);
    /* Full rebuild to reflect the flipped on.png/off.png sprite -- same
     * "flip the setting, then repopulate" pattern every native dynamic pill
     * toggle row already uses (e.g. dlna_toggle_cb() -> populate_dlna_screen()),
     * rather than reaching into the row's own image object to swap it in
     * place. */
    populate_plugin_settings_list_screen(slot);
}

/* Same VALUE_CHANGED-updates-live/RELEASED-notifies-Lua split as every
 * native settings slider (screen_timeout_slider_event_cb et al.) -- calling
 * back into Lua on every drag tick would hammer the plugin's on_change for a
 * value that only matters once the user lets go. */
static void plugin_settings_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    void * packed = lv_event_get_user_data(e);
    int slot = unpack_plugin_settings_slot(packed);
    int row = unpack_plugin_settings_row(packed);

    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * card = lv_obj_get_parent(slider);
        lv_obj_t * value_label = lv_obj_get_child(card, 1); /* see add_pill_slider_row()'s own child-index comment */
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int) value);
        lv_label_set_text(value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        plugin_settings_list_row_state[slot][row].slider_value = (int) value;
        plugin_manager_settings_list_slid(slot, row, (int) value);
    }
}

/* Rebuilds pool slot `slot` from plugin_settings_list_row_state[slot][] --
 * shared by gui_plugin_show_settings_list() (initial build) and
 * plugin_settings_toggle_row_click_cb() (rebuild after a toggle flips). */
static void populate_plugin_settings_list_screen(int slot) {
    for (int i = 0; i < plugin_settings_list_slider_card_count[slot]; i++) {
        unregister_swipe_dead_zone(plugin_settings_list_slider_cards[slot][i]);
    }
    plugin_settings_list_slider_card_count[slot] = 0;

    lv_obj_t * list = plugin_settings_list_lists[slot];
    lv_obj_clean(list);

    int count = plugin_settings_list_row_state_count[slot];
    if (count <= 0) {
        lv_obj_t * label = lv_label_create(list);
        lv_label_set_text(label, "Nothing here");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }

    for (int row = 0; row < count; row++) {
        plugin_settings_list_row_state_t * st = &plugin_settings_list_row_state[slot][row];
        void * packed = pack_plugin_settings_slot_row(slot, row);
        const char * icon = st->icon_path[0] ? st->icon_path : NULL;
        /* Default "small" -- matches this whole screen's own previous
         * hardcoded gui_theme_font(GUI_FONT_ROLE_SUBTEXT), so a row that doesn't set text_size renders
         * exactly as it did before this field existed. */
        const char * text_size = st->text_size[0] ? st->text_size : "small";

        if (st->type == PLUGIN_SETTINGS_ROW_TOGGLE) {
            lv_obj_t * row_obj = add_pill_toggle_row(list, st->label, st->toggle_value, NULL);
            lv_obj_add_event_cb(row_obj, plugin_settings_toggle_row_click_cb, LV_EVENT_CLICKED, packed);
            lv_obj_t * label = lv_obj_get_child(row_obj, 0); /* add_pill_row_base()'s own child-0-is-the-label layout */
            lv_obj_set_style_text_font(label, pill_row_resolve_text_size(text_size), 0);
            pill_row_apply_icon(row_obj, label, icon, PILL_ROW_ICON_PX_DEFAULT, LV_ALIGN_LEFT_MID, 24, 0);
            apply_plugin_pill_row_resize(row_obj, st->row_height, st->row_width);
            int32_t row_width = st->row_width > 0 ? st->row_width : pill_row_default_width();
            if (row_width < PILL_ROW_WIDTH_MIN) row_width = PILL_ROW_WIDTH_MIN;
            if (row_width > PILL_ROW_WIDTH_MAX) row_width = PILL_ROW_WIDTH_MAX;
            configure_scrolling_row_label(label, row_width - 24 - (icon ? PILL_ROW_ICON_PX_DEFAULT + 12 : 0) - 112);
        } else if (st->type == PLUGIN_SETTINGS_ROW_SLIDER) {
            lv_obj_t * card = add_pill_slider_row(list, st->label, st->slider_min, st->slider_max, st->slider_value,
                                                   plugin_settings_slider_event_cb, packed, icon, text_size);
            apply_plugin_pill_row_resize(card, 0, st->row_width);
            int32_t row_width = st->row_width > 0 ? st->row_width : pill_row_default_width();
            if (row_width < PILL_ROW_WIDTH_MIN) row_width = PILL_ROW_WIDTH_MIN;
            if (row_width > PILL_ROW_WIDTH_MAX) row_width = PILL_ROW_WIDTH_MAX;
            lv_obj_t * label = lv_obj_get_child(card, 0);
            configure_scrolling_row_label(label, row_width - 20 - (icon ? PILL_ROW_ICON_PX_DEFAULT + 12 : 0) - 96);
            /* Same reasoning as every native slider card's own identical
             * pair of calls -- see register_swipe_dead_zone()'s own
             * top-of-block comment. */
            lv_obj_remove_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
            register_swipe_dead_zone(card);
            if (plugin_settings_list_slider_card_count[slot] < PLUGIN_SETTINGS_LIST_MAX_SLIDERS) {
                plugin_settings_list_slider_cards[slot][plugin_settings_list_slider_card_count[slot]++] = card;
            }
        } else { /* PLUGIN_SETTINGS_ROW_TAP */
            lv_obj_t * row_obj = add_pill_chevron_row(list, st->label, NULL);
            lv_obj_add_event_cb(row_obj, plugin_settings_tap_row_click_cb, LV_EVENT_CLICKED, packed);
            lv_obj_t * label = lv_obj_get_child(row_obj, 0);
            lv_obj_set_style_text_font(label, pill_row_resolve_text_size(text_size), 0);
            pill_row_apply_icon(row_obj, label, icon, PILL_ROW_ICON_PX_DEFAULT, LV_ALIGN_LEFT_MID, 24, 0);
            apply_plugin_pill_row_resize(row_obj, st->row_height, st->row_width);
            int32_t row_width = st->row_width > 0 ? st->row_width : pill_row_default_width();
            if (row_width < PILL_ROW_WIDTH_MIN) row_width = PILL_ROW_WIDTH_MIN;
            if (row_width > PILL_ROW_WIDTH_MAX) row_width = PILL_ROW_WIDTH_MAX;
            configure_scrolling_row_label(label, row_width - 24 - (icon ? PILL_ROW_ICON_PX_DEFAULT + 12 : 0) - 60);
        }
    }
}

int gui_plugin_show_settings_list(const char * title, const int * row_types, const char * const * labels,
                                   const bool * toggle_initial, const int * slider_min, const int * slider_max,
                                   const int * slider_value, const char * const * icon_paths, const int32_t * heights,
                                   const int32_t * widths, const char * const * text_sizes, int count) {
    int slot = plugin_settings_list_pool_next;
    plugin_settings_list_pool_next = (plugin_settings_list_pool_next + 1) % PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE;

    lv_label_set_text(plugin_settings_list_title_labels[slot], title);

    int n = count;
    if (n > PLUGIN_SETTINGS_LIST_MAX_ROWS) n = PLUGIN_SETTINGS_LIST_MAX_ROWS;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        plugin_settings_list_row_state_t * st = &plugin_settings_list_row_state[slot][i];
        st->type = row_types[i];
        snprintf(st->label, sizeof(st->label), "%s", labels[i] ? labels[i] : "");
        st->toggle_value = toggle_initial[i];
        st->slider_min = slider_min[i];
        st->slider_max = slider_max[i];
        st->slider_value = slider_value[i];
        snprintf(st->icon_path, sizeof(st->icon_path), "%s", icon_paths[i] ? icon_paths[i] : "");
        st->row_height = heights[i];
        st->row_width = widths[i];
        snprintf(st->text_size, sizeof(st->text_size), "%s", text_sizes[i] ? text_sizes[i] : "");
    }
    plugin_settings_list_row_state_count[slot] = n;

    populate_plugin_settings_list_screen(slot);
    nav_push(plugin_settings_list_screens[slot]);
    return slot;
}


void gui_plugins_init(void) {
    for (int i = 0; i < PLUGIN_LIST_SCREEN_POOL_SIZE; i++) {
        plugin_list_screens[i] = build_subsonic_list_screen("Plugin", &plugin_list_title_labels[i], &plugin_list_lists[i]);
    }
    for (int i = 0; i < PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE; i++) {
        plugin_settings_list_screens[i] = build_subsonic_list_screen("Plugin Settings", &plugin_settings_list_title_labels[i], &plugin_settings_list_lists[i]);
    }
}


void gui_plugin_free_string_array(char ** array, int count) {
    for (int i = 0; i < count; i++) free(array[i]);
    free(array);
}

/* ---- plugin.library_* -- see gui.h's own comment for the design intent.
 * Every one of these goes straight to metadata_db.c (its own METADATA_DB_
 * GUARD), same as gui_plugin_get_artist_albums() and friends above -- no
 * plugin call in this file forces any whole-library load just because it
 * looked at the library first. ---- */

static int gui_plugin_library_clamp_limit(int limit) {
    if (limit <= 0 || limit > GUI_PLUGIN_LIBRARY_MAX_PAGE) return GUI_PLUGIN_LIBRARY_MAX_PAGE;
    return limit;
}

int64_t gui_plugin_library_song_count(void) {
    return metadata_db_get_song_count();
}

int gui_plugin_library_get_songs(const char * query, const char * artist, const char * album_artist,
                                  const char * album, int offset, int limit, song_row_t * out_rows,
                                  int64_t * out_total) {
    if (offset < 0) offset = 0;
    limit = gui_plugin_library_clamp_limit(limit);
    if (out_total) *out_total = metadata_db_count_songs_filtered(query, artist, album_artist, album);
    return metadata_db_get_songs_filtered_page(query, artist, album_artist, album, offset, limit, out_rows);
}

int gui_plugin_library_search(const char * query, int limit, song_row_t * out_rows) {
    limit = gui_plugin_library_clamp_limit(limit);
    return metadata_db_search_songs(query, out_rows, limit);
}

bool gui_plugin_library_get_song(int64_t id, song_row_t * out_row) {
    return metadata_db_get_song_by_id(id, out_row);
}

/* Real bug caught in review, now moot: metadata_db_get_groups_page()/
 * get_albums_page_filtered() used to only support a keyset "after_name"
 * cursor (never actually continued by any real caller), so this used to
 * fetch offset+limit rows in one shot and slice out [offset, offset+limit)
 * in C, capped at a generous-but-still-finite ceiling -- confirmed against
 * this device's real library (210 distinct albums) to silently truncate
 * offsets past that ceiling with no way for a plugin to detect it. Both
 * functions take a real offset now (see their own metadata_db.h comments),
 * so this is a direct pass-through with no cap beyond GUI_PLUGIN_LIBRARY_
 * MAX_PAGE itself. */
int gui_plugin_library_get_artists(int offset, int limit, group_row_t * out_rows) {
    if (offset < 0) offset = 0;
    limit = gui_plugin_library_clamp_limit(limit);
    return metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, offset, limit, out_rows);
}

int gui_plugin_library_get_albums(int offset, int limit, const char * artist_filter, group_row_t * out_rows) {
    if (offset < 0) offset = 0;
    limit = gui_plugin_library_clamp_limit(limit);
    return metadata_db_get_albums_page_filtered(artist_filter, offset, limit, out_rows);
}

bool gui_plugin_refresh_library(void) {
    if (gui_library_has_background_work()) return false;
    start_library_rescan();
    return true;
}
