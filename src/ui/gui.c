#include "gui.h"
#include "assets.h"
#include "backlight.h"
#include "debug_log.h"
#include "import_web.h"
#include "airplay_control.h"
#include "dlna_control.h"
#include "remote_control.h"
#include "battery.h"
#include "wifi_status.h"
#include "audio.h"
#include "file_browser.h"
#include "text_reader.h"
#include "hw_buttons.h"
#include "metadata.h"
#include "metadata_db.h"
#include "peq.h"
#include "screen_builders.h"
#include "settings.h"
#include "subsonic_client.h"
#include "http_client.h"
#include "cover_decode.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
#include "hiby_sys_server.h"
#ifndef HOST_BUILD
#include "bt_media_player.h"
#endif
#include "headphone_status.h"
#include "usb_audio_output.h"
#include "plugin_manager.h"
#include "led_control.h"
#include "charge_limiter.h"
#include "idle_shutdown.h"
#include "power_suspend.h"
#include "device_config.h"
#include "usb_mode_control.h"
#include "usb_dac_bridge.h"
#include "firmware_update.h"
#include "playlist_files.h"
#include "subprocess.h"
#include "timezone_data.h"
#include "timezone_apply.h"
#include "src/core/lv_obj.h"
#include "src/core/lv_obj_pos.h"
#include "src/core/lv_obj_style.h"
#include "src/core/lv_obj_style_gen.h"
#include "src/layouts/flex/lv_flex.h"
#include "src/misc/lv_area.h"
#include "src/draw/lv_draw_buf.h"
#include "src/others/snapshot/lv_snapshot.h"
#include "src/widgets/image/lv_image.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point, per the layout the stock hiby_player uses. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"

  /* main.c's own boot-checkpoint logger (see its own comment there) --
   * gui_init() below calls it directly since the still-unresolved cold-boot
   * hang has moved further into startup than main.c alone can see. Remove
   * alongside main.c's own copy once cold boot is confirmed working. */
  extern void boot_checkpoint(const char * step);

  /* main.c's own best-effort `mount -t vfat .../sd_0` retry (see its own
   * comment on why this needs to exist at all -- no hotplug mechanism for
   * the internal SD card slot exists anywhere on this firmware). Called
   * once at boot from main.c already; poll_sd_card_hotplug() below calls
   * it again periodically so re-inserting the card after boot eventually
   * gets mounted too, not just the very first insertion main.c catches. */
  extern void mount_sd_card_if_needed(void);
#endif

/* ---- UI text size (Settings -> Font Size) ----
 *
 * Real-device feedback: the app's fixed text size (unchanged since the UI
 * rebuild) reads fine on some eyes, too small on others. Every label in
 * this codebase sets its own font via a plain per-object LVGL style
 * (lv_obj_set_style_text_font()) rather than a shared lv_style_t object
 * LVGL could bulk-refresh in place (the same trick app-wide accent color
 * uses, see lv_obj_report_style_change()'s own comment elsewhere in this
 * file) -- so these four pointers stand in for the four literal
 * lv_font_montserrat_NN sizes (16/20/22/28) this app used at every call
 * site that does NOT need fallback_font.h's non-Latin fallback chaining
 * (fixed English UI chrome -- button captions, section headers, settings
 * rows -- that will never show metadata-derived text). Deliberately named
 * ui_size_* rather than app_font_* -- fallback_font.h already declares
 * real (non-const) app_font_16/22/28 instances for the OTHER category
 * (metadata-derived text: song title/artist, drill-down screen titles,
 * ...), and those are handled separately, by resizing fallback_font.c's
 * own copies in place (see its own comment) rather than through these
 * pointers -- reusing that name here would silently collide.
 *
 * Set once by apply_font_size_tier(), called at the very top of
 * gui_init() before any screen is built. Changing the setting afterward
 * only takes effect on the next launch -- see settings.h's own comment on
 * font_size_tier for why a live re-style isn't practical here.
 *
 * Metadata-derived text (song title/artist, ...) is NOT covered by this
 * function -- that goes through fallback_font.h's app_font_16/22/28
 * instead, resized by its own fallback_font_init_early(tier) call
 * (gui_init() passes current_settings.font_size_tier there directly), so
 * both systems move together without this function reaching into that
 * one.
 *
 * Not static -- screen_builders.c's own handful of fixed-chrome literal
 * font sites (build_title(), pill-list row labels/chevron) need these
 * too, same cross-translation-unit pattern fallback_font.h's app_font_16/
 * 22/28 already use. Declared extern in screen_builders.h rather than a
 * new header of their own, since that's the one file already shared by
 * every screen-building call site on both sides. */
const lv_font_t * ui_size_16 = &lv_font_montserrat_16;
const lv_font_t * ui_size_20 = &lv_font_montserrat_20;
const lv_font_t * ui_size_22 = &lv_font_montserrat_22;
const lv_font_t * ui_size_28 = &lv_font_montserrat_28;

static void apply_font_size_tier(int tier) {
    switch (tier) {
        case 1: /* Medium */
            ui_size_16 = &lv_font_montserrat_20;
            ui_size_20 = &lv_font_montserrat_24;
            ui_size_22 = &lv_font_montserrat_26;
            ui_size_28 = &lv_font_montserrat_32;
            break;
        case 2: /* BlindMF -- literal feature name, requested verbatim */
            ui_size_16 = &lv_font_montserrat_24;
            ui_size_20 = &lv_font_montserrat_30;
            ui_size_22 = &lv_font_montserrat_34;
            ui_size_28 = &lv_font_montserrat_40;
            break;
        default: /* Small -- the original, unchanged sizing */
            ui_size_16 = &lv_font_montserrat_16;
            ui_size_20 = &lv_font_montserrat_20;
            ui_size_22 = &lv_font_montserrat_22;
            ui_size_28 = &lv_font_montserrat_28;
            break;
    }
}

static lv_obj_t * home_screen;
static lv_obj_t * music_screen;
static lv_obj_t * files_screen;
static lv_obj_t * files_search_list; /* search-results overlay, see search_binding_t's is_overlay_list comment */
static lv_obj_t * all_songs_screen;
static lv_obj_t * all_songs_list; /* inner scrollable list, for the A-Z browse index -- see build_compact_list_screen()'s out_list param */
static lv_obj_t * artists_screen;
static lv_obj_t * artists_list;
static lv_obj_t * albums_screen;
static lv_obj_t * albums_list;
static lv_obj_t * album_artist_screen;
static lv_obj_t * album_artist_list;
static lv_obj_t * playlists_screen;
static lv_obj_t * playlists_edit_btn;
static bool playlists_edit_mode = false;
static lv_obj_t * artist_albums_screen;
static lv_obj_t * stream_media_screen;
static lv_obj_t * subsonic_entry_screen;
static lv_obj_t * wireless_screen;
static lv_obj_t * wifi_screen;
static lv_obj_t * bt_screen;
static lv_obj_t * books_screen;
static lv_obj_t * about_screen;
static lv_obj_t * accent_color_screen;
static lv_obj_t * player_screen;
static lv_obj_t * settings_screen;
static lv_obj_t * settings_playback_screen;
static lv_obj_t * settings_display_screen;
static lv_obj_t * settings_power_screen;
static lv_obj_t * settings_system_screen;
static lv_obj_t * resume_mode_screen;
static lv_obj_t * resume_mode_list;
static lv_obj_t * dac_home_screen;

static lv_obj_t * screen_timeout_screen;
static lv_obj_t * screen_timeout_switch;
static lv_obj_t * screen_timeout_slider_card;
static lv_obj_t * screen_timeout_value_label;
static lv_obj_t * screen_timeout_slider;

static lv_obj_t * startup_volume_screen;
static lv_obj_t * startup_volume_switch;
static lv_obj_t * startup_volume_slider_card;
static lv_obj_t * startup_volume_value_label;
static lv_obj_t * startup_volume_slider;

static lv_obj_t * sleep_timer_screen;
static lv_obj_t * sleep_timer_value_label;
static lv_obj_t * sleep_timer_slider;

static lv_obj_t * idle_shutdown_screen;
static lv_obj_t * idle_shutdown_switch;
static lv_obj_t * idle_shutdown_slider_card;
static lv_obj_t * idle_shutdown_value_label;
static lv_obj_t * idle_shutdown_slider;
static lv_obj_t * idle_action_section;
static lv_obj_t * idle_action_poweroff_row;
static lv_obj_t * idle_action_suspend_row;

/* timezone_region_screen (small, fixed, pre-built like every other screen)
 * and timezone_city_screen (delete+rebuilt per region on every open) are
 * declared alongside the rest of the Time Zone picker's code, not here --
 * see that block's own comments for why the City screen can't just be
 * pre-built once. */

static lv_obj_t * eq_screen;
static lv_obj_t * eq_bypass_switch;
static lv_obj_t * eq_band_buttons[PEQ_NUM_BANDS];
static lv_obj_t * eq_band_button_labels[PEQ_NUM_BANDS];
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

static lv_obj_t * play_btn;
static lv_obj_t * song_title_label;
static lv_obj_t * song_folder_label;
static lv_obj_t * progress_slider;
static lv_obj_t * pos_label;
static lv_obj_t * dur_label;
static lv_obj_t * format_badge_label;
static lv_obj_t * song_count_label;
static lv_obj_t * favorite_icon;
static lv_obj_t * cover_img;
/* The 480x320 panel behind the transport controls (title/artist/progress/
 * time/controls_row) -- built in build_player_screen(), but also targeted
 * by poll_cover_decode()/compute_reflection_bytes() below to swap its background
 * between the plain static buttom.png (no embedded art to reflect) and a
 * freshly generated per-track reflection, hence file-scope rather than a
 * local inside build_player_screen(). */
static lv_obj_t * player_overlay_panel;
/* Backing pixels for the currently-displayed embedded cover art, if any --
 * a plain RGB565 bitmap produced by cover_decode_to_rgb565() (see
 * poll_cover_decode() below), not the original compressed JPEG/PNG bytes. Freed
 * and replaced whenever a new track loads; must outlive the lv_image_set_src()
 * call since current_cover_dsc.data just points at it, unlike a plain PNG
 * file path. */
static uint8_t * current_cover_bytes;
static lv_image_dsc_t current_cover_dsc;
/* Same idea as current_cover_bytes/current_cover_dsc above, for the
 * generated reflection (see generate_reflection()) shown as
 * player_overlay_panel's background. */
static uint8_t * current_reflection_bytes;
static lv_image_dsc_t current_reflection_dsc;
static bool favorite_is_set = false;
static lv_obj_t * volume_slider;
/* Clock, top bar center: real topbar/N.png digit + topbar/colon.png
 * sprites, same asset family as the volume readout below, instead of an
 * lv_label -- switched from font text so it's pixel-identical in size/style
 * to the volume/headphone indicator rather than an approximate font-size
 * match (real-device feedback: "match the size of ... the volume and
 * headphone indicator"). Fixed HH:MM layout, always all 5 slots visible
 * (no leading-zero hiding the way the volume/battery readouts need, since
 * a clock always shows both digits of the hour). */
static lv_obj_t * clock_topbar_group;
static lv_obj_t * clock_topbar_digit[5]; /* H, H, :, M, M */
/* Volume readout, far left of the status bar: real topbar/N.png digit
 * sprites (theme2 asset set) plus topbar/speaker.png and topbar/po.png
 * (headphone-out glyph), not a text label -- matches the stock player's own
 * top bar exactly (confirmed via a real-device screenshot of the stock
 * `hiby_player` binary: speaker icon, a red-recolored volume number, then a
 * headphone icon, all pinned to the left edge, with the clock centered
 * separately -- our previous layout had guessed a plain lv_label clock at
 * the left edge with the volume group trailing after it, which doesn't
 * match). Up to 3 digit slots for 0-100; unused leading slots are hidden
 * rather than left blank, so the flex row collapses the gap instead of
 * showing empty space before the first significant digit. volume_topbar_headphone
 * is shown/hidden by refresh_headphone_icon() based on real jack-detect
 * state (see headphone_status.h), not always-on. */
static lv_obj_t * volume_topbar_group;
static lv_obj_t * volume_topbar_digit[3];
static lv_obj_t * volume_topbar_headphone;

/* Loud-volume warning color threshold, read once at startup from the stock
 * firmware's own /usr/resource/config.json (see device_config.h) -- e.g. a
 * real R1 Pro had this set to 40 via the stock Settings screen. -1 means
 * "feature disabled" (file missing/host build, or vol_warn_enable=0 in the
 * config), in which case the volume digits always stay their native white
 * and never recolor red, regardless of level. */
static int volume_warn_threshold_percent = -1;

/* Quick-access drawer mirrors of the persistent status bar / player-screen
 * widgets above -- kept in sync from the same single update sites (see
 * refresh_clock_label/refresh_battery_topbar/refresh_wifi_icon/
 * set_play_button_state/favorite_icon_event_cb/apply_track_metadata_to_ui)
 * rather than introducing a second, separately-polled source of truth.
 * NULL until build_quick_drawer() runs; every update site guards on that. */
static lv_obj_t * quick_drawer;
static lv_obj_t * quick_drawer_wifi_icon;
static lv_obj_t * quick_drawer_bt_icon;
static lv_obj_t * quick_drawer_title_label;
static lv_obj_t * quick_drawer_artist_label;
static lv_obj_t * quick_drawer_favorite_icon;
static lv_obj_t * quick_drawer_play_btn;
static lv_obj_t * quick_drawer_order_icon; /* visual-only mirror of order_icon's mode, not independently clickable */
static lv_obj_t * quick_drawer_brightness_track;
static lv_obj_t * quick_drawer_brightness_label;
static bool quick_drawer_open = false;

/* Real-device bug report: the drawer's brightness slider/label only ever
 * reflected whatever backlight_get_percent() read back at build_quick_
 * drawer() time (app startup) -- turning the screen off and back on
 * restores the real prior brightness (see backlight_set_screen_on()'s own
 * comment) without this ever re-reading it, so the slider silently drifted
 * out of sync with the real screen brightness on every screen-off/on cycle,
 * not just the one the reset bug above also affected. Called once at build
 * time (build_quick_drawer() itself) and again every time the drawer opens
 * (open_quick_drawer()), so it's never stale by the time the user can
 * actually see it. */
static void refresh_quick_drawer_brightness(void) {
    int brightness = backlight_get_percent();
    /* backlight_get_percent() already returns a clean logical 0-100 (see
     * backlight.h's own comment) -- host/no-backlight-node is the only
     * remaining case needing a fallback here. */
    if (brightness < 0) brightness = 100;
    lv_slider_set_value(quick_drawer_brightness_track, brightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(quick_drawer_brightness_label, "%d%%", brightness);
}

static char ** playlist = NULL;
static int playlist_count = 0;
static int playlist_index = -1;

/* "Up Next" queue (long-press a song -> Add to Queue): how many playlist[]
 * slots starting right at playlist_index+1 are still-unplayed queue
 * insertions, and where the NEXT "Add to Queue" tap should splice one in.
 * Implemented as a splice into the live playlist[] array (queue_add_song(),
 * defined with the rest of the playback-advance machinery below) rather
 * than a separate out-of-band list, since apply_track_metadata_to_ui()/
 * arm_next_track_for_audio()/the "Track X of Y" label are all already
 * deeply coupled to playlist[]/playlist_index -- reusing that machinery
 * needs zero changes to any of it. compute_auto_advance_index()/compute_
 * manual_step_index() both check queued_pending_count first, ahead of
 * play_mode, so a queued song plays next regardless of shuffle/repeat --
 * plain array adjacency alone wouldn't guarantee that under Shuffle, which
 * jumps around the array via shuffle_order rather than stepping by 1. */
static int queued_pending_count = 0;
static int queue_next_insert_index = -1; /* -1 = nothing pending, next add goes right after playlist_index */

/* Index into all_songs_paths/all_song_tags (NOT playlist[]/playlist_index --
 * those track the CURRENT PLAYBACK QUEUE's own position, which for Group
 * Songs/Files/Subsonic playback isn't an index into the whole-library
 * arrays at all) for whichever song is currently playing, or -1 if nothing
 * is (or the current track isn't part of the local library, e.g. an
 * Airplay/DLNA source). Set once per real track-start in apply_track_
 * metadata_to_ui(). This is the single source of truth every now-playing
 * indicator (Artists/Albums/All Songs/group-songs rows) reads from --
 * see refresh_now_playing_indicators() below. */
static int now_playing_song_index = -1;

/* Where the current playlist came from -- the player screen's "List" menu
 * option (more_menu_list_cb) uses this to reopen the screen the current
 * track was tapped from, scrolled back to it. Deliberately NOT derived
 * from `playlist` itself: that's just a flat array of paths with no
 * memory of which screen/group built it. Each interactive play-launch
 * site (all_songs_row_click_cb, group_song_row_click_cb,
 * files_search_row_click_cb, on_file_browser_selected) calls the
 * matching set_player_source_*() helper right before on_file_selected();
 * the Subsonic-download and DLNA-cast play sites call
 * clear_player_source() instead, since a streamed/cast single track has
 * no on-device list to go back to. */
typedef enum {
    PLAYER_SOURCE_NONE,
    PLAYER_SOURCE_ALL_SONGS,
    PLAYER_SOURCE_GROUP_SONGS,
    PLAYER_SOURCE_FILE_BROWSER,
} player_source_kind_t;

static player_source_kind_t player_source_kind = PLAYER_SOURCE_NONE;

static int player_source_all_songs_index = -1; /* row index into all_songs_list / all_songs_sort_order */

/* Own copy of the group's song indices at the moment playback started --
 * group_songs_indices/count/title_label themselves just describe
 * whichever group group_songs_screen CURRENTLY shows, which can change
 * (browsing to a different artist/album, or a library rescan) before the
 * user ever opens "List". */
static char * player_source_group_title = NULL;
static int * player_source_group_indices = NULL;
static int player_source_group_count = 0;
static int player_source_group_pos = -1; /* row index within the group */

static char player_source_file_browser_dir[PATH_MAX];
static int player_source_file_browser_row = -1;

/* Queue play mode -- cycled via the order/loop/single/random icon on the
 * player screen (order_icon_event_cb). Persisted as current_settings.play_mode
 * (plain int, see settings.h). Sequential is the only mode where reaching the
 * end of the queue actually stops playback instead of continuing somewhere. */
typedef enum {
    PLAY_MODE_SEQUENTIAL = 0,
    PLAY_MODE_REPEAT_ALL,
    PLAY_MODE_REPEAT_ONE,
    PLAY_MODE_SHUFFLE,
} play_mode_t;

static lv_obj_t * order_icon;

/* Shuffle "bag": a permutation of 0..playlist_count-1 walked front-to-back
 * rather than picking a fresh random index every time, so every track plays
 * exactly once before any repeats -- a bare `rand() % count` on every
 * advance can (and eventually will) replay the same track twice in a row or
 * leave others unplayed for a long stretch, which reads as broken shuffle
 * rather than random. Regenerated (and reshuffled) whenever it's stale --
 * see ensure_shuffle_order_current(). */
static int * shuffle_order = NULL;
static int shuffle_order_count = 0; /* playlist_count this bag was generated for -- staleness check */
static int shuffle_pos = -1;        /* index into shuffle_order such that shuffle_order[shuffle_pos] == playlist_index */

static bool user_seeking = false;

static player_settings_t current_settings;

/* Shared style object driving the app-wide accent color (sliders, checked
 * switches, the selected EQ band) -- one lv_style_t whose bg_color gets
 * updated in place whenever the user picks a new color, so every widget
 * that has this style attached re-renders automatically without having to
 * walk and restyle each one individually. */
static lv_style_t style_accent;
/* Plain light-gray text style for the *unselected* EQ band labels -- kept
 * as its own shared style (rather than a one-off lv_obj_set_style_text_color)
 * so toggling selection is just swapping which shared style is attached,
 * with no risk of a local per-object style override taking priority over
 * style_accent and silently defeating the highlight. */
static lv_style_t style_muted_text;

static lv_color_t accent_lv_color(void) {
    return lv_color_hex(current_settings.accent_color);
}

static void apply_accent_color(uint32_t rgb) {
    current_settings.accent_color = rgb;
    lv_style_set_bg_color(&style_accent, lv_color_hex(rgb));
    lv_style_set_text_color(&style_accent, lv_color_hex(rgb));
    /* Real-device bug report: accent color wasn't applying to the volume
     * popup slider, the quick-drawer brightness slider, or the player
     * screen's scrub bar -- those three, unlike every other slider/switch
     * in the app, are built from fixed-art bg_image_src assets
     * (volume/vol_progress.png, playing_plane/progress.png) rather than a
     * plain bg_color fill, so style_accent's bg_color alone never touched
     * them. bg_image_recolor tints an image in place (same "shared style,
     * one update point" mechanism as everything else here) -- see those
     * three sliders' own lv_obj_add_style(..., &style_accent, ...) calls. */
    lv_style_set_bg_image_recolor(&style_accent, lv_color_hex(rgb));
    lv_style_set_bg_image_recolor_opa(&style_accent, LV_OPA_COVER);
    /* image_recolor (distinct LVGL property from bg_image_recolor above) is
     * what a plain lv_image_create() widget reads -- needed for toggle rows
     * built once at startup and never rebuilt (e.g. remote_control_toggle_img),
     * which can't just re-read accent_lv_color() on a later screen visit the
     * way a repopulated list (add_pill_toggle_row()) can.
     *
     * Real-device bug report: on.png's ON sprite bakes its white handle
     * circle and colored track into one flat bitmap (no separate LVGL
     * part/knob the way a real lv_switch has) -- recoloring at LV_OPA_COVER
     * (the previous value here) replaces every non-transparent pixel with
     * the exact same solid accent color regardless of its original
     * lightness, so the handle and track become one indistinguishable
     * blob. LVGL's image recolor is a plain per-channel alpha blend
     * (result = recolor*opa + original*(255-opa), see
     * lv_draw_sw_img.c's "Apply recolor" block) -- at any opa below COVER,
     * the handle's original white and the track's original (darker, more
     * saturated) color still land at different final brightnesses after
     * blending toward the same accent color, since they started from
     * different values. LV_OPA_80 keeps a strong, clearly accent-tinted
     * look while leaving enough of that original contrast for the handle
     * to still read as a distinct circle against the track. */
    lv_style_set_image_recolor(&style_accent, lv_color_hex(rgb));
    lv_style_set_image_recolor_opa(&style_accent, LV_OPA_80);
    /* Modifying a shared lv_style_t's properties in place doesn't by
     * itself invalidate the objects that reference it -- LVGL needs an
     * explicit nudge to know to redraw them (confirmed empirically: without
     * this, the accent color only ever showed on whatever was drawn after
     * the change, not on already-visible widgets like a switch already on
     * screen). lv_obj_refresh_style(NULL, ...) is NOT the right call for
     * this -- it crashes (NULL obj dereference); lv_obj_report_style_change
     * is the API meant specifically for "a shared style changed, find and
     * refresh whoever uses it", and safely walks every screen itself. */
    lv_obj_report_style_change(&style_accent);
    settings_save(&current_settings);
}

static const uint32_t accent_palette[] = {
    0x2196F3, /* blue (default) */
    0x4CAF50, /* green */
    0xF44336, /* red */
    0xFF9800, /* orange */
    0x9C27B0, /* purple */
    0x009688, /* teal */
    0xE91E63, /* pink */
    0xE0E0E0, /* light gray */
    0xFFEB3B, /* yellow */
    0x00BCD4, /* cyan */
    0x3F51B5, /* indigo */
    0xFFC107, /* amber */
    0xCDDC39, /* lime */
    0x795548, /* brown */
    0x607D8B, /* blue gray */
    0xFFFFFF, /* white */
};
#define ACCENT_PALETTE_COUNT (sizeof(accent_palette) / sizeof(accent_palette[0]))
static lv_obj_t * accent_swatches[ACCENT_PALETTE_COUNT];

static void accent_swatch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint32_t rgb = (uint32_t) (intptr_t) lv_event_get_user_data(e);
    apply_accent_color(rgb);

    /* The screen isn't rebuilt on re-visit, so the selection ring has to be
     * updated here rather than only at construction time. */
    for (size_t i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        lv_obj_set_style_border_width(accent_swatches[i], accent_palette[i] == rgb ? 4 : 0, 0);
    }
}

/* Generic back-stack, replacing the old pairwise hardcoded back targets
 * (settings always -> browser, eq always -> settings). Every screen's back
 * button and the left-to-right swipe gesture just call nav_pop(); forward
 * navigation calls nav_push(). Root (home_screen) is seeded once in
 * gui_init() and is never popped past. */
#define NAV_STACK_MAX 16
static lv_obj_t * nav_stack[NAV_STACK_MAX];
static int nav_depth = 0;

/* Forward navigation slides the current screen out to the left as the new
 * one slides in from the right (matching a left-swipe gesture); back
 * navigation is the mirror of that. */
#define NAV_ANIM_TIME_MS 165 /* 220ms * 0.75, per real-hardware feedback */

/* lv_screen_load_anim() animates the real screen OBJECTS, which on this
 * GPU-less MIPS target meant fully re-rendering both the outgoing and
 * incoming screen's whole widget tree on every single animation frame --
 * confirmed via real-hardware testing to be the actual cause of choppy,
 * tearing-prone navigation (not just a refresh-rate/vsync issue). Instead,
 * take ONE snapshot of each screen as a static bitmap up front, and slide
 * those two bitmaps on a top-layer overlay -- "N frames x 2 full re-renders"
 * becomes "2 renders + N cheap bitmap blits". The real target screen is
 * only actually made active once the slide finishes. */

/* A deliberately narrow set of screens are pure static content -- fixed
 * icon-grid/pill-list items baked in at build time, no toggles, no
 * per-item state, nothing a timer ever touches (the clock/battery/wifi
 * live on lv_layer_top(), outside every screen's own snapshot). Their
 * transition bitmap is rendered ONCE at startup and reused forever,
 * since re-rendering them before every single visit was the last
 * remaining cost in an otherwise-cheap transition. Anything with actual
 * dynamic content (player screen, file/song lists, Settings' toggles,
 * the accent-color picker's selection ring, Subsonic status screens) is
 * deliberately left out and always rendered fresh. */
#define STATIC_SNAPSHOT_SCREEN_COUNT 9
static lv_obj_t * static_snapshot_screen[STATIC_SNAPSHOT_SCREEN_COUNT];
static lv_draw_buf_t * static_snapshot_buf[STATIC_SNAPSHOT_SCREEN_COUNT];

static lv_draw_buf_t * get_static_snapshot(lv_obj_t * scr) {
    for (int i = 0; i < STATIC_SNAPSHOT_SCREEN_COUNT; i++) {
        if (static_snapshot_screen[i] == scr) return static_snapshot_buf[i];
    }
    return NULL;
}

static void register_static_snapshot(int index, lv_obj_t * scr) {
    static_snapshot_screen[index] = scr;
    static_snapshot_buf[index] = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
}

typedef struct {
    lv_obj_t * overlay;
    lv_obj_t * img_from;
    lv_obj_t * img_to;
    lv_draw_buf_t * buf_from;
    lv_draw_buf_t * buf_to;
    bool buf_from_owned;
    bool buf_to_owned;
    lv_obj_t * to_scr;
    int32_t to_offset;
    /* Only meaningful for the interactive (finger-driven) player-swipe
     * below (poll_quick_drawer_drag()'s own player_swipe_* state) --
     * screen_transition_slide()'s own fixed-duration path always commits.
     * true: the drag crossed its commit threshold, so finish entering
     * to_scr for real once this settle animation reaches the end. false:
     * released short of the threshold, snap back to from_scr instead --
     * to_scr never becomes the active screen at all. */
    bool commit;
} slide_transition_ctx_t;

static bool slide_transition_active = false;

static void slide_transition_anim_x_cb(void * var, int32_t v) {
    slide_transition_ctx_t * ctx = (slide_transition_ctx_t *) var;
    lv_obj_set_x(ctx->img_from, v);
    lv_obj_set_x(ctx->img_to, v + ctx->to_offset);
}

static void slide_transition_done_cb(lv_anim_t * a) {
    slide_transition_ctx_t * ctx = (slide_transition_ctx_t *) lv_anim_get_user_data(a);
    if (ctx->commit) lv_screen_load(ctx->to_scr); /* skipped on a cancelled interactive drag -- from_scr was never actually replaced, nothing to load */
    lv_obj_delete(ctx->overlay); /* deletes img_from/img_to too */
    if (ctx->buf_from_owned) lv_draw_buf_destroy(ctx->buf_from);
    if (ctx->buf_to_owned) lv_draw_buf_destroy(ctx->buf_to);
    lv_free(ctx);
    slide_transition_active = false;
}

/* Shared setup for both screen_transition_slide()'s own fixed-duration
 * path and the interactive (finger-driven) player-swipe further down
 * (poll_quick_drawer_drag()'s player_swipe_* state) -- snapshotting
 * from_scr/to_scr and building the two-image overlay is identical either
 * way; only how the resulting ctx gets ANIMATED afterward differs (one
 * fixed lv_anim_t vs. driven directly from live touch position, then a
 * short commit/cancel settle animation). Returns NULL if to_scr is
 * already active, a transition is already in flight, or a snapshot
 * failed (OOM) -- the caller should fall back to an instant
 * lv_screen_load(to_scr) in every one of those cases (screen_transition_slide()
 * does; the interactive path just abandons the gesture and lets it fall
 * through as whatever else it might have been, since there's no
 * "genuinely already there" case to fall back to for a still-in-progress
 * drag). ctx->commit defaults to true, matching every existing caller
 * (only the interactive path ever sets it false, on a cancelled drag).
 *
 * force_snapshot skips the live-framebuffer-aliasing fast path below for
 * img_from, always taking a real independent copy instead -- see that
 * code's own comment for why the fast path is normally preferred, and
 * player_swipe_tracking's own call site (poll_quick_drawer_drag()) for why
 * it can't be used there: that comment accepts the live-alias's one
 * known downside ("the outgoing bitmap could show that change a frame
 * early") as a fine trade specifically because the ORIGINAL fixed-duration
 * transition finishes in ~200ms, far too short for anything to visibly
 * redraw. An interactive drag has no such bound -- it lasts exactly as
 * long as the finger stays down, which can be seconds -- so from_scr
 * stays the real lv_screen_active() (never swapped out until commit) and
 * keeps getting its own completely normal LVGL redraws the whole time.
 * Since the live-alias path makes img_from literally the same memory as
 * the real framebuffer, every one of those redraws (the topbar clock
 * ticking over, anything else on Home invalidating) bled through into the
 * SLIDING overlay in real time -- confirmed as a real-device bug report
 * ("swiping to enter the player causes the main menu to flicker"), not
 * just a theoretical risk. A real snapshot is immune to that: whatever
 * from_scr does afterward, img_from stays exactly what it looked like the
 * instant this was called. */
static slide_transition_ctx_t * begin_slide_transition(lv_obj_t * to_scr, bool forward, bool force_snapshot) {
    lv_obj_t * from_scr = lv_screen_active();
    if (from_scr == to_scr || slide_transition_active) return NULL;

    lv_display_t * disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);

    slide_transition_ctx_t * ctx = lv_malloc(sizeof(*ctx));
    ctx->commit = true;

    /* from_scr is, by definition, exactly what's on screen right now -- if
     * the display driver can hand us that page directly (real double
     * buffering via panning), reuse it instead of paying for a whole extra
     * render pass. This is what actually made touch-to-slide-start latency
     * noticeable: two full-screen snapshots taken synchronously in the
     * touch handler, before the very first animation frame. Halving that
     * to one is the single biggest lever available without a caching
     * scheme. (This aliases live fb memory rather than an independent
     * copy, so in the rare case something on from_scr redraws during the
     * ~200ms slide -- e.g. the clock ticking over -- the outgoing bitmap
     * could show that change a frame early; a fine trade for consistently
     * lower latency.) */
    /* lv_display_get_buf_active() -- the real LVGL 9.x API for this, not
     * the fbdev-driver-specific function this used to call, which doesn't
     * actually exist anywhere in the vendored lv_linux_fbdev.h (confirmed
     * by grepping the whole vendored lvgl/ tree for it -- nothing, not
     * even an unexported definition). That made every target build fail
     * outright (implicit-declaration + pointer-from-int errors), so this
     * optimization was never actually exercised on real hardware despite
     * the comment above describing real touch-latency profiling -- either
     * written against a different LVGL checkout that once had a custom
     * fbdev patch, or the profiling was done before this line regressed.
     * lv_display_get_buf_active() returns the same live-fb-aliased
     * lv_draw_buf_t the display driver itself is using -- works on host
     * (SDL) too, so the separate HOST_BUILD/LV_USE_LINUX_FBDEV branch
     * this used to need is gone; NULL there just means no frame has
     * flushed yet, same fallback-to-snapshot reasoning as below. */
    lv_draw_buf_t * active_buf = force_snapshot ? NULL : lv_display_get_buf_active(disp);
    lv_draw_buf_t * buf_from;
    if (active_buf) {
        buf_from = active_buf;
        ctx->buf_from_owned = false;
    } else {
        buf_from = lv_snapshot_take(from_scr, LV_COLOR_FORMAT_RGB565);
        ctx->buf_from_owned = true;
    }
    lv_draw_buf_t * buf_to = get_static_snapshot(to_scr);
    if (buf_to) {
        ctx->buf_to_owned = false;
    } else {
        buf_to = lv_snapshot_take(to_scr, LV_COLOR_FORMAT_RGB565);
        ctx->buf_to_owned = true;
    }
    if (!buf_from || !buf_to) {
        /* Snapshot failed (e.g. OOM) -- caller falls back to an instant cut
         * rather than crash or get stuck mid-navigation/mid-drag. */
        if (buf_from && ctx->buf_from_owned) lv_draw_buf_destroy(buf_from);
        if (buf_to && ctx->buf_to_owned) lv_draw_buf_destroy(buf_to);
        lv_free(ctx);
        return NULL;
    }

    lv_obj_t * overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE); /* swallow touches while the slide is in flight */
    /* The persistent status bar (clock/battery/wifi) and the volume popup
     * both live on this same top layer, created once in gui_init() and
     * meant to stay fixed on every screen, Android-status-bar style. Since
     * this overlay is a sibling created fresh on every transition, it would
     * otherwise draw ON TOP of them (later-added siblings draw over
     * earlier ones) and briefly cover the status bar during every slide --
     * push it to the back of the layer instead so those stay visibly on
     * top throughout. */
    lv_obj_move_to_index(overlay, 0);

    lv_obj_t * img_from = lv_image_create(overlay);
    lv_image_set_src(img_from, buf_from);
    lv_obj_set_pos(img_from, 0, 0);

    int32_t to_offset = forward ? w : -w;
    lv_obj_t * img_to = lv_image_create(overlay);
    lv_image_set_src(img_to, buf_to);
    lv_obj_set_pos(img_to, to_offset, 0);

    ctx->overlay = overlay;
    ctx->img_from = img_from;
    ctx->img_to = img_to;
    ctx->buf_from = buf_from;
    ctx->buf_to = buf_to;
    ctx->to_scr = to_scr;
    ctx->to_offset = to_offset;

    slide_transition_active = true;
    return ctx;
}

static void screen_transition_slide(lv_obj_t * to_scr, bool forward) {
    slide_transition_ctx_t * ctx = begin_slide_transition(to_scr, forward, false); /* fast live-alias path -- fine here, this transition always finishes in NAV_ANIM_TIME_MS */
    if (!ctx) {
        lv_screen_load(to_scr);
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_user_data(&a, ctx);
    lv_anim_set_values(&a, 0, -ctx->to_offset);
    lv_anim_set_duration(&a, NAV_ANIM_TIME_MS);
    lv_anim_set_exec_cb(&a, slide_transition_anim_x_cb);
    lv_anim_set_completed_cb(&a, slide_transition_done_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void nav_push(lv_obj_t * scr) {
    if (nav_depth > 0 && nav_stack[nav_depth - 1] == scr) {
        lv_screen_load(scr); /* already the active screen -- nothing to push */
        return;
    }
    if (nav_depth < NAV_STACK_MAX) {
        nav_stack[nav_depth++] = scr;
    }
    /* Live A/B test: forward navigation (entering a submenu) now cuts
     * instantly instead of sliding -- screen_transition_slide() is still
     * used by nav_pop() below, so backing out still animates. */
    lv_screen_load(scr);
}

static void nav_pop(void) {
    if (nav_depth > 1) nav_depth--;
    screen_transition_slide(nav_stack[nav_depth - 1], false);
}

/* Splices the stack slot at `index` out entirely (shifting everything
 * above it down by one), with no screen load of any kind -- used when a
 * transient interstitial screen (Wi-Fi/Subsonic's "Connecting..."/
 * "Downloading..." screen, or a chained show_text_entry() call) has
 * already been left behind by something that pushed a DIFFERENT screen on
 * top of it instead of returning to it. Real-device bug report: a
 * downloaded Subsonic track played fine (see poll_subsonic_download()'s
 * own comment on the race this and its sibling nav_pop()-skipping fixes
 * solve), but backing out of the player afterward landed back on the now-
 * defunct "Downloading..." screen instead of the song list underneath it
 * -- because skipping nav_pop() to avoid yanking the player screen away
 * left that interstitial's own slot sitting in the stack forever, one
 * level below wherever things actually ended up. This removes exactly
 * that stale slot after the fact, once it's clear something else already
 * took its place, so a later Back walks back through where the user
 * really came from instead of a resolved, no-longer-relevant waiting
 * screen. Purely bookkeeping -- whatever's currently on screen was
 * already loaded by the nav_push() that grew the stack past `index` in
 * the first place, so nothing here should touch the display. */
static void nav_remove_stack_slot(int index) {
    for (int i = index; i < nav_depth - 1; i++) nav_stack[i] = nav_stack[i + 1];
    if (nav_depth > 0) nav_depth--;
}

/* Collapses the whole nav stack back to Home -- used after a library rescan,
 * since any deeper screen (Artists/Albums/group songs/...) may be showing
 * rows built from the pre-rescan data and would otherwise still be reachable
 * via back-navigation. */
static void nav_reset_to_home(void) {
    nav_depth = 1;
    nav_stack[0] = home_screen;
    lv_screen_load(home_screen);
}

/* Shared back-button handler for every screen built via the reusable
 * icon-grid/pill-list builders -- passed directly as their back_btn_cb. */
static void generic_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

/* Sets LV_OBJ_FLAG_GESTURE_BUBBLE on every descendant of `obj` so a swipe
 * started anywhere inside a screen (over a label, button, or scrollable
 * list) bubbles up to the screen itself to be handled as app-wide
 * navigation. Deliberately does NOT recurse into drag-to-adjust widgets
 * (sliders/switches/dropdowns/rollers) -- those consume horizontal drags
 * themselves (e.g. seeking the progress bar), and letting a big drag on one
 * of those also fire a navigation swipe would be surprising. */
static void enable_gesture_bubble_recursive(lv_obj_t * obj) {
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t * child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_slider_class) ||
            lv_obj_check_type(child, &lv_switch_class) ||
            lv_obj_check_type(child, &lv_dropdown_class) ||
            lv_obj_check_type(child, &lv_roller_class)) {
            continue;
        }
        lv_obj_add_flag(child, LV_OBJ_FLAG_GESTURE_BUBBLE);
        enable_gesture_bubble_recursive(child);
    }
}

/* Defined later alongside the rest of the quick-access drawer, but needed
 * here for the swipe-down-from-the-top-edge trigger below. */
static void open_quick_drawer(void);
#define QUICK_DRAWER_ANIM_MS 120 /* real-hardware feedback: 200 felt slow for the post-release snap */
#define QUICK_DRAWER_TRIGGER_ZONE 140 /* swipe-down must start within this many px of the top edge to open it */

/* Global swipe handling for back/forward nav. Swipe left-to-right (finger
 * drags rightward) = go back, matching the standard back gesture shown in
 * the reference photos. Swipe right-to-left (finger drags leftward) = jump
 * straight to the player screen from anywhere, matching the real device's
 * "now playing" shortcut. Registered per-screen in
 * finalize_screen_navigation(), which bubbles correctly via
 * enable_gesture_bubble_recursive() (see its own comment).
 *
 * The quick-access drawer's open/close used to also be driven from here
 * (swipe-down/up as instant, threshold-triggered LV_EVENT_GESTURE actions),
 * but that's now poll_quick_drawer_drag()'s job instead -- see its own
 * comment for why: both the GESTURE-bubbling approach here and an
 * indev-wide LV_EVENT_PRESSING attempt turned out unreliable on real
 * hardware for a surface as densely covered in its own interactive
 * children (icons, a 300px-wide slider) as the drawer is. */
/* Defined in the search-binding section below -- true (and closes it)
 * if `screen` had an active inline search; forward-declared here so the
 * back-swipe gesture can close search first instead of popping straight
 * past it to the previous screen, same convention as a back button/gesture
 * dismissing an open search box before it navigates anywhere. */
static bool search_close_if_active_for_screen(lv_obj_t * screen);

static void screen_gesture_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_indev_t * indev = lv_indev_active();
    if (!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT) {
        if (!search_close_if_active_for_screen(lv_screen_active())) {
            nav_pop();
        }
        /* The finger is still down mid-gesture when the screen swaps out
         * from under it -- without this, its eventual release gets
         * delivered to whatever object now sits at that same coordinate on
         * the NEW screen, firing an unwanted click there (real-hardware
         * testing: swiping back from a submenu landed a phantom tap on
         * whatever Home tile happened to be under the finger). Telling the
         * indev to disregard everything until the next physical release
         * stops that bleed-through. */
        lv_indev_wait_release(indev);
    }
    /* Swipe-left-to-player used to be handled here too (instant
     * nav_push(player_screen), no animation) -- real-device bug report:
     * entering the player didn't follow the finger the way the quick
     * drawer's own drag does. Replaced with a live, finger-driven version
     * in poll_quick_drawer_drag() (see its own player_swipe_* state),
     * which claims the press well before LVGL's own ~50px built-in gesture
     * threshold (LV_INDEV_DEF_GESTURE_LIMIT) would ever fire this handler
     * for LV_DIR_LEFT -- left here removed rather than merely unreachable,
     * since leaving it live risked a second, redundant nav_push() firing
     * behind the new interactive one, and creating an overlay object under
     * an already-moving finger is exactly the kind of thing that corrupts
     * LVGL's own press tracking (see the PRESS_LOST history on the
     * drawer's own icons, quick_drawer_wifi_long_press_cb's comment) if
     * anything else is still independently reacting to the same gesture.
     *
     * Swipe-up-to-Home is deliberately NOT handled here -- real-device
     * feedback: this fired from anywhere on screen, including a drag that
     * started well above the home indicator band and only incidentally
     * ended up moving upward (e.g. an aborted attempt to scroll a list up).
     * home_indicator_gesture_cb() (see build_home_indicator_bar()) is the
     * only path to nav_reset_to_home() now -- it only ever fires for a drag
     * that actually started within the reserved bottom strip, matching the
     * Android gesture-bar convention this is modeled on. */
}

/* Finishing touch every build_XXX_screen() calls just before returning: wire
 * up the swipe gestures generically instead of repeating this per screen. */
static void finalize_screen_navigation(lv_obj_t * scr) {
    /* lv_obj_hit_test() -- and therefore all press/drag/gesture detection --
     * bails out immediately for any object lacking LV_OBJ_FLAG_CLICKABLE
     * (confirmed by reading lv_obj_hit_test() in lv_obj_pos.c). A plain
     * lv_obj_create(NULL) screen doesn't have that flag by default, so a
     * swipe starting on bare screen background (anywhere not already
     * covered by a clickable child) would otherwise never even register as
     * a press, let alone a gesture. */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    /* Every inner container built by this codebase already has SCROLLABLE
     * removed, but the screen root itself never did -- left scrollable (the
     * lv_obj default), a drag is first consumed as a scroll attempt (visible
     * on real hardware as the whole screen dragging/rubber-banding) and only
     * escalates to a real LV_EVENT_GESTURE in narrower cases, which is why
     * swipe-to-go-back wasn't firing reliably. None of these screens use
     * their own root as a scroll viewport -- scrolling, where it exists,
     * always happens on a dedicated inner list/container instead. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    enable_gesture_bubble_recursive(scr);
    /* LV_EVENT_PRESSED is deliberately NOT registered here -- see the
     * indev-level registration in gui_init() below for why. */
    lv_obj_add_event_cb(scr, screen_gesture_event_cb, LV_EVENT_GESTURE, NULL);
}

/* Persistent status bar (clock + battery/wifi icons), built once on LVGL's
 * top layer rather than duplicated into every build_XXX_screen() -- objects
 * on the top layer are drawn over whichever screen is currently active, on
 * every screen, automatically. Left non-clickable throughout so touches
 * still fall through to the real screen underneath (confirmed by reading
 * lv_indev.c's search order: layer_top is checked before the active screen,
 * but only objects with LV_OBJ_FLAG_CLICKABLE ever hit-test positive, so a
 * plain informational bar like this never intercepts anything). Battery
 * percentage (battery_get_percent()), wifi connection state
 * (wifi_get_status()), and the clock are all real, live data -- the
 * battery reads -1 on host (no /sys/class/power_supply there), in which
 * case the label is just left blank and only the plain icon shows; wifi
 * always reads disconnected on host too, since there's no wlan0
 * wpa_supplicant instance to query there, same honest "no data" treatment
 * either way. Battery percentage is rendered the same sprite-digit way as
 * the clock/volume readouts (see battery_topbar_group below), not an
 * lv_label, for the same size/style match. */
static lv_obj_t * battery_topbar_group;
static lv_obj_t * battery_topbar_digit[3];
static lv_obj_t * battery_topbar_percent;
static lv_obj_t * battery_icon_frame;
static lv_obj_t * battery_icon_fill_clip;
static lv_obj_t * battery_icon_fill_img;

/* Fill sprite (topbar/battery.png) bbox within its own 20x30 native canvas,
 * measured directly off the asset (alpha bbox: x 4-15, y 9-22) -- used to
 * clip it down from the bottom as a charge-level gauge in
 * refresh_battery_topbar(). Not derived at runtime since nothing else in
 * this codebase decodes PNG alpha to find sprite bounds; a fixed asset gets
 * a fixed constant, same as every other hand-placed topbar sprite here. */
#define BATTERY_FILL_W 12
#define BATTERY_FILL_H 14
static lv_obj_t * wifi_icon;
static lv_obj_t * bt_status_icon;
static lv_obj_t * a2dp_status_icon;
static lv_obj_t * usb_audio_status_icon;
static lv_obj_t * play_pause_status_icon;
static lv_obj_t * status_bar_band;

static void build_status_bar(void) {
    lv_obj_t * bar = lv_layer_top();

    /* Every plain lv_obj_create() gets LV_OBJ_FLAG_SCROLLABLE by default
     * (confirmed in lv_obj.c's base constructor), including layer_top
     * itself -- nothing ever removes it since we only ever add children to
     * this layer, never scroll it. Left alone, lv_indev_find_scroll_obj()
     * walks the pressed object's FULL ancestor chain (see lv_indev_scroll.c)
     * looking for a scrollable object with overflow, and can end up
     * "claiming" a touch as a scroll of layer_top instead of delivering it
     * as a normal press/click/gesture to whatever real widget was actually
     * touched (this is exactly what silently broke the quick-drawer's
     * swipe-up-to-close gesture, and is a very plausible cause of the
     * drawer's on-screen buttons appearing unresponsive on the real
     * touchscreen -- a real finger tap always has a few px of jitter, unlike
     * a synthetic zero-movement click, and that's enough to trigger this
     * scroll-vs-click arbitration). */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* A dedicated band sized exactly to STATUS_BAR_CLEARANCE, with every
     * status bar element vertically MID-aligned within it, rather than
     * each element aligned to the full-screen top layer with a small
     * hand-tuned Y offset -- the old per-element offsets (1, -3) put
     * everything within a few px of the true screen top regardless of how
     * tall STATUS_BAR_CLEARANCE actually was, so shrinking the clearance
     * left all the real content hugging y=0 with dead space below it
     * instead of using the newly smaller band evenly. */
    lv_obj_t * band = lv_obj_create(bar);
    status_bar_band = band;
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, lv_pct(100), STATUS_BAR_CLEARANCE);
    lv_obj_set_pos(band, 0, 0);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered on screen, not left-aligned -- confirmed against a real
     * stock-player screenshot: "02:45" sat at x=205-273 out of a 480px-wide
     * panel (center ~239, screen center is 240), not flush against the
     * left edge like our previous layout had it. Sprite digits (topbar/
     * N.png + colon.png), same as volume_topbar_group below, instead of an
     * lv_label -- keeps every top bar readout pixel-identical in size/style
     * rather than an lv_font approximating it. */
    clock_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(clock_topbar_group);
    lv_obj_set_size(clock_topbar_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clock_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock_topbar_group, 0, 0);
    lv_obj_remove_flag(clock_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; i++) {
        clock_topbar_digit[i] = lv_image_create(clock_topbar_group);
        lv_image_set_src(clock_topbar_digit[i], asset_path(i == 2 ? "topbar/colon.png" : "topbar/0.png"));
        lv_image_set_scale(clock_topbar_digit[i], LV_SCALE_NONE);
    }

    /* LAST, after every child exists -- see the matching comment on
     * volume_topbar_group's own align() call below for why (LV_SIZE_CONTENT
     * doesn't retroactively re-run an earlier alignment as children grow
     * it). refresh_clock_label() (called right after build_status_bar() in
     * gui_init) immediately overwrites these placeholder "0"/":" sprites
     * with the real time, so there's no visible flash of "00:00". */
    lv_obj_align(clock_topbar_group, LV_ALIGN_CENTER, 0, 0);

    /* Left edge of the bar, in the clock's old spot -- matches the stock
     * player's own layout (speaker icon, red volume number, headphone-out
     * icon, all pinned left, confirmed via real-device screenshot). A flex
     * row lets hidden digit slots (see refresh_volume_topbar()) collapse
     * cleanly instead of leaving a gap. */
    volume_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(volume_topbar_group);
    lv_obj_set_size(volume_topbar_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(volume_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volume_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* No extra column padding -- each digit sprite already has ~1px of
     * transparent margin baked into its own canvas on both edges (e.g.
     * topbar/9.png is a 14px-wide canvas with the glyph itself only
     * spanning x=1..13), which is enough separation on its own. Adding a
     * pad_column on top of that visibly widened the gap between digits at
     * this size (confirmed against real-device feedback: "space between
     * the numbers"). */
    lv_obj_set_style_pad_column(volume_topbar_group, 0, 0);
    lv_obj_remove_flag(volume_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    /* Rendered at native asset resolution (LV_SCALE_NONE), same as
     * battery_icon/wifi_icon/bt_status_icon below -- an earlier downscale
     * here (~0.65x) was based on a mismeasured comparison against the
     * clock text and came out looking too small (real-device feedback).
     * Re-measured directly against a real stock-player screenshot: the red
     * "93" glyph bbox was 21px tall vs the clock's 19px -- i.e. native size
     * is already the right size, no downscale needed. */
    lv_obj_t * volume_topbar_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(volume_topbar_icon, asset_path("topbar/speaker.png"));
    lv_image_set_scale(volume_topbar_icon, LV_SCALE_NONE);

    /* White by default (the sprite's own native color, no recolor style
     * applied at creation); refresh_volume_topbar() below switches each
     * digit to a flat (255,0,0) recolor once the level reaches
     * volume_warn_threshold_percent, matching the stock player's own
     * config-driven behavior (see device_config.h) instead of the flat
     * always-red guess from the previous round. */
    for (int i = 0; i < 3; i++) {
        volume_topbar_digit[i] = lv_image_create(volume_topbar_group);
        lv_image_set_src(volume_topbar_digit[i], asset_path("topbar/0.png"));
        lv_image_set_scale(volume_topbar_digit[i], LV_SCALE_NONE);
    }

    /* Headphone-out glyph (topbar/po.png, confirmed by pixel comparison
     * against a real-device screenshot) -- starts hidden and is only shown
     * by refresh_headphone_icon() once real jack-detect state says a
     * headphone/dongle is actually plugged in (see headphone_status.h),
     * not shown unconditionally like the previous round had it. */
    volume_topbar_headphone = lv_image_create(volume_topbar_group);
    lv_image_set_src(volume_topbar_headphone, asset_path("topbar/po.png"));
    lv_image_set_scale(volume_topbar_headphone, LV_SCALE_NONE);
    lv_obj_add_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);

    /* Same flex row as the headphone-jack glyph above, not a separate fixed
     * position -- shown/hidden independently by its own real A2DP state
     * (poll_refresh_bt_icon()), so it naturally sits right next to the jack
     * glyph when both a wired and a Bluetooth output are connected at once,
     * or takes the jack glyph's spot on its own when only Bluetooth is (the
     * flex row's own hidden-children-collapse behavior, already relied on
     * by the volume digit slots above, does this for free -- no manual
     * "replace" logic needed). */
    a2dp_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(a2dp_status_icon, asset_path("topbar/a2dp.png"));
    lv_image_set_scale(a2dp_status_icon, LV_SCALE_NONE);
    lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN); /* shown by poll_refresh_bt_icon() once an A2DP source PCM exists */

    /* Same flex-collapse shape as the headphone/A2DP glyphs above -- shown/
     * hidden by poll_usb_audio_output() once an external USB audio device
     * (DAC/amp) is detected, entirely automatically, no Settings toggle
     * anywhere (unlike Storage/USB DAC/ADB in the manual USB Mode screen --
     * this is meant to feel like the wired headphone jack, not a mode you
     * switch into). */
    usb_audio_status_icon = lv_image_create(volume_topbar_group);
    /* topbar/usb.png, NOT usb/usb.png -- real-device bug report: the latter
     * is the big centered glyph the USB DAC mode overlay screen uses
     * (build_usb_dac_overlay_screen(), further down), a different asset
     * sized/styled for that full-screen context, not this small topbar
     * status row (which every other icon here -- topbar/a2dp.png,
     * topbar/play.png -- already correctly pulls from topbar/). */
    lv_image_set_src(usb_audio_status_icon, asset_path("topbar/usb.png"));
    lv_image_set_scale(usb_audio_status_icon, LV_SCALE_NONE);
    lv_obj_add_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Rightmost in this row -- always after whichever headphone-output
     * glyph(s) above are currently shown, per the same flex-collapse
     * reasoning. play.png while actually playing, pause.png while paused,
     * hidden entirely when stopped/nothing loaded --
     * refresh_play_pause_topbar(). */
    play_pause_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(play_pause_status_icon, asset_path("topbar/play.png"));
    lv_image_set_scale(play_pause_status_icon, LV_SCALE_NONE);
    lv_obj_add_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Deliberately LAST, after every child exists -- done earlier, the
     * LV_SIZE_CONTENT group still had zero content size at that point, and
     * its later growth as children were added did NOT retroactively re-run
     * this alignment (confirmed on real hardware in an earlier round of
     * this same bug: the group ended up anchored low and out of vertical
     * sync with the rest of the bar). */
    lv_obj_align(volume_topbar_group, LV_ALIGN_LEFT_MID, 16, 0);

    /* Outline frame -- swapped between battery_bg.png (normal),
     * battery_charge_bg.png (charging, has its own baked-in bolt glyph) and
     * battery_low_bg.png (red, <5% and not charging) by
     * refresh_battery_topbar(). Previously this was a single static
     * "topbar/battery.png" (the plain fill rectangle below, with no outline
     * at all) that was never touched again after creation -- the icon never
     * reflected charge state or percentage at all, just a fixed white
     * square regardless of real battery level. */
    battery_icon_frame = lv_image_create(band);
    lv_image_set_src(battery_icon_frame, asset_path("topbar/battery_bg.png"));
    lv_image_set_scale(battery_icon_frame, LV_SCALE_NONE);
    lv_obj_align(battery_icon_frame, LV_ALIGN_RIGHT_MID, -15, 0);

    /* Charge-level gauge: a plain clipping container sized/positioned every
     * refresh to BATTERY_FILL_W x (BATTERY_FILL_H * percent/100), holding
     * the full-size fill sprite bottom-aligned inside it -- lv_obj clips
     * children to its own box by default (no LV_OBJ_FLAG_OVERFLOW_VISIBLE
     * set here), so shrinking the container's height reveals progressively
     * less of the sprite from the top down, same visual as a liquid gauge
     * draining towards the frame's terminal nub. Hidden outright while
     * charging (the charge_bg frame already shows its own bolt glyph) or
     * below 5% (the low frame should read as "empty", not partially
     * filled). */
    battery_icon_fill_clip = lv_obj_create(band);
    lv_obj_remove_style_all(battery_icon_fill_clip);
    lv_obj_set_size(battery_icon_fill_clip, BATTERY_FILL_W, BATTERY_FILL_H);
    lv_obj_remove_flag(battery_icon_fill_clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(battery_icon_fill_clip, battery_icon_frame, LV_ALIGN_BOTTOM_MID, 0, -8);

    battery_icon_fill_img = lv_image_create(battery_icon_fill_clip);
    lv_image_set_src(battery_icon_fill_img, asset_path("topbar/battery.png"));
    lv_image_set_scale(battery_icon_fill_img, LV_SCALE_NONE);
    lv_obj_align(battery_icon_fill_img, LV_ALIGN_BOTTOM_MID, 0, 7);

    /* Sprite digits (topbar/N.png + percent.png), same treatment as the
     * clock/volume readouts above -- up to 3 digit slots (0-100, same
     * leading-slot-hiding scheme as volume_topbar_digit) plus a trailing
     * percent sign. The whole group is hidden outright when the real
     * percent is unknown (battery_get_percent() < 0, e.g. host with no
     * /sys/class/power_supply) -- same "icon only, no fake reading" honesty
     * the old blank-text label had. */
    battery_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(battery_topbar_group);
    lv_obj_set_size(battery_topbar_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(battery_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_topbar_group, 0, 0);
    lv_obj_remove_flag(battery_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        battery_topbar_digit[i] = lv_image_create(battery_topbar_group);
        lv_image_set_src(battery_topbar_digit[i], asset_path("topbar/0.png"));
        lv_image_set_scale(battery_topbar_digit[i], LV_SCALE_NONE);
    }
    battery_topbar_percent = lv_image_create(battery_topbar_group);
    lv_image_set_src(battery_topbar_percent, asset_path("topbar/percent.png"));
    lv_image_set_scale(battery_topbar_percent, LV_SCALE_NONE);

    /* Anchored to battery_icon itself (not a hand-tuned x) rather than a
     * fixed band offset, since the group's own width varies with the
     * digit count (1-3) -- LAST, after every child exists, same reasoning
     * as volume_topbar_group's align() below. */
    lv_obj_align_to(battery_topbar_group, battery_icon_frame, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    wifi_icon = lv_image_create(band);
    lv_image_set_src(wifi_icon, asset_path("topbar/wifi_unconnect.png"));
    lv_image_set_scale(wifi_icon, LV_SCALE_NONE);
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -105, 0);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_wifi_icon() once wifi_control_is_enabled() */

    bt_status_icon = lv_image_create(band);
    lv_image_set_src(bt_status_icon, asset_path("topbar/bluetooth.png"));
    lv_image_set_scale(bt_status_icon, LV_SCALE_NONE);
    lv_obj_align(bt_status_icon, LV_ALIGN_RIGHT_MID, -145, 0);
    lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_bt_icon() once bt_control_is_powered() */
}

static void refresh_play_pause_topbar(void) {
    if (audio_is_playing()) {
        lv_obj_remove_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(play_pause_status_icon, asset_path("topbar/play.png"));
    } else if (audio_is_paused()) {
        lv_obj_remove_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(play_pause_status_icon, asset_path("topbar/pause.png"));
    } else {
        lv_obj_add_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_battery_topbar(void) {
    int percent = battery_get_percent();
    if (percent < 0) {
        lv_obj_add_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_icon_fill_clip, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(battery_icon_frame, asset_path("topbar/battery_bg.png"));
        return;
    }
    lv_obj_remove_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
    if (percent > 100) percent = 100;

    bool charging = battery_is_charging();
    bool low = !charging && percent < 5;

    lv_image_set_src(battery_icon_frame,
                      asset_path(charging ? "topbar/battery_charge_bg.png"
                                 : low    ? "topbar/battery_low_bg.png"
                                          : "topbar/battery_bg.png"));

    /* Fill gauge only makes sense for the plain frame -- the charging frame
     * already carries its own bolt glyph, and "low" should read as visually
     * empty, not a sliver of fill. */
    if (charging || low) {
        lv_obj_add_flag(battery_icon_fill_clip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(battery_icon_fill_clip, LV_OBJ_FLAG_HIDDEN);
        int fill_h = (BATTERY_FILL_H * percent + 50) / 100;
        if (fill_h < 1) fill_h = 1;
        if (fill_h > BATTERY_FILL_H) fill_h = BATTERY_FILL_H;
        lv_obj_set_height(battery_icon_fill_clip, fill_h);
        lv_obj_align_to(battery_icon_fill_clip, battery_icon_frame, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    /* Same leading-slot-hiding scheme as refresh_volume_topbar(). */
    char digits[4];
    snprintf(digits, sizeof(digits), "%d", percent);
    int len = (int) strlen(digits);

    for (int i = 0; i < 3; i++) {
        int digit_index = i - (3 - len);
        if (digit_index < 0) {
            lv_obj_add_flag(battery_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            char asset[24];
            snprintf(asset, sizeof(asset), "topbar/%c.png", digits[digit_index]);
            lv_image_set_src(battery_topbar_digit[i], asset_path(asset));
            lv_obj_remove_flag(battery_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Called once at startup (gui_init, with the volume loaded from settings),
 * every time the hardware volume buttons change the level
 * (update_timer_cb), and now also live while the user drags volume_popup_
 * track itself (volume_popup_track_event_cb below). */
static void refresh_volume_topbar(int32_t percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    char digits[4];
    snprintf(digits, sizeof(digits), "%d", (int) percent);
    int len = (int) strlen(digits);

    /* volume_warn_threshold_percent is -1 when the feature is off (see its
     * declaration) -- guard it explicitly rather than just comparing
     * percent >= threshold, since percent >= -1 is always true. */
    bool warn = volume_warn_threshold_percent >= 0 && percent >= volume_warn_threshold_percent;

    for (int i = 0; i < 3; i++) {
        int digit_index = i - (3 - len);
        if (digit_index < 0) {
            lv_obj_add_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            char asset[24];
            snprintf(asset, sizeof(asset), "topbar/%c.png", digits[digit_index]);
            lv_image_set_src(volume_topbar_digit[i], asset_path(asset));
            lv_obj_remove_flag(volume_topbar_digit[i], LV_OBJ_FLAG_HIDDEN);
        }
        /* LV_OPA_TRANSP disables the recolor mix entirely, leaving the
         * sprite's own native white showing through -- simpler than
         * swapping between a white-recolor and a red-recolor style. */
        lv_obj_set_style_image_recolor(volume_topbar_digit[i], lv_color_make(255, 0, 0), 0);
        lv_obj_set_style_image_recolor_opa(volume_topbar_digit[i], warn ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

/* Polled every timer tick alongside refresh_battery_topbar() -- like
 * battery.c's sysfs read, this is a single cheap fopen/fgets with no
 * subprocess fork, so it doesn't need wifi/bt's throttled polling. */
static void refresh_headphone_icon(void) {
    if (headphone_is_connected()) {
        lv_obj_remove_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    }
}

/* wpa_cli forks a process per call (see wifi_status.c), so this is only
 * polled every WIFI_POLL_TICKS timer ticks rather than every tick like the
 * clock/battery -- wifi signal doesn't change fast enough to need
 * sub-second polling anyway. */
#define WIFI_POLL_TICKS 10
static int wifi_poll_tick_counter = 0;

/* The drawer's own wifi icon just reflects radio-on/off (blue as soon as
 * enabled, real-device feedback: the connected-vs-just-enabled distinction
 * is a top-bar-only thing) -- the top bar icon keeps the finer-grained
 * enabled-vs-actually-associated-to-an-AP distinction below. */
static void refresh_wifi_icon(void) {
    bool enabled = wifi_control_is_enabled();
    if (quick_drawer_wifi_icon) {
        lv_image_set_src(quick_drawer_wifi_icon, asset_path(enabled ? "pull_down/wifi_s.png" : "pull_down/wifi.png"));
    }

    if (!enabled) {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);

    int level;
    if (wifi_get_status(&level)) {
        char asset[40];
        snprintf(asset, sizeof(asset), "topbar/wifi_connect_%d.png", level);
        lv_image_set_src(wifi_icon, asset_path(asset));
    } else {
        lv_image_set_src(wifi_icon, asset_path("topbar/wifi_unconnect.png"));
    }
}

/* Same treatment as refresh_wifi_icon(): the drawer's bt icon just reflects
 * powered-on/off, blue as soon as enabled -- only the top bar distinguishes
 * powered-but-nothing-paired from actually-connected, via
 * bt_control_is_connected() (checks each paired device's "Connected: yes"
 * state via bluetoothctl info, cheap -- no discovery scan). */
/* Mirrors current_settings.bt_dac_mode_enabled's real-world effect --
 * external_dac_block_reason() reads this (see its own comment) instead of
 * calling bt_control_is_powered() itself, since that's a subprocess spawn
 * (bluetoothctl show, potentially several seconds when Bluetooth actually is
 * powered on) and the block check runs on every play-button tap. Kept fresh
 * by refresh_bt_icon()'s existing periodic poll and by poll_bt_toggle()'s
 * immediate refresh after a manual toggle, rather than adding a new
 * subprocess call to the play hot path. */
static bool bt_is_powered_cached = false;

/* The connected A2DP accessory's own MAC + live-negotiated codec, kept
 * fresh by the same background refresh_bt_icon_thread_func() poll as
 * bt_is_powered_cached above -- add_bt_device_row() (Bluetooth screen) uses
 * these to know which paired-device row is the actual A2DP-audio one (not
 * just "connected" -- a non-audio BLE peripheral could be connected too)
 * and what to print on its second line. Empty when nothing's A2DP-connected. */
static char bt_connected_mac_cached[18] = "";
static char bt_connected_codec_cached[32] = "";

/* Real-device incident: on a genuine cold boot, hci0 briefly reads
 * "powered" right after S80_bt_init's own firmware flash (which leaves the
 * raw HCI device up as a side effect of attaching it), before bluetoothd
 * itself has even started, and forces it back off per its own
 * AutoEnable=false default (/etc/bluetooth/main.conf) once bluetoothd does
 * start -- a real, if variable-length (real-device measurements from ~5s
 * to ~16s depending on boot timing), on-then-off transition at the
 * hci0/bluetoothd level, not a UI caching bug -- see S80_bt_init's own
 * /usr/bin/bt_init for the actual sequence. This app's own status poll can
 * straddle that window and flash the topbar/drawer icon on, then off, a
 * few seconds apart.
 *
 * A plain time-boxed suppression window was tried here and removed
 * (2026-08-13): it only hid the transient from polls that happened to land
 * inside the window, so the same on-then-off flash still showed up right
 * after the window closed instead -- not an actual fix, just relocating
 * when the flicker becomes visible.
 *
 * Reintroduced with a real difference this time: rather than a fixed
 * window a slow boot can simply outlast, this suppresses the display
 * (forces "off", regardless of the real polled state -- see
 * poll_refresh_bt_icon()'s own use of this) until EITHER a generous
 * safety cap elapses OR the user manually taps the Bluetooth toggle
 * themselves (quick_drawer_bt_event_cb() ends it immediately on tap) --
 * whichever comes first. A manual tap ending it immediately is the part
 * the 2026-08-13 attempt didn't have: it means a user who taps to turn
 * Bluetooth on isn't fighting a suppression window that doesn't know
 * they've acted, and from that point on this app shows real state
 * honestly, including if S80_bt_init's own still-in-flight script later
 * reverts that tap -- at least that's now visible as what it is (a real
 * state change) instead of unexplained UI noise. Doesn't fix the
 * underlying transient itself (still S80_bt_init/bluetoothd startup, still
 * outside this app), just stops this app's own display from being
 * confusing about it. */
static uint32_t bt_boot_suppress_start_tick = 0; /* 0 once suppression has ended (cap elapsed or a manual tap) */
#define BT_BOOT_SUPPRESS_MS 20000

/* /usr/bin/bt_init's (stock, unmodified) very last line is
 * `mkdir -p /tmp; echo > /tmp/bt_init_ok`, right after its own real UART
 * chip firmware flash and everything else it does. /tmp is tmpfs on this
 * device, so this file can never be a stale leftover from a previous boot.
 *
 * Real-device incident: tapping Bluetooth on (quick_drawer_bt_event_cb()
 * below) while bt_init's own chip flash was still genuinely in progress
 * raced this app's own bt_control_init_chip() -> /usr/bin/bt_resume
 * against it -- confirmed live to actually wedge the chip (unrecoverable
 * without a full power cycle), the exact class of incident bt_chip_mutex's
 * own comment already documents (a userspace mutex in THIS app can't
 * protect against a SEPARATE process, bt_init, touching the same UART).
 * The display-suppression window above made the *cosmetic* flicker
 * disappear, but made this WORSE, not better: "graduate on tap" meant a
 * tap landing early now reliably reached the real toggle thread instead of
 * being naturally rate-limited by the flicker's own visibility. Checked in
 * quick_drawer_bt_event_cb() before it does anything real -- see its own
 * comment. */
#define BT_INIT_OK_FLAG_PATH "/tmp/bt_init_ok"

static bool bt_boot_suppress_active(void) {
    if (bt_boot_suppress_start_tick == 0) return false;
    return lv_tick_get() - bt_boot_suppress_start_tick < BT_BOOT_SUPPRESS_MS;
}

/* Moved up from the Bluetooth settings screen section further down (still
 * used there, see populate_bt_screen()) -- refresh_bt_icon_thread_func()
 * below needs to reference bt_scan_results directly, before its own
 * definition down there, to fix a real-device bug: the settings screen's
 * device list only ever reflected paired/connected state as of the last
 * explicit scan, never refreshed afterward unlike the top-bar icon (see
 * poll_refresh_bt_icon()'s own comment on the merge step below). */
#define BT_MAX_RESULTS 32
static bt_device_t bt_scan_results[BT_MAX_RESULTS];
static int bt_scan_result_count = 0;

/* Written by refresh_bt_icon_thread_func() below, merged into
 * bt_scan_results by poll_refresh_bt_icon() -- see its own comment. */
static bt_device_t bt_paired_states_result[BT_MAX_RESULTS];
static int bt_paired_states_count = 0;

/* Real-device incident: this used to call bt_control_is_powered()/
 * bt_control_is_connected() (bluetoothctl show / bluetoothctl info)
 * directly, synchronously, right here on the UI thread -- every call site
 * of what's now start_refresh_bt_icon() ran on that thread, including the
 * periodic ~5s poll. subprocess_run()'s own 15s timeout-and-kill exists
 * specifically because bluetoothctl show is known to hang under certain
 * Bluetooth states (see its doc comment) -- confirmed on a real device
 * that active A2DP audio streaming (Bluetooth DAC, phone actively playing)
 * is exactly such a state: bluetoothctl show hung, and since the periodic
 * poll re-issued another call as soon as (or before) the previous one's
 * bounded wait gave up, the whole UI stayed frozen for as long as the hang
 * persisted, not just one bounded 15s stall. Backgrounded the same way
 * every other slow Bluetooth operation in this file already is, so a hang
 * here can no longer block LVGL's own timer_handler() from running. */
static pthread_t refresh_bt_icon_thread;
static bool refresh_bt_icon_active = false;
static volatile bool refresh_bt_icon_done_flag = false;
static bool refresh_bt_icon_result_powered = false;
static bool refresh_bt_icon_result_connected = false;
static bool refresh_bt_icon_result_a2dp_connected = false;
static char refresh_bt_icon_result_mac[18] = "";
static char refresh_bt_icon_result_codec[32] = "";

static void * refresh_bt_icon_thread_func(void * arg) {
    (void) arg;
    bool powered = bt_control_is_powered();
    refresh_bt_icon_result_powered = powered;

    /* Same background thread/cadence as everything else here -- one more
     * subprocess call (bluealsa-cli list-pcms) alongside the bluetoothctl
     * calls below, not a separate poll loop. */
    refresh_bt_icon_result_a2dp_connected = powered && bt_control_is_a2dp_source_connected();

    /* Two more subprocess calls (bluealsa-cli info, reusing the same PCM
     * path lookup bt_control_is_a2dp_source_connected() just did) -- only
     * worth paying when something's actually A2DP-connected. Both left at
     * "" (not stale) when nothing is, so add_bt_device_row() never shows a
     * leftover codec line for a device that just disconnected. */
    refresh_bt_icon_result_mac[0] = '\0';
    refresh_bt_icon_result_codec[0] = '\0';
    if (refresh_bt_icon_result_a2dp_connected) {
        bt_control_get_connected_device_mac(refresh_bt_icon_result_mac, sizeof(refresh_bt_icon_result_mac));
        bt_control_get_connected_device_codec(refresh_bt_icon_result_codec, sizeof(refresh_bt_icon_result_codec));
    }

    if (powered) {
        /* bt_control_list_paired_states(), not bt_control_is_connected() --
         * same per-device `bluetoothctl info` cost either way, but this also
         * hands back the full breakdown poll_refresh_bt_icon() merges into
         * bt_scan_results below, instead of throwing it away. -1 (the query
         * itself failed, not "genuinely 0 paired") is normalized to 0 here
         * for the any_connected scan below (an empty loop either way), but
         * poll_refresh_bt_icon() checks the raw value separately before
         * treating "nothing here" as authoritative -- see its own comment. */
        bt_paired_states_count = bt_control_list_paired_states(bt_paired_states_result, BT_MAX_RESULTS);
        bool any_connected = false;
        for (int i = 0; i < bt_paired_states_count; i++) {
            if (bt_paired_states_result[i].connected) { any_connected = true; break; }
        }
        refresh_bt_icon_result_connected = any_connected;
    } else {
        bt_paired_states_count = 0;
        refresh_bt_icon_result_connected = false;
    }

    refresh_bt_icon_done_flag = true; /* written last -- poll_refresh_bt_icon only checks this flag */
    return NULL;
}

static void start_refresh_bt_icon(void) {
    if (refresh_bt_icon_active) return; /* previous check still in flight -- same "ignore taps until it lands" pattern as everything else here */
    refresh_bt_icon_active = true;
    refresh_bt_icon_done_flag = false;
    pthread_create(&refresh_bt_icon_thread, NULL, refresh_bt_icon_thread_func, NULL);
}

static void populate_bt_screen(void); /* defined with the rest of the Bluetooth settings screen, below */
static bool bt_toggle_active; /* defined with the rest of the tap-to-toggle mechanism, below -- see poll_refresh_bt_icon()'s own use of it */

static void poll_refresh_bt_icon(void) {
    if (!refresh_bt_icon_active || !refresh_bt_icon_done_flag) return;
    refresh_bt_icon_active = false;
    pthread_join(refresh_bt_icon_thread, NULL);

    /* Real-device bug report: enabling Bluetooth from the quick drawer
     * flipped the drawer icon on (quick_drawer_bt_event_cb()'s own
     * optimistic flip), then back off, then back on again. Root cause:
     * this poll runs independently, on its own periodic cadence, of the
     * user's own tap-to-toggle -- if one lands mid-flight (turning
     * Bluetooth on for real can take ~10-13s cold), refresh_bt_icon_result_
     * powered still reflects the OLD, pre-toggle state, since bt_control_
     * is_powered() genuinely hasn't changed yet. The populate_bt_screen()
     * call further down was ALREADY guarded against exactly this race (see
     * its own comment) after an earlier, identical bug report about the
     * Bluetooth settings screen's own toggle row -- but that fix only
     * covered the settings screen, not bt_is_powered_cached itself or the
     * drawer icon below, which this same stale result was still freely
     * overwriting. Skipping the whole result application while
     * bt_toggle_active leaves the optimistic flip standing undisturbed
     * everywhere, not just on the settings screen, until poll_bt_toggle()'s
     * own follow-up start_refresh_bt_icon() call lands with the real,
     * settled state once the in-flight toggle actually completes. */
    if (bt_toggle_active) return;

    bool display_powered = refresh_bt_icon_result_powered;
    if (bt_boot_suppress_active()) display_powered = false; /* see bt_boot_suppress_active()'s own comment */

    bt_is_powered_cached = display_powered;
    snprintf(bt_connected_mac_cached, sizeof(bt_connected_mac_cached), "%s", refresh_bt_icon_result_mac);
    snprintf(bt_connected_codec_cached, sizeof(bt_connected_codec_cached), "%s", refresh_bt_icon_result_codec);
    if (quick_drawer_bt_icon) {
        lv_image_set_src(quick_drawer_bt_icon, asset_path(display_powered ? "pull_down/bt_s.png" : "pull_down/bt.png"));
    }

    if (!display_powered) {
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
        /* Bluetooth screen's own toggle row + everything gated on it reads
         * bt_is_powered_cached too -- only actually needs rebuilding while
         * that screen is the one on screen, see the comment below on the
         * other populate_bt_screen() call site for why. (No bt_toggle_active
         * check needed here anymore -- the whole function already returned
         * early above while a toggle's in flight, see that comment for the
         * real-device bug this used to only half-fix.) */
        if (nav_depth > 0 && nav_stack[nav_depth - 1] == bt_screen) populate_bt_screen();
        return;
    }
    lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(bt_status_icon, asset_path(refresh_bt_icon_result_connected ? "topbar/bluetooth.png" : "topbar/bluetooth_unconnect.png"));
    if (refresh_bt_icon_result_a2dp_connected) {
        lv_obj_remove_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    }

    /* Real-device bug: the Bluetooth settings screen's device list kept
     * showing paired/connected state as of the last explicit scan forever
     * after -- populate_bt_screen() re-runs every poll tick already (right
     * below), but it only re-renders bt_scan_results, which nothing kept
     * fresh; only this function's own icon update above was ever current.
     * Update in place by MAC match rather than appending -- a device not
     * already in bt_scan_results (nothing scanned yet) still needs an
     * explicit Rescan, same as before; this only fixes staleness for
     * entries already on screen.
     *
     * Real-device bug #2 (found later): "Forget Device" (poll_bt_forget())
     * clears bt_scan_results[i].paired locally and immediately, but this
     * loop used to only ever UPDATE an entry that appeared in the fresh
     * bt_paired_states_result[] snapshot -- never explicitly clear one that
     * dropped OUT of it. A background poll that started (bt_control_
     * list_paired_states() takes several real subprocess round trips)
     * before the user hit Forget, but which HAPPENS to land afterward,
     * still carried the stale "still paired" snapshot -- reapplying it
     * here silently undid poll_bt_forget()'s own correct clear, confirmed
     * live as "forgot a device, it stopped showing up at all" (stuck
     * showing as still-paired, so filtered out of both list sections it
     * could sanely appear in). Iterating bt_scan_results and searching
     * bt_paired_states_result (inverted from the original nesting) instead
     * makes this poll's own result authoritative in BOTH directions: found
     * -> apply it, not found -> it's not paired, full stop, regardless of
     * whatever an even-more-stale direct clear or a previous poll left
     * behind.
     *
     * Guarded on bt_paired_states_count >= 0 (not -1, see
     * bt_control_list_paired_states()'s own doc comment): a failed query
     * means no fresh data at all this cycle, not "0 devices are paired" --
     * applying the "not found -> clear it" half on a failed query would
     * incorrectly wipe every device's real paired state over a transient
     * subprocess hiccup. Skipping the whole merge (leaving bt_scan_results
     * exactly as it was) just means this cycle contributes nothing, same as
     * if the poll simply hadn't run yet -- the next successful cycle
     * catches up normally. */
    if (bt_paired_states_count >= 0) {
        for (int j = 0; j < bt_scan_result_count; j++) {
            bool found = false;
            for (int i = 0; i < bt_paired_states_count; i++) {
                if (strcmp(bt_scan_results[j].mac, bt_paired_states_result[i].mac) == 0) {
                    bt_scan_results[j].paired = bt_paired_states_result[i].paired;
                    bt_scan_results[j].connected = bt_paired_states_result[i].connected;
                    found = true;
                    break;
                }
            }
            if (!found) {
                bt_scan_results[j].paired = false;
                bt_scan_results[j].connected = false;
            }
        }
    }
    /* Real-device bug: this used to call populate_bt_screen() unconditionally
     * every ~5s poll tick regardless of which screen was actually on
     * screen -- lv_obj_clean() + rebuilding every device row (icons,
     * labels, buttons) is real LVGL work, confirmed live as visible UI
     * tearing/animation stutter on OTHER screens (player, home, ...) the
     * whole time Bluetooth was on, not just while the Bluetooth screen was
     * open. bt_scan_results itself is still kept fresh above every cycle
     * regardless (cheap, no LVGL calls) -- only the actual widget rebuild is
     * gated, and open_bluetooth_screen() already calls populate_bt_screen()
     * itself once on entry, so the screen is never stale when the user
     * actually opens it. (No bt_toggle_active check needed here either --
     * same reasoning as the other populate_bt_screen() call site above,
     * this function's own !display_powered branch.) */
    if (nav_depth > 0 && nav_stack[nav_depth - 1] == bt_screen) populate_bt_screen();

    /* Real-device bug: pairing/connecting Bluetooth headphones worked (this
     * poll's own refresh_bt_icon_result_connected went true), but no audio
     * ever played -- see audio_set_bt_output()'s doc comment in audio.h for
     * the root cause (this app's output was hardcoded to local hardware,
     * with no path to bluealsa at all). Gated on bt_dac_mode_enabled being
     * off: DAC mode runs bluealsa as a2dp-sink (receiving audio FROM a
     * phone), not a2dp-source, so there's no source profile for this app's
     * own playback to route into while DAC mode has that swapped out (see
     * bt_control_apply_output_settings()'s own comment on the two being
     * mutually exclusive). */
    bool use_bt_output = refresh_bt_icon_result_connected && !current_settings.bt_dac_mode_enabled;
    audio_set_bt_output(use_bt_output);

    /* Same gating as audio_set_bt_output() right above -- real-device bug
     * report: once Bluetooth output itself worked, the headphones' own
     * volume buttons had no effect on this app and vice versa. See
     * bt_control_source_volume_sync_start()'s own comment in
     * bluetooth_control.c for why this doesn't double-attenuate on top of
     * this app's own volume taper. */
    if (use_bt_output) {
        bt_control_source_volume_sync_start();
    } else {
        bt_control_source_volume_sync_stop();
    }

    /* Same gating again -- see bt_control_output_disconnect_watch_start()'s
     * own comment (bluetooth_control.h) for what this buys over the plain
     * ~5s poll below (refresh_bt_icon_result_a2dp_connected itself): a real
     * disconnect surfaces in well under a second instead of up to ~17s. */
    if (use_bt_output) {
        bt_control_output_disconnect_watch_start();
    } else {
        bt_control_output_disconnect_watch_stop();
    }

    /* Same gating again -- real-device bug report: "can't use bluetooth
     * headphones while on USB DAC". usb_dac_bridge.c's own output stream
     * used to always go straight to local hardware regardless of this;
     * now it shares the same audio_output module local playback uses (see
     * usb_dac_bridge_set_bt_output()'s own doc comment), so it needs the
     * same signal. Harmless to call when USB DAC mode isn't even active
     * (the bridge just isn't running, so this only updates a flag it'll
     * read next time it starts). */
    usb_dac_bridge_set_bt_output(use_bt_output);
}

/* Transient volume popup (Task #28 / closes Task #17): built once, hidden,
 * on the top layer -- same reasoning as the status bar, but shown only for
 * a couple of seconds after the hardware volume buttons change the level,
 * then auto-hidden, instead of a permanently visible slider. */
static lv_obj_t * volume_popup;
static lv_obj_t * volume_popup_track;
static lv_timer_t * volume_popup_hide_timer;

static void volume_popup_hide_timer_cb(lv_timer_t * timer) {
    (void) timer;
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(volume_popup_hide_timer);
}

/* Real-device feedback: the popup's own slider used to be display-only
 * (hw volume buttons the only way to change it) -- this makes it drag/
 * touch-able too, same PRESSED/VALUE_CHANGED/RELEASED shape as the player
 * screen's own volume_slider_event_cb, plus keeping that slider and the
 * topbar readout in sync since all three show the same value. */
static void volume_popup_track_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_PRESSED) {
        /* Stop the 1.5s auto-hide countdown while a finger's still on it --
         * otherwise a slow drag could get hidden out from under the user
         * mid-interaction. */
        lv_timer_pause(volume_popup_hide_timer);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        audio_set_volume((float) percent / 100.0f); /* live feedback while dragging */
        lv_slider_set_value(volume_slider, percent, LV_ANIM_OFF);
        refresh_volume_topbar(percent);
    } else if (code == LV_EVENT_RELEASED) {
        /* Only persist once the drag settles, not on every intermediate
         * tick -- same as volume_slider_event_cb. Resets the hide timer
         * fresh from here rather than leaving it paused. */
        current_settings.volume = (float) percent / 100.0f;
        settings_save(&current_settings);
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    }
}

static void build_volume_popup(void) {
    lv_obj_t * top = lv_layer_top();

    volume_popup = lv_obj_create(top);
    lv_obj_set_size(volume_popup, 440, 60);
    /* Right below the status bar, not floating mid-screen. */
    lv_obj_align(volume_popup, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + 12);
    /* bg.png (and vol_bg.png/vol_progress.png/cursor.png below) are
     * rounded-rect sprites with genuinely transparent corners (verified:
     * corner pixel alpha is 0 in the source PNGs). bg_opa and bg_image_opa
     * are independent LVGL v9 style properties (lv_obj_draw.c gates them
     * separately) -- the previous fix set an opaque bg_color to cover
     * LVGL's default light background, but that just replaced it with an
     * opaque BLACK square showing through those same transparent corners.
     * Turning bg_opa fully off removes the rect fill entirely; the
     * background IMAGE still draws (bg_image_opa defaults to COVER on its
     * own, per lv_style_prop_get_default()), so the transparent corners
     * now correctly show whatever is actually behind the popup. */
    lv_obj_set_style_bg_opa(volume_popup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_image_src(volume_popup, asset_path("volume/bg.png"), 0);
    lv_obj_set_style_border_width(volume_popup, 0, 0);
    /* The real cause of the icon/slider overlap: lv_obj_create() pulls in
     * the default theme's "card" style, which sets pad_all to PAD_DEF
     * (16-24px depending on disp_size, lv_theme_default.c) -- left
     * un-zeroed, that shrinks the box speaker_icon's LEFT_MID/+20 and
     * volume_popup_track's RIGHT_MID/-20 actually align within on each
     * side, which was enough to make the two visually touch/overlap on a
     * real-device screenshot despite the 32px gap the raw 440/20/340/20
     * numbers below suggest. Zeroing it makes this object's content box
     * the full 440x60 those numbers assume. */
    lv_obj_set_style_pad_all(volume_popup, 0, 0);
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * speaker_icon = lv_image_create(volume_popup);
    lv_image_set_src(speaker_icon, asset_path("volume/vol.png"));
    lv_obj_align(speaker_icon, LV_ALIGN_LEFT_MID, 20, 0);

    volume_popup_track = lv_slider_create(volume_popup);
    lv_obj_set_size(volume_popup_track, 340, 12);
    lv_obj_align(volume_popup_track, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_slider_set_range(volume_popup_track, 0, 100);
    /* Same bg_opa-vs-bg_image_opa fix as volume_popup itself -- each part
     * (MAIN/INDICATOR/KNOB) has its own independent bg_opa/bg_color. */
    lv_obj_set_style_bg_opa(volume_popup_track, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volume_popup_track, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(volume_popup_track, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_bg_image_src(volume_popup_track, asset_path("volume/vol_bg.png"), LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(volume_popup_track, asset_path("volume/vol_progress.png"), LV_PART_INDICATOR);
    lv_obj_set_style_bg_image_src(volume_popup_track, asset_path("volume/cursor.png"), LV_PART_KNOB);
    /* Real-device bug report: accent color didn't apply here -- see
     * apply_accent_color()'s own comment on why an image-art slider needs
     * bg_image_recolor, not just bg_color. */
    lv_obj_add_style(volume_popup_track, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(volume_popup_track, &style_accent, LV_PART_KNOB);
    lv_obj_set_style_width(volume_popup_track, 30, LV_PART_KNOB);
    lv_obj_set_style_height(volume_popup_track, 30, LV_PART_KNOB);
    /* Clickable by default (lv_slider_create()) -- drag/touch-able, not
     * just driven by the hw volume buttons, see volume_popup_track_event_cb. */
    lv_obj_add_event_cb(volume_popup_track, volume_popup_track_event_cb, LV_EVENT_ALL, NULL);

    volume_popup_hide_timer = lv_timer_create(volume_popup_hide_timer_cb, 1500, NULL);
    lv_timer_pause(volume_popup_hide_timer);
}

/* Android-style home indicator: a small pill fixed to the bottom edge,
 * living on lv_layer_top() (drawn above every screen, same trick as the
 * status bar) so a swipe-up starting there is always caught by THIS object
 * instead of whatever scrollable list happens to be underneath it.
 *
 * Real-device bug report: plain swipe-up-anywhere (screen_gesture_event_cb()
 * above) didn't work on any screen with a scrollable list -- LVGL claims a
 * vertical drag as a list SCROLL before it ever escalates to a gesture (see
 * enable_gesture_bubble_recursive()'s own comment: that only affects whether
 * a completed gesture bubbles up, not whether one gets generated in the
 * first place on an object that can still scroll in that direction). A
 * horizontal swipe doesn't have this problem since none of these lists
 * scroll sideways, which is why back/forward navigation swipes were never
 * affected. Deliberately NOT shrinking every screen's own content height to
 * visually reserve this strip too (touches this many build_XXX_screen()
 * call sites for a first pass) -- it overlays the very bottom of scrollable
 * content instead, same tradeoff plenty of real apps make with a floating
 * gesture bar.
 *
 * Second real-device bug report: an LV_EVENT_GESTURE handler directly on
 * this band (the first attempt) never fired at all. Same root cause already
 * documented and fixed for the quick drawer's own edge-swipe (see
 * poll_quick_drawer_drag()'s own long comment): LVGL's gesture detection is
 * unreliable for this exact "small dedicated edge zone" shape of
 * interaction on real hardware. Tracking is done there instead, by polling
 * the indev's raw position every tick alongside the drawer's own drag
 * tracking (home_swipe_tracking/home_swipe_start_y/home_swipe_triggered,
 * declared there) -- sidesteps LVGL's hit-testing/gesture-escalation
 * machinery entirely, the same fix that made the drawer's own swipe
 * reliable. */
#define HOME_INDICATOR_BAND_HEIGHT 34
static lv_obj_t * home_indicator_band;

static void build_home_indicator_bar(void) {
    lv_obj_t * top = lv_layer_top();

    home_indicator_band = lv_obj_create(top);
    lv_obj_remove_style_all(home_indicator_band);
    lv_obj_set_size(home_indicator_band, lv_pct(100), HOME_INDICATOR_BAND_HEIGHT);
    lv_obj_align(home_indicator_band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(home_indicator_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_CLICKABLE); /* claims touches in this strip before any list underneath can -- the actual swipe-up trigger is poll_quick_drawer_drag()'s raw position polling, not a click/gesture event on this object */

    /* The visible pill itself -- plain light-gray rounded bar, matching
     * Android's own gesture-nav home indicator. */
    lv_obj_t * pill = lv_obj_create(home_indicator_band);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 120, 4);
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pill, lv_color_make(220, 220, 220), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_60, 0);
    lv_obj_set_style_radius(pill, 2, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE); /* purely visual -- home_indicator_band above is what's actually clickable */

    lv_obj_add_flag(home_indicator_band, current_settings.swipe_up_home_enabled ? 0 : LV_OBJ_FLAG_HIDDEN);
}

/* Generic transient error/status toast, top layer, auto-hides after 2.5s --
 * same shape as volume_popup above. Reusable anywhere a background op can
 * fail with something worth telling the user about; nothing like this
 * existed before (see poll_subsonic_download()'s and
 * subsonic_connect_row_cb's own "no error-toast UI exists yet" notes) --
 * first real use is Wi-Fi/Bluetooth connect failures. */
static lv_obj_t * error_toast;
static lv_obj_t * error_toast_label;
static lv_timer_t * error_toast_hide_timer;

static void error_toast_hide_timer_cb(lv_timer_t * timer) {
    (void) timer;
    lv_obj_add_flag(error_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(error_toast_hide_timer);
}

static void build_error_toast(void) {
    lv_obj_t * top = lv_layer_top();

    error_toast = lv_obj_create(top);
    lv_obj_set_size(error_toast, 400, 70);
    lv_obj_align(error_toast, LV_ALIGN_CENTER, 0, -180);
    lv_obj_set_style_radius(error_toast, 16, 0);
    lv_obj_set_style_bg_color(error_toast, lv_color_make(40, 20, 20), 0);
    lv_obj_set_style_bg_opa(error_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(error_toast, 0, 0);
    lv_obj_remove_flag(error_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(error_toast, LV_OBJ_FLAG_HIDDEN);

    error_toast_label = lv_label_create(error_toast);
    lv_obj_set_width(error_toast_label, lv_pct(90));
    lv_label_set_long_mode(error_toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(error_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(error_toast_label, lv_color_make(255, 200, 200), 0);
    lv_obj_set_style_text_font(error_toast_label, ui_size_20, 0);
    lv_obj_center(error_toast_label);

    error_toast_hide_timer = lv_timer_create(error_toast_hide_timer_cb, 2500, NULL);
    lv_timer_pause(error_toast_hide_timer);
}

static void show_error_toast(const char * msg) {
    lv_label_set_text(error_toast_label, msg);
    lv_obj_remove_flag(error_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(error_toast);
    lv_timer_reset(error_toast_hide_timer);
    lv_timer_resume(error_toast_hide_timer);
}

/* Fully automatic, no Settings entry -- meant to feel like the wired
 * headphone jack (refresh_headphone_icon() above), not a mode the user
 * switches into (unlike Storage/USB DAC/ADB in the manual USB Mode
 * screen). usb_audio_output_is_connected() is a plain /proc file read (no
 * subprocess), same cheap class of check as headphone_is_connected()'s own
 * direct sysfs read, so this is safe to call directly on the UI thread at
 * the same low cadence as the wifi/Bluetooth polls (see their own
 * WIFI_POLL_TICKS call site) rather than needing its own background
 * thread. Toast fires only on the actual connect transition (was_connected
 * tracked across calls), matching how "Paused: headphones disconnected"
 * only fires once per real disconnect rather than every poll tick. */
static void poll_usb_audio_output(void) {
    static bool was_connected = false;
    char alsa_device[32];
    bool connected = usb_audio_output_is_connected(alsa_device, sizeof(alsa_device));

    if (connected && !was_connected) show_error_toast("USB audio device detected");
    was_connected = connected;

    audio_set_usb_output(connected, alsa_device);
    if (connected) {
        lv_obj_remove_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Neutral-styled sibling of show_error_toast() -- that one's red color
 * scheme and short 2.5s/400x70 sizing fit a brief failure message, not a
 * longer explanatory one (first use: Car Mode's own explanation on
 * enabling). Bigger box for wrapping, 5s so there's time to actually read
 * it, no error coloring since nothing failed. */
static lv_obj_t * info_toast;
static lv_obj_t * info_toast_label;
static lv_timer_t * info_toast_hide_timer;

static void info_toast_hide_timer_cb(lv_timer_t * timer) {
    (void) timer;
    lv_obj_add_flag(info_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(info_toast_hide_timer);
}

static void build_info_toast(void) {
    lv_obj_t * top = lv_layer_top();

    info_toast = lv_obj_create(top);
    lv_obj_set_size(info_toast, 420, 140);
    lv_obj_align(info_toast, LV_ALIGN_CENTER, 0, -160);
    lv_obj_set_style_radius(info_toast, 16, 0);
    lv_obj_add_style(info_toast, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(info_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(info_toast, 0, 0);
    lv_obj_remove_flag(info_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(info_toast, LV_OBJ_FLAG_HIDDEN);

    info_toast_label = lv_label_create(info_toast);
    lv_obj_set_width(info_toast_label, lv_pct(90));
    lv_label_set_long_mode(info_toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(info_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(info_toast_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(info_toast_label, ui_size_20, 0);
    lv_obj_center(info_toast_label);

    info_toast_hide_timer = lv_timer_create(info_toast_hide_timer_cb, 5000, NULL);
    lv_timer_pause(info_toast_hide_timer);
}

static void show_info_toast(const char * msg) {
    lv_label_set_text(info_toast_label, msg);
    lv_obj_remove_flag(info_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(info_toast);
    lv_timer_reset(info_toast_hide_timer);
    lv_timer_resume(info_toast_hide_timer);
}

/* Shows the popup at the given 0-100 level and (re)starts its 1.5s
 * auto-hide countdown -- called every time the level actually changes, so
 * it stays visible for as long as the user keeps pressing volume buttons. */
static void show_volume_popup(int32_t percent) {
    lv_slider_set_value(volume_popup_track, percent, LV_ANIM_OFF);
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(volume_popup_hide_timer);
    lv_timer_resume(volume_popup_hide_timer);
}

/* Quick-access drawer (Android-style notification-shade convention): slides
 * down over the whole screen from a swipe-down starting near the status
 * bar. Real pull_down/ theme2 assets throughout. Every row-1 icon (Wifi/
 * Bluetooth, mirroring the same wifi_status.c/bluetooth_control.c state as
 * the main status bar; crossfade; sleep timer) and the now-playing card
 * (real playback state, reusing the exact same callbacks as the player
 * screen's own transport buttons) are backed by real functionality.
 * QUICK_DRAWER_ANIM_MS/TRIGGER_ZONE are defined earlier, alongside
 * screen_gesture_event_cb, which needs the latter for its
 * swipe-down-near-the-top-edge check. */

/* Real control now -- see crossfade_switch_event_cb (Settings > Crossfade)
 * for the other half of this same toggle; both read/write
 * current_settings.crossfade_enabled and both keep the OTHER one's icon/
 * switch state in sync (refresh_quick_drawer_crossfade_icon() /
 * sync_settings_crossfade_toggle()), so whichever one you use, the other
 * reflects it next time you look. Uses pull_down/fade.png/fade_s.png (a
 * real stock asset, dedicated to this -- confirmed by name, unlike
 * gain_h.png/gain_l.png this used to show, which was always a placeholder
 * for "output gain", never a real control). */
static lv_obj_t * quick_drawer_crossfade_icon;
static void refresh_quick_drawer_crossfade_icon(void) {
    if (!quick_drawer_crossfade_icon) return;
    lv_image_set_src(quick_drawer_crossfade_icon,
                     asset_path(current_settings.crossfade_enabled ? "pull_down/fade_s.png" : "pull_down/fade.png"));
}

/* Settings > Playback's own Crossfade toggle row (build_settings_playback_screen(),
 * captured via pill_list_item_t's out_toggle_img) -- that screen is built
 * once at gui_init() and never rebuilt, so its toggle's sprite/LV_STATE_CHECKED
 * only ever reflects whatever current_settings.crossfade_enabled was at
 * that one build time unless something explicitly pokes it afterward.
 * Real-device bug report: toggling crossfade from the quick drawer left
 * this row showing the old (now wrong) state the next time Settings >
 * Playback was opened -- the settings->drawer direction already worked
 * (crossfade_switch_event_cb calls refresh_quick_drawer_crossfade_icon()),
 * but nothing called the reverse. */
static lv_obj_t * settings_crossfade_toggle_img;
static void sync_settings_crossfade_toggle(void) {
    if (!settings_crossfade_toggle_img) return;
    lv_image_set_src(settings_crossfade_toggle_img,
                     asset_path(current_settings.crossfade_enabled ? "settings/on.png" : "settings/off.png"));
    if (current_settings.crossfade_enabled) lv_obj_add_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
}

static void quick_drawer_crossfade_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.crossfade_enabled = !current_settings.crossfade_enabled;
    audio_set_crossfade_enabled(current_settings.crossfade_enabled);
    settings_save(&current_settings);
    refresh_quick_drawer_crossfade_icon();
    sync_settings_crossfade_toggle();
}

/* Defined later, alongside the rest of the transport-button wiring --
 * forward-declared here since poll_sleep_timer() below needs it on
 * expiry. */
static void toggle_play_pause(void);

/* Sleep timer: tapping this icon arms/disarms a real countdown (current
 * duration from current_settings.sleep_timer_minutes, configurable via
 * Settings > Sleep Timer) that pauses playback once it elapses -- see
 * poll_sleep_timer() (update_timer_cb) for the actual countdown/expiry
 * logic. quick_drawer_sleep_label shows the remaining time below the icon
 * while armed, per real-device feedback wanting a visible countdown, not
 * just an on/off glow -- hidden the rest of the time. Session-only state
 * (sleep_timer_active): arming isn't persisted, so a relaunch never resumes
 * a stale countdown from a previous session. */
static bool sleep_timer_active = false;
static uint32_t sleep_timer_start_tick = 0;
static lv_obj_t * quick_drawer_sleep_icon;
static lv_obj_t * quick_drawer_sleep_label;
static void quick_drawer_sleep_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    sleep_timer_active = !sleep_timer_active;
    if (sleep_timer_active) {
        sleep_timer_start_tick = lv_tick_get();
        lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch_s.png"));
        lv_label_set_text_fmt(quick_drawer_sleep_label, "%dm", current_settings.sleep_timer_minutes);
        lv_obj_remove_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
        lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Called every update_timer_cb tick (500ms). Cheap no-op when not armed. */
static void poll_sleep_timer(void) {
    if (!sleep_timer_active) return;

    uint32_t total_ms = (uint32_t) current_settings.sleep_timer_minutes * 60000;
    uint32_t elapsed_ms = lv_tick_elaps(sleep_timer_start_tick);

    if (elapsed_ms >= total_ms) {
        sleep_timer_active = false;
        if (audio_is_playing()) toggle_play_pause(); /* pause, not stop -- resumable, same as any other pause */
        lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
        lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Round up so the label never shows "0m" for the last, still-live
     * sub-minute stretch -- counts down 15,14,...,1 then disarms above
     * rather than ever displaying a misleading zero. */
    int remaining_min = (int) ((total_ms - elapsed_ms + 59999) / 60000);
    lv_label_set_text_fmt(quick_drawer_sleep_label, "%dm", remaining_min);
}

static void quick_drawer_anim_y_cb(void * var, int32_t v) {
    lv_obj_set_y((lv_obj_t *) var, v);
}

static void open_quick_drawer(void) {
    if (quick_drawer_open) return;
    quick_drawer_open = true;
    refresh_quick_drawer_brightness(); /* see its own comment -- keeps the slider from showing a stale pre-screen-off value */
    lv_obj_move_foreground(quick_drawer); /* above regular screens/volume popup while showing */
    /* ...but the status bar (clock/battery/wifi/bt) stays above THAT --
     * real-hardware feedback wanted it to stay visible/readable the whole
     * time the drawer is open, not get covered by it. quick_drawer's own
     * pull_down/bg.png is opaque black for the first ~59px anyway (measured
     * directly off the asset), so the status bar ends up sitting on that as
     * a backdrop rather than on anything from the screen underneath. */
    lv_obj_move_foreground(status_bar_band);
    /* Real-device bug report: "drawer animation is sluggish" -- root cause
     * was two (or more) of these animations running concurrently, not a
     * rendering-speed problem. lv_anim_start() only dedupes same-var/
     * same-exec_cb animations via its own early_apply path (see
     * remove_concurrent_anims() in lv_anim.c), which this never opts into
     * -- so a second open/close triggered before a prior 120ms animation
     * finished (an easy thing to do with a quick double-flick, or
     * re-grabbing the drawer to drag again right after a release-snap)
     * left BOTH animations alive, each calling quick_drawer_anim_y_cb with
     * its own diverging interpolated Y every tick and visibly fighting
     * each other -- indistinguishable from slow/janky rendering unless you
     * know to look for it. Explicitly cancelling any prior animation on
     * this exact (var, exec_cb) pair before starting a new one removes
     * that race entirely. */
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, quick_drawer);
    lv_anim_set_values(&a, lv_obj_get_y(quick_drawer), 0);
    lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
    lv_anim_set_exec_cb(&a, quick_drawer_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void close_quick_drawer(void) {
    if (!quick_drawer_open) return;
    quick_drawer_open = false;
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb); /* see open_quick_drawer()'s own comment on why */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, quick_drawer);
    lv_anim_set_values(&a, lv_obj_get_y(quick_drawer), -h);
    lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
    lv_anim_set_exec_cb(&a, quick_drawer_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static bool library_rescan_active; /* defined with the rest of the Update Music Database rescan, below -- see poll_quick_drawer_drag()'s own use of it */

static bool quick_drawer_drag_tracking = false;
static bool quick_drawer_was_pressed = false;
static int32_t quick_drawer_drag_touch_start_y = 0;
static int32_t quick_drawer_drag_panel_start_y = 0;
static int32_t quick_drawer_last_velocity = 0;
#define QUICK_DRAWER_FLICK_VELOCITY 12 /* px/tick (~750px/s at the ~16ms poll rate) -- fast enough to read as an intentional flick */
#define QUICK_DRAWER_DRAG_DEADZONE 10 /* matches LVGL's own LV_INDEV_DEF_SCROLL_LIMIT -- see poll_quick_drawer_drag()'s comment */

/* Home-indicator swipe-up tracking -- see build_home_indicator_bar()'s own
 * comment for why this rides alongside the quick drawer's own drag tracking
 * in poll_quick_drawer_drag() below instead of an LV_EVENT_GESTURE handler.
 * Independent of quick_drawer_drag_tracking above (a press can only ever be
 * the start of one or the other, decided by where it lands). */
static bool home_swipe_tracking = false;
static int32_t home_swipe_start_y = 0;
static bool home_swipe_triggered = false; /* fires nav_reset_to_home() at most once per press, even if the finger keeps moving past the threshold */
#define HOME_SWIPE_UP_THRESHOLD 40 /* px the finger must move up from its start point before this counts as a swipe, not a stray tap in the band */

/* Swipe-left-to-player tracking -- same "raw indev polling, own dedicated
 * fast timer" reasoning as poll_quick_drawer_drag()'s own doc comment,
 * replacing the old LV_EVENT_GESTURE-based instant cut (see
 * screen_gesture_event_cb()'s own comment on why that couldn't just be
 * left running alongside this). Unlike the drawer's drag (claimed
 * instantly, by which zone the press started in) or the home-swipe
 * (claimed instantly, by starting inside a fixed band), this can start
 * ANYWHERE on screen -- matching the gesture it replaces -- so which
 * press this is can't be decided at press-down; it's provisional
 * (player_swipe_candidate) until enough movement accumulates to judge
 * direction, then either confirmed (player_swipe_tracking, the overlay
 * gets built and starts following the finger) or abandoned, letting the
 * press fall through as whatever else it actually was (a tap, a vertical
 * scroll, or the existing swipe-RIGHT-to-back gesture, still handled the
 * old event-based way since only entering the player needed this). */
static bool player_swipe_candidate = false;
static bool player_swipe_tracking = false;
static int32_t player_swipe_touch_start_x = 0;
static int32_t player_swipe_touch_start_y = 0;
static int32_t player_swipe_last_v = 0; /* last x actually applied to img_from, for per-tick velocity -- same idea as quick_drawer_last_velocity */
static int32_t player_swipe_last_velocity = 0;
static slide_transition_ctx_t * player_swipe_ctx = NULL;
#define PLAYER_SWIPE_DEADZONE 20 /* px before judging direction -- comfortably under LVGL's own ~50px built-in gesture threshold (LV_INDEV_DEF_GESTURE_LIMIT) so this always claims a genuine left-swipe before LVGL's own dormant gesture recognition would have */
#define PLAYER_SWIPE_FLICK_VELOCITY 12 /* same scale/reasoning as QUICK_DRAWER_FLICK_VELOCITY */

/* Forward declarations -- both fully built later in this file, needed here
 * so poll_quick_drawer_drag() below can exclude the home-swipe gesture
 * while either DAC overlay is active (see its own comment on why). */
static lv_obj_t * bt_dac_overlay_screen;
static lv_obj_t * usb_dac_overlay_screen;

/* Drives the quick drawer's open/close by directly following the finger's
 * raw Y position every tick -- "dynamic", per real-hardware feedback,
 * rather than an instant threshold-triggered animation -- snapping to fully
 * open or fully closed only once the finger actually lifts.
 *
 * Polled from its own dedicated fast lv_timer (see gui_init()) rather than
 * update_timer_cb's existing 500ms one, or driven by touch events, for two
 * separate reasons discovered in that order: first, LV_EVENT_PRESSED/
 * RELEASED/CLICKED/LONG_PRESSED are the only events LVGL ever dispatches to
 * an indev's own event list regardless of which object was actually hit
 * (confirmed directly in lv_indev.c's send_event()) -- LV_EVENT_PRESSING is
 * deliberately NOT among them, so a first attempt (an indev-wide
 * LV_EVENT_PRESSING handler) silently never fired at all, and the attempt
 * before THAT (the drawer's own LV_EVENT_GESTURE handler, relying on
 * enable_gesture_bubble_recursive()) got swallowed whenever the swipe
 * started on the 300px-wide brightness slider, deliberately excluded from
 * gesture-bubbling for the same reason every other screen excludes its own
 * sliders. Reading the indev's raw position directly instead sidesteps
 * hit-testing entirely. Second, once switched to polling, real-device
 * testing showed the drag still wasn't followed smoothly: it turned out
 * update_timer_cb's 500ms period is far slower than a typical swipe (well
 * under 300ms start to finish), so it was only ever sampling zero or one
 * point per gesture -- hence this gets its own ~60fps timer instead. */
/* Not just lv_indev_get_next(NULL) -- the target build only ever registers
 * the one touchscreen indev, but the host simulator also registers a
 * keyboard indev (see main.c's lv_sdl_keyboard_create()), and there's no
 * guarantee which one comes back first. Explicitly finding the
 * pointer-type one is correct on both. Shared by every raw-touch-polling
 * timer in this file (poll_quick_drawer_drag(), poll_az_index_drag()) --
 * see poll_quick_drawer_drag()'s own doc comment for why polling raw indev
 * state is necessary here at all instead of LVGL's own touch events. */
static lv_indev_t * find_pointer_indev(void) {
    for (lv_indev_t * candidate = lv_indev_get_next(NULL); candidate; candidate = lv_indev_get_next(candidate)) {
        if (lv_indev_get_type(candidate) == LV_INDEV_TYPE_POINTER) return candidate;
    }
    return NULL;
}

/* Same drag-adjust widget set enable_gesture_bubble_recursive() excludes
 * from swipe-bubbling (sliders/switches/dropdowns/rollers), checked here
 * for the player-swipe candidate below -- that check is unrelated to
 * GESTURE_BUBBLE (this whole file's raw-indev-polling swipe detectors
 * don't go through LVGL's event/bubbling system at all, per this
 * function's own doc comment on why). Real-device feedback: dragging the
 * Idle Shutdown timeout slider leftward (its natural adjustment
 * direction) was randomly getting hijacked mid-drag into a "swipe to
 * player screen" transition once the horizontal movement crossed
 * PLAYER_SWIPE_DEADZONE, abandoning the slider adjustment -- this was
 * never about GESTURE_BUBBLE at all, it's a completely separate polling
 * loop with no per-widget exclusions of its own. lv_indev_get_active_obj()
 * is the object LVGL's own input processing most recently hit-tested a
 * press against, so this reflects whatever's actually under the finger
 * right now, not just this timer's own idea of screen layout. */
static bool active_press_is_over_drag_adjust_widget(void) {
    lv_obj_t * act = lv_indev_get_active_obj();
    while (act) {
        if (lv_obj_check_type(act, &lv_slider_class) ||
            lv_obj_check_type(act, &lv_switch_class) ||
            lv_obj_check_type(act, &lv_dropdown_class) ||
            lv_obj_check_type(act, &lv_roller_class)) {
            return true;
        }
        act = lv_obj_get_parent(act);
    }
    return false;
}

/* active_press_is_over_drag_adjust_widget() alone wasn't enough: real-
 * device feedback after that fix still showed a press starting on a
 * slider card's background -- near the slider but not precisely inside
 * its own hit-test box -- still hijacked into a player-swipe. Root cause:
 * every one of these cards is deliberately built WITHOUT
 * LV_OBJ_FLAG_CLICKABLE (matching finalize_screen_navigation()'s own
 * comment on plain lv_obj_create() objects), so a press on the card's
 * background hit-tests straight through to the screen itself --
 * lv_indev_get_active_obj() then returns the screen, indistinguishable
 * from a press on genuinely empty space that SHOULD navigate. Each of
 * these cards already marks itself as a swipe dead zone by having its
 * own LV_OBJ_FLAG_GESTURE_BUBBLE removed (for the separate GESTURE-event-
 * based back/down-swipe path) -- register_swipe_dead_zone() reuses that
 * same set of objects for this unrelated raw-polling path, checked by
 * raw point-in-rect instead of by hit-tested object identity so it
 * doesn't matter whether the card itself is clickable.
 *
 * Widened from 8 to 16: the original 8 slots are all native sliders (built
 * once at startup, live forever). plugin.show_settings_list()'s own slider
 * rows (gui_plugin_show_settings_list()) add real, bounded headroom on top
 * -- PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE pool slots x PLUGIN_SETTINGS_
 * LIST_MAX_SLIDERS sliders each (plugin_manager.h) -- not unbounded growth,
 * since a pool slot's own slider cards are unregistered (see
 * unregister_swipe_dead_zone() below) before that slot is ever repopulated. */
#define SWIPE_DEAD_ZONE_MAX 16
static lv_obj_t * swipe_dead_zones[SWIPE_DEAD_ZONE_MAX];
static int swipe_dead_zone_count = 0;

static void register_swipe_dead_zone(lv_obj_t * obj) {
    if (swipe_dead_zone_count < SWIPE_DEAD_ZONE_MAX) swipe_dead_zones[swipe_dead_zone_count++] = obj;
}

/* Compact-remove by pointer identity -- pairs with register_swipe_dead_zone()
 * above for objects that DON'T live forever (unlike every native slider
 * card, which registers once at startup and never needs to unregister). A
 * plugin.show_settings_list() pool slot's slider cards are deleted and
 * recreated on every call that reuses that slot (lv_obj_clean(), see
 * gui_plugin_show_settings_list()) -- calling this for each of a slot's own
 * previously-registered cards BEFORE that lv_obj_clean() runs is required,
 * not just tidy: point_in_swipe_dead_zone()'s own lv_obj_get_screen(obj) !=
 * lv_screen_active() guard still needs `obj` to be a live pointer to
 * dereference, so leaving a freed card's pointer in this array would be a
 * use-after-free on the next swipe check, not a graceful skip. No-op if obj
 * isn't currently registered. */
static void unregister_swipe_dead_zone(lv_obj_t * obj) {
    for (int i = 0; i < swipe_dead_zone_count; i++) {
        if (swipe_dead_zones[i] == obj) {
            swipe_dead_zones[i] = swipe_dead_zones[swipe_dead_zone_count - 1];
            swipe_dead_zone_count--;
            return;
        }
    }
}

static bool point_in_swipe_dead_zone(lv_point_t p) {
    for (int i = 0; i < swipe_dead_zone_count; i++) {
        lv_obj_t * obj = swipe_dead_zones[i];
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_screen(obj) != lv_screen_active()) continue;
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        if (p.x >= area.x1 && p.x <= area.x2 && p.y >= area.y1 && p.y <= area.y2) return true;
    }
    return false;
}

static bool player_swipe_press_excluded(lv_point_t p) {
    return active_press_is_over_drag_adjust_widget() || point_in_swipe_dead_zone(p);
}

static void poll_quick_drawer_drag(lv_timer_t * timer) {
    (void) timer;
    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    if (pressed && !quick_drawer_was_pressed) {
        /* Cancel any release-snap animation still in flight -- re-grabbing
         * the drawer right after a flick (within its 120ms animation
         * window) used to leave that old animation alive, fighting this
         * new drag's own direct lv_obj_set_y() calls every tick (same
         * underlying issue as open_quick_drawer()'s own comment on
         * concurrent animations). Harmless/cheap no-op when nothing's
         * actually animating. */
        lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);

        /* A new press just started. If the drawer's already open, any press
         * anywhere could be the start of a close-drag -- a plain tap on a
         * button/slider still works fine, since a tap has ~0 movement and
         * this only ever repositions the drawer by that same ~0 delta. If
         * it's closed, only a press starting near the very top edge
         * counts, so e.g. scrolling a list elsewhere never accidentally
         * drags it open. Panel start reference is the drawer's ACTUAL
         * current Y (not a fixed 0/-h assumption) -- with the animation
         * just cancelled above, that could be mid-flight anywhere between
         * fully open and fully closed, not only at one of the two
         * endpoints. */
        if (quick_drawer_open) {
            quick_drawer_drag_tracking = true;
            quick_drawer_drag_panel_start_y = lv_obj_get_y(quick_drawer);
        } else if (p.y <= QUICK_DRAWER_TRIGGER_ZONE && !library_rescan_active) {
            /* !library_rescan_active -- opening the drawer mid-rescan reads
             * now-playing song state (quick_drawer_title_label/_artist_label
             * etc.) from the same all_songs/all_song_tags arrays
             * library_scan_once() frees and reallocates while a rescan runs
             * (see free_library_scan_state()'s own comment) -- confirmed
             * live as a crash. Only blocks starting a NEW open-drag; if the
             * drawer somehow got dragged open right as a rescan started, the
             * quick_drawer_open branch above still lets it be dragged closed
             * again, which touches none of that data. */
            quick_drawer_drag_tracking = true;
            quick_drawer_drag_panel_start_y = lv_obj_get_y(quick_drawer);
            lv_obj_move_foreground(quick_drawer); /* above regular screens/volume popup while dragging into view */
            lv_obj_move_foreground(status_bar_band); /* but the status bar stays above THAT -- see open_quick_drawer()'s comment */
        } else {
            quick_drawer_drag_tracking = false;
        }
        quick_drawer_drag_touch_start_y = p.y;

        /* Home-indicator swipe-up: only a press starting within the band
         * itself counts, matching the Android gesture-bar convention this
         * is modeled on (see build_home_indicator_bar()) -- a press
         * anywhere else on screen never triggers this, even if it later
         * moves upward (e.g. an aborted attempt to scroll a list).
         * Real-device bug report: with the drawer already open, ANY press
         * is presumed to be the start of a close-drag (see quick_drawer_open
         * branch just above) regardless of where on screen it starts --
         * competing for the same swipe-up gesture let a single upward drag
         * both close the drawer AND jump to Home. Excluded outright while
         * the drawer is open; the drawer itself always wins that gesture
         * there.
         * Real-device bug report: swiping up to Home while Bluetooth DAC
         * mode was active left DAC mode itself still on (bluealsa/bt-agent
         * still running, phone still "connected") with no way back to it
         * short of re-entering Settings and toggling DAC mode off and back
         * on -- only the DAC overlay's own back button is supposed to be
         * able to leave it, since that's the only path that actually tears
         * DAC mode down (see bt_dac_overlay_back_cb()). Excluded here the
         * same way the drawer already is; USB DAC mode has the identical
         * "only the back button exits" design (build_usb_dac_overlay_screen())
         * and shares the same gap fixed here.
         * Same exclusion for library_rescan_active (Settings > Update Music
         * Database) -- nav_reset_to_home() here would leave the rescan
         * thread running against arrays the home/library screens themselves
         * are about to read, same crash as the quick-drawer exclusion just
         * above. This makes the busy screen fully undismissable while a
         * rescan is in flight -- it has no swipe-to-back gesture wired
         * either (build_subsonic_downloading_screen() never calls
         * finalize_screen_navigation()), so this was the only remaining way
         * off it. */
        home_swipe_tracking = current_settings.swipe_up_home_enabled && !quick_drawer_open &&
                               lv_screen_active() != bt_dac_overlay_screen &&
                               lv_screen_active() != usb_dac_overlay_screen &&
                               !library_rescan_active &&
                               p.y >= h - HOME_INDICATOR_BAND_HEIGHT;
        home_swipe_start_y = p.y;
        home_swipe_triggered = false;

        /* Player-swipe: eligible unless this exact press already got
         * claimed by the drawer-drag above (quick_drawer_drag_tracking),
         * the drawer is open (its own close-drag owns every press while
         * open), the player screen is already the one showing (nothing
         * to swipe TO), or the press actually started on a slider/switch/
         * dropdown/roller or one of the registered slider-card dead zones
         * (see player_swipe_press_excluded()'s own comment -- dragging a
         * slider leftward, or starting the drag on its card's background,
         * was getting mistaken for this gesture). Real direction isn't
         * knowable from a single point -- judged once real movement
         * accumulates, below -- so this only marks the press as a
         * CANDIDATE, not yet a confirmed drag. Also excluded while
         * library_rescan_active, same reasoning as the quick-drawer/
         * home-swipe exclusions above -- this is the last remaining swipe
         * gesture that could navigate off the rescan's busy screen. */
        player_swipe_candidate = !quick_drawer_drag_tracking && !quick_drawer_open &&
                                  lv_screen_active() != player_screen &&
                                  !library_rescan_active &&
                                  !player_swipe_press_excluded(p);
        player_swipe_touch_start_x = p.x;
        player_swipe_touch_start_y = p.y;
        player_swipe_tracking = false;
    }

    if (pressed && home_swipe_tracking && !home_swipe_triggered && home_swipe_start_y - p.y >= HOME_SWIPE_UP_THRESHOLD) {
        home_swipe_triggered = true;
        nav_reset_to_home();
    }

    if (pressed && player_swipe_candidate && !player_swipe_tracking) {
        int32_t dx = p.x - player_swipe_touch_start_x;
        int32_t dy = p.y - player_swipe_touch_start_y;
        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;
        if (adx >= PLAYER_SWIPE_DEADZONE || ady >= PLAYER_SWIPE_DEADZONE) {
            /* Enough movement to judge direction. Horizontal-left-dominant
             * confirms it; anything else (vertical, or rightward) rules it
             * out for good -- either way, stop re-checking every tick. */
            if (dx < 0 && adx > ady) {
                player_swipe_ctx = begin_slide_transition(player_screen, true, true); /* force a real snapshot -- see begin_slide_transition()'s own comment on why the live-alias path flickers here */
                if (player_swipe_ctx) {
                    player_swipe_tracking = true;
                    player_swipe_last_v = 0;
                    player_swipe_last_velocity = 0;
                    /* Same reasoning as nav_pop()'s own lv_indev_wait_release()
                     * call -- the overlay just created sits directly under
                     * this still-down finger, and without this, the eventual
                     * release would hit whatever's now underneath at that
                     * coordinate instead (a real screen swap mid-press, same
                     * PRESS_LOST-adjacent class of bug already found and
                     * fixed once for the drawer's own icons). */
                    lv_indev_wait_release(indev);
                }
            } else if (ady > adx && !lv_indev_get_scroll_obj(indev)) {
                /* Real-device bug report: on a screen whose list is short
                 * enough to need no scrolling at all (confirmed case: the
                 * Settings home category list), a vertical swipe attempt
                 * lands here (ruled out as a player-swipe, since it isn't
                 * horizontal-left-dominant) with nothing scrollable to
                 * claim it either -- LVGL only cancels a pending click once
                 * some object's own lv_indev_get_scroll_obj() claims the
                 * drag as a real scroll, so a swipe that travelled well
                 * past PLAYER_SWIPE_DEADZONE but found nothing to scroll
                 * still resolved as a plain tap on whatever row the finger
                 * started on, firing that row's own action. Suppressing the
                 * eventual click here, same tool as the confirmed-swipe
                 * branch above, whenever the drag went far enough to
                 * clearly not be a stationary tap.
                 * Real-device bug report #2: this originally fired for ANY
                 * non-left-confirmed direction, including rightward -- which
                 * silently broke swipe-to-go-back (screen_gesture_event_cb's
                 * own LV_DIR_RIGHT handling), since lv_indev_wait_release()
                 * called this early (past this function's own 20px
                 * PLAYER_SWIPE_DEADZONE) pre-empted LVGL's own native
                 * gesture recognition before it could reach its ~50px
                 * LV_INDEV_DEF_GESTURE_LIMIT and fire the real
                 * LV_EVENT_GESTURE. Restricted to ady > adx (clearly
                 * vertical, matching the actual "tried to scroll" bug this
                 * fixes) so a horizontal drag -- rightward (back) or
                 * leftward-but-not-quite-dominant-yet -- is left completely
                 * alone here and keeps reaching LVGL's own gesture handling
                 * normally.
                 * Left alone either way (no suppression) when
                 * lv_indev_get_scroll_obj() IS set -- that's a real, working
                 * scroll already in progress, and forcing an early release
                 * there would cut its motion off mid-drag. */
                lv_indev_wait_release(indev);
            }
            player_swipe_candidate = false;
        }
    }

    if (pressed && player_swipe_tracking) {
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t v = p.x - player_swipe_touch_start_x;
        if (v > 0) v = 0;   /* never past fully-open (finger drifting back right of the start point just holds at 0) */
        if (v < -w) v = -w; /* never past fully-off (finger overshooting left of a full screen width) */
        player_swipe_last_velocity = v - player_swipe_last_v;
        player_swipe_last_v = v;
        slide_transition_anim_x_cb(player_swipe_ctx, v);
    }

    if (pressed && quick_drawer_drag_tracking) {
        /* Deadzone before actually moving the panel -- real-device feedback:
         * long-pressing a drawer icon (wifi/bt) wasn't opening its settings
         * screen at all. Root cause, confirmed by reading indev_proc_press()
         * in lv_indev.c: it re-hit-tests the SAME raw screen point on every
         * tick, and if that now resolves to a different object than the one
         * originally pressed, LVGL sends PRESS_LOST and resets the press --
         * killing the long-press timer before it can fire. Moving the whole
         * drawer (and everything on it, including the icon under the
         * finger) by even a couple of px in response to ordinary touch
         * jitter during a "held still" long-press was exactly triggering
         * that. 10px matches LVGL's own LV_INDEV_DEF_SCROLL_LIMIT -- the
         * same threshold it uses internally to tell a stationary press from
         * an intentional scroll/drag. Below it, the panel doesn't move at
         * all, so a tap or long-press on a child stays stable under the
         * finger; past it, the deadzone amount is subtracted so dragging
         * starts smoothly from zero rather than jumping. */
        int32_t raw_delta = p.y - quick_drawer_drag_touch_start_y;
        if (raw_delta > QUICK_DRAWER_DRAG_DEADZONE || raw_delta < -QUICK_DRAWER_DRAG_DEADZONE) {
            int32_t adjusted_delta = raw_delta > 0 ? raw_delta - QUICK_DRAWER_DRAG_DEADZONE
                                                    : raw_delta + QUICK_DRAWER_DRAG_DEADZONE;
            int32_t new_y = quick_drawer_drag_panel_start_y + adjusted_delta;
            if (new_y > 0) new_y = 0;
            if (new_y < -h) new_y = -h;
            /* Per-tick velocity, in case the finger lifts mid-flick (see the
             * release branch below) -- a plain position delta rather than
             * lv_indev_get_vect() so it's driven by the exact same samples
             * this function already reads, not a second/possibly-
             * differently-timed source. */
            quick_drawer_last_velocity = new_y - lv_obj_get_y(quick_drawer);
            lv_obj_set_y(quick_drawer, new_y);
        } else {
            quick_drawer_last_velocity = 0;
        }
    }

    if (!pressed && quick_drawer_was_pressed && quick_drawer_drag_tracking) {
        /* Finger lifted. A fast flick -- real-hardware feedback: "a quick
         * swap [that] goes back to closed"/"keeps open" -- often doesn't
         * travel far enough to cross the halfway rubber-band point before
         * the finger leaves, even though the user's intent was obvious from
         * how fast it moved. Falls back to the halfway position check only
         * for a slow/undecided drag that ends with little to no velocity. */
        quick_drawer_drag_tracking = false;
        bool snap_open;
        if (quick_drawer_last_velocity > QUICK_DRAWER_FLICK_VELOCITY) {
            snap_open = true; /* still moving down at release */
        } else if (quick_drawer_last_velocity < -QUICK_DRAWER_FLICK_VELOCITY) {
            snap_open = false; /* still moving up at release */
        } else {
            snap_open = lv_obj_get_y(quick_drawer) > -h / 2;
        }
        /* open_quick_drawer()/close_quick_drawer() animate from the
         * drawer's CURRENT (mid-drag) position, so forcing quick_drawer_open
         * to the opposite state first just defeats their own early-return
         * guard rather than fighting the animation. */
        if (snap_open) {
            quick_drawer_open = false;
            open_quick_drawer();
        } else {
            quick_drawer_open = true;
            close_quick_drawer();
        }
    }

    if (!pressed && quick_drawer_was_pressed && player_swipe_tracking) {
        /* Finger lifted mid-swipe. Same flick-vs-halfway decision as the
         * drawer's own release logic just above, just horizontal. */
        player_swipe_tracking = false;
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t current_v = lv_obj_get_x(player_swipe_ctx->img_from);
        bool commit;
        if (player_swipe_last_velocity < -PLAYER_SWIPE_FLICK_VELOCITY) {
            commit = true; /* still moving left fast at release */
        } else if (player_swipe_last_velocity > PLAYER_SWIPE_FLICK_VELOCITY) {
            commit = false; /* still moving back right fast at release */
        } else {
            commit = current_v < -w / 2; /* past halfway, slow/undecided release */
        }
        player_swipe_ctx->commit = commit;
        if (commit) {
            /* Stack bookkeeping mirroring nav_push()'s own -- the real
             * lv_screen_load() happens inside slide_transition_done_cb()
             * once this settle animation finishes, not here; nothing else
             * reads the nav stack before then, so only the bookkeeping
             * needs to be right immediately. */
            if (nav_depth < NAV_STACK_MAX && !(nav_depth > 0 && nav_stack[nav_depth - 1] == player_screen)) {
                nav_stack[nav_depth++] = player_screen;
            }
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, player_swipe_ctx);
        lv_anim_set_user_data(&a, player_swipe_ctx);
        lv_anim_set_values(&a, current_v, commit ? -w : 0);
        lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS); /* short settle, same duration class as the drawer's own release-snap */
        lv_anim_set_exec_cb(&a, slide_transition_anim_x_cb);
        lv_anim_set_completed_cb(&a, slide_transition_done_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        player_swipe_ctx = NULL;
    }

    quick_drawer_was_pressed = pressed;
}

/* Forward declarations -- defined later in this file (with the player
 * screen's own transport buttons, which they were originally written for),
 * but the quick drawer's mini now-playing card reuses them verbatim. */
static void favorite_icon_event_cb(lv_event_t * e);
static void prev_btn_event_cb(lv_event_t * e);
static void play_btn_event_cb(lv_event_t * e);
static void next_btn_event_cb(lv_event_t * e);
static const char * play_mode_icon_asset(play_mode_t mode);
static const char * basename_of(const char * path);
static lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list);
/* Defined much later, alongside the rest of the new Wi-Fi/Bluetooth
 * screens -- long-pressing the drawer's wifi/bt icons opens the real
 * settings screen for that radio, matching Android's quick-settings
 * convention (tap toggles, long-press opens the full screen). */
static void open_wifi_screen(void);
static void open_bluetooth_screen(void);

/* Long-press handlers for the drawer's wifi/bt icons -- hides the drawer
 * instantly (no slide-out animation; the settings screen navigation is
 * about to slide in over it anyway) then opens the real settings screen.
 *
 * Real-device incident: LVGL still sends LV_EVENT_CLICKED on release even
 * when LV_EVENT_LONG_PRESSED already fired earlier in that same press --
 * confirmed directly in lv_indev.c's indev_proc_release(), which doesn't
 * check whether a long-press was already sent before deciding a release
 * without enough movement counts as a click. Without this flag, a
 * long-press opened the settings screen AND, on release, the click handler
 * fired right behind it and toggled the radio -- e.g. long-pressing the
 * wifi icon opened Wi-Fi settings but also turned wifi off. Each click
 * handler below checks and clears its own flag first, skipping its toggle
 * entirely when the long-press already handled this press. */
static bool quick_drawer_wifi_long_press_fired = false;
static bool quick_drawer_bt_long_press_fired = false;

static void quick_drawer_wifi_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_wifi_long_press_fired = true;
    quick_drawer_open = false;
    lv_obj_set_y(quick_drawer, -lv_display_get_vertical_resolution(lv_display_get_default()));
    open_wifi_screen();
}

static void quick_drawer_bt_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_bt_long_press_fired = true;
    quick_drawer_open = false;
    lv_obj_set_y(quick_drawer, -lv_display_get_vertical_resolution(lv_display_get_default()));
    open_bluetooth_screen();
}

/* Real tap-to-toggle for the wifi icon -- unlike quick_drawer_bt_event_cb
 * (a purely local sprite swap, "no real backend" per its own comment), wifi
 * actually has one (wifi_control_enable()/disable()), it just was never
 * wired to anything that could turn it back OFF: the only existing caller,
 * wifi_scan_thread_func(), only ever enables it (auto-enabling when the
 * Wi-Fi settings screen is opened), so wifi previously could only ever go
 * from off to on, matching the "stays on all the time" real-device report.
 * enable()/disable() each block for about a second, so this runs on its own
 * thread, polled the same way as every other background op in this file. */
static pthread_t wifi_toggle_thread;
static bool wifi_toggle_active = false;
static volatile bool wifi_toggle_done_flag = false;

static void * wifi_toggle_thread_func(void * arg) {
    (void) arg;
    bool turning_on = !wifi_control_is_enabled();
    if (turning_on) wifi_control_enable();
    else wifi_control_disable();

    /* Real-device bug: wifi_control_is_enabled() (a plain access() check on
     * wpa_supplicant's control socket) can still read the OLD state for a
     * moment right after wifi_on.sh/wifi_off.sh return -- the script
     * finishing doesn't guarantee the socket has actually been created/
     * removed yet. Confirmed live: the optimistic UI flip (quick_drawer_
     * wifi_event_cb) briefly reverted to the old state once poll_wifi_
     * toggle()'s own check landed too early against this still-settling
     * socket, then corrected itself again shortly after -- a visible
     * on/off/on bounce with no user action in between. Retrying here
     * instead of trusting the first read means poll_wifi_toggle()'s check
     * lands on the real, settled state instead. */
    for (int i = 0; i < 10 && wifi_control_is_enabled() != turning_on; i++) {
        usleep(300000);
    }

    wifi_toggle_done_flag = true; /* written last -- poll_wifi_toggle only checks this flag */
    return NULL;
}

static void populate_wifi_screen(bool enabled); /* defined with the rest of the Wi-Fi settings screen, below */

static void quick_drawer_wifi_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_wifi_long_press_fired) { /* see quick_drawer_wifi_long_press_cb()'s own comment */
        quick_drawer_wifi_long_press_fired = false;
        return;
    }
    if (wifi_toggle_active) return; /* already toggling -- ignore taps until it lands */
    wifi_toggle_active = true;
    wifi_toggle_done_flag = false;

    /* Optimistic sprite flip -- wifi_control_is_enabled() is a plain
     * access() check (see its own comment), not a subprocess spawn, so
     * it's cheap enough to call synchronously right here. The actual
     * radio toggle below can take a couple seconds; flipping the icon
     * immediately instead of waiting for poll_wifi_toggle() to confirm it
     * is what makes the tap read as instant. poll_wifi_toggle() still
     * re-reads the real state once the thread lands and corrects this if
     * the toggle unexpectedly failed. */
    bool wifi_will_be_enabled = !wifi_control_is_enabled();
    lv_image_set_src(quick_drawer_wifi_icon, asset_path(wifi_will_be_enabled ? "pull_down/wifi_s.png" : "pull_down/wifi.png"));

    /* Optimistically rebuild the whole Wi-Fi settings screen too (not just
     * the toggle row) when that's the screen showing -- real-device
     * feedback: flipping just the row's own sprite left the Wi-Fi Info/
     * Manual SSID Entry/Memorized Networks rows (populate_wifi_screen()'s
     * own enabled-gated content) not appearing until wifi_control_enable()
     * actually finished (~1-3s), which read as the screen "taking a while"
     * even though the toggle itself looked instant. Passing the optimistic
     * wifi_will_be_enabled here (not a real wifi_control_is_enabled() call,
     * which would still read the pre-toggle state) makes the whole screen
     * -- toggle row and its gated content -- appear immediately, exactly
     * like the eventual real rebuild will look on success.
     * poll_wifi_toggle() still re-populates with the real, authoritative
     * state once wifi_control_enable()/disable() actually lands, correcting
     * this if the toggle unexpectedly failed. */
    if (nav_depth > 0 && nav_stack[nav_depth - 1] == wifi_screen) populate_wifi_screen(wifi_will_be_enabled);

    pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL);
}

static void poll_wifi_toggle(void) {
    if (!wifi_toggle_active || !wifi_toggle_done_flag) return;
    wifi_toggle_active = false;
    pthread_join(wifi_toggle_thread, NULL);
    refresh_wifi_icon(); /* re-reads the real state -- updates both the status bar and drawer icons */
    populate_wifi_screen(wifi_control_is_enabled()); /* the Wi-Fi screen's own toggle row + everything gated on it needs the same refresh */
}

/* Same real tap-to-toggle treatment for Bluetooth, mirroring the wifi
 * mechanism above -- bluetoothctl's power on/off each block for about a
 * second, so this runs on its own thread too. Turning ON additionally
 * brings up the chip first via bt_control_init_chip() if it isn't already
 * (no-op once hci0 exists) -- see that function's own comment for why this,
 * not just bluetoothctl power on, is what actually makes the toggle work at
 * all on a fresh boot. */
static pthread_t bt_toggle_thread;
static bool bt_toggle_active = false;
static volatile bool bt_toggle_done_flag = false;

static lv_obj_t * subsonic_downloading_screen; /* forward decl -- fully built below with the rest of that shared screen */
static lv_obj_t * subsonic_downloading_label;

/* Set by bt_toggle_thread_func() when disabling Bluetooth while DAC mode
 * was on -- consumed by poll_bt_toggle() to turn the setting off and close
 * the DAC overlay screen if it's the one currently showing. */
static bool bt_toggle_forced_dac_off = false;

static void * bt_toggle_thread_func(void * arg) {
    (void) arg;
    bt_toggle_forced_dac_off = false;
    bool turning_on = !bt_control_is_powered();
    bool chip_wedged = false;
    if (!turning_on) {
        /* Real-device incident: turning Bluetooth off via the quick drawer
         * while Bluetooth DAC mode was still on left bluealsa/bt-agent
         * (spawned by bt_control_apply_output_settings() when DAC mode
         * turned on) running orphaned against an adapter that was about to
         * be powered off out from under them -- confirmed to corrupt
         * bluetoothd's own adapter registration (hci0 stayed up fine at
         * the kernel level, but bluetoothd stopped seeing it entirely,
         * "No default controller available", and Bluetooth couldn't be
         * re-enabled again until bluetoothd was manually restarted). Tear
         * DAC mode's processes down first, same call
         * bt_dac_leave_confirm_cb() uses, before disabling the radio. */
        if (current_settings.bt_dac_mode_enabled) {
            bt_control_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
            bt_toggle_forced_dac_off = true;
        }
        bt_control_disable();
    } else {
        /* Real-device incident: enabling Bluetooth when bt_resume can't
         * actually bring hci0 up (a wedged BT chip -- confirmed live,
         * "Can't get device info: No such device" surviving repeated
         * bt_resume retries, needing a real power cycle to clear) used to
         * call bt_control_enable() anyway, adding a second full ~15s bounded
         * subprocess_run() wait against an adapter already known not to
         * exist -- with no busy screen at all (see quick_drawer_bt_event_cb's
         * own comment on why), the whole ~45s combined stall read to the
         * user as the device having frozen. Skipping the pointless second
         * wait here doesn't fix the underlying wedge (nothing in userspace
         * can), but at least stops doubling how long the unresponsive-
         * feeling wait lasts. */
        if (bt_control_init_chip()) bt_control_enable();
        else chip_wedged = true;
    }

    /* Real-device bug: bt_control_is_powered() (bluetoothctl show) can
     * still read the OLD state for a moment right after bt_control_enable()/
     * disable() return -- the command completing doesn't mean bluetoothd
     * has actually finished updating the adapter's reported Powered state
     * yet. Confirmed live: the optimistic UI flip (quick_drawer_bt_
     * event_cb) briefly reverted to the old state once poll_bt_toggle()'s
     * own start_refresh_bt_icon() check landed too early against this
     * still-settling state, then corrected itself again on the next
     * periodic poll a few seconds later -- a visible on/off/on bounce with
     * no user action in between. Retrying here instead of trusting the
     * first read means that check lands on the real, settled state
     * instead. Skipped entirely for the known-wedged-chip case
     * (chip_wedged) -- retrying there would just be the same pointless
     * wait the comment above already avoids, since the chip genuinely
     * isn't coming up. */
    if (!chip_wedged) {
        for (int i = 0; i < 5 && bt_control_is_powered() != turning_on; i++) {
            sleep(1);
        }
    }

    bt_toggle_done_flag = true; /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

/* Real-device incident: a tap that landed while bt_init's own chip flash
 * was still genuinely in progress used to just be refused outright (see
 * BT_INIT_OK_FLAG_PATH's own comment for the actual chip-wedging risk that
 * guards against) -- functionally safe, but made the user re-tap
 * themselves once it settled, AND gave no visual feedback at all that
 * anything had registered (the drawer's own icon never flipped, unlike a
 * normal toggle). This queues the intent properly instead: waits for
 * BT_INIT_OK_FLAG_PATH (cheap access() polling, no subprocess spawn), then
 * performs the exact same turn-on sequence bt_toggle_thread_func()'s own
 * turning-on path uses, automatically, no second tap needed. Deliberately
 * NOT the same thing as the unconditional-auto-enable-at-boot approach
 * tried and reverted earlier -- this only ever fires because the user
 * explicitly asked to turn Bluetooth on, just before it was safe to.
 *
 * Reuses bt_toggle_active/bt_toggle_thread/bt_toggle_done_flag/
 * poll_bt_toggle() -- the exact same in-flight-toggle bookkeeping
 * bt_toggle_thread_func() already uses -- rather than a separate parallel
 * flag, specifically so quick_drawer_bt_event_cb()'s own optimistic icon
 * flip (and populate_bt_screen() guard) apply here too automatically,
 * fixing the "no visual clue" gap. poll_bt_toggle()'s own
 * start_refresh_bt_icon() call at the end is exactly what's wanted here
 * too: a fresh real-state poll once this either succeeds or times out.
 * Capped at BT_BOOT_ENABLE_MAX_WAIT_MS so a genuinely wedged/never-
 * finishing bt_init doesn't leave this polling forever. */
#define BT_BOOT_ENABLE_MAX_WAIT_MS 30000
#define BT_BOOT_ENABLE_POLL_INTERVAL_MS 300

static void * bt_pending_enable_thread_func(void * arg) {
    (void) arg;
    uint32_t waited_ms = 0;
    while (waited_ms < BT_BOOT_ENABLE_MAX_WAIT_MS) {
        if (access(BT_INIT_OK_FLAG_PATH, F_OK) == 0) {
            if (bt_control_init_chip()) bt_control_enable();
            break;
        }
        usleep(BT_BOOT_ENABLE_POLL_INTERVAL_MS * 1000);
        waited_ms += BT_BOOT_ENABLE_POLL_INTERVAL_MS;
    }
    bt_toggle_done_flag = true; /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

static void quick_drawer_bt_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_bt_long_press_fired) { /* see quick_drawer_bt_long_press_cb()'s own comment */
        quick_drawer_bt_long_press_fired = false;
        return;
    }
    if (bt_toggle_active) return; /* already toggling -- ignore taps until it lands */

    bool bt_will_be_powered = !bt_is_powered_cached;

    /* Real-device incident: turning Bluetooth ON before BT_INIT_OK_FLAG_PATH
     * exists raced this app's own bt_control_init_chip() against bt_init's
     * own still-in-progress UART chip firmware flash and genuinely wedged
     * the chip (unrecoverable without a full power cycle) -- see its own
     * comment. Turning OFF is always safe (bt_control_disable() is
     * D-Bus-only, no chip-level operation), so this only ever affects the
     * turning-on direction: bt_pending_now queues bt_toggle_thread_func()'s
     * usual work behind bt_pending_enable_thread_func() instead of running
     * it directly, but everything below (icon flip, cached value,
     * settings-screen refresh, bt_toggle_active bookkeeping) is identical
     * either way -- exactly the fix for the earlier version of this, which
     * showed no visual feedback at all for a tap that landed too early. */
    bool bt_pending_now = bt_will_be_powered && access(BT_INIT_OK_FLAG_PATH, F_OK) != 0;

    /* Ends the early-boot display suppression immediately on a real user
     * tap -- see bt_boot_suppress_active()'s own comment for why this
     * matters (a manual tap shouldn't be fighting a window that doesn't
     * know the user has acted). No-op once suppression has already ended
     * on its own. */
    bt_boot_suppress_start_tick = 0;

    bt_toggle_active = true;
    bt_toggle_done_flag = false;

    /* Optimistic sprite flip, same reasoning as quick_drawer_wifi_event_cb's
     * own comment -- bt_is_powered_cached (kept fresh by
     * poll_refresh_bt_icon()) is a plain bool read, not the subprocess spawn
     * bt_control_is_powered() itself is, so it's safe to read synchronously
     * here. Turning on cold can take ~10-13s (bt_control_init_chip()), or
     * however long is left of bt_init's own run if bt_pending_now; this is
     * what makes the tap itself read as instant instead of the icon
     * sitting frozen until poll_bt_toggle() confirms the real state once
     * the thread lands. */
    lv_image_set_src(quick_drawer_bt_icon, asset_path(bt_will_be_powered ? "pull_down/bt_s.png" : "pull_down/bt.png"));

    /* Optimistically rebuild the whole Bluetooth settings screen too (not
     * just the toggle row) when that's the screen showing -- same
     * "screen takes a while to appear" real-device feedback as
     * quick_drawer_wifi_event_cb's own comment, actually worse here: the
     * real path is bt_toggle_thread_func() (up to ~10-13s cold) followed by
     * a SEPARATE start_refresh_bt_icon() subprocess round-trip
     * (poll_bt_toggle() doesn't call populate_bt_screen() itself -- see its
     * own comment) before bt_is_powered_cached actually catches up and the
     * screen naturally rebuilds. Setting bt_is_powered_cached directly here
     * is safe: poll_refresh_bt_icon() (the only other writer) skips its own
     * populate_bt_screen() call entirely while bt_toggle_active is true
     * (see its own comment on this exact race), so nothing overwrites this
     * until start_refresh_bt_icon()'s real result lands afterward and
     * correctly finalizes it. */
    bt_is_powered_cached = bt_will_be_powered;
    if (nav_depth > 0 && nav_stack[nav_depth - 1] == bt_screen) populate_bt_screen();

    /* Runs fully in the background, same as the stock player -- no busy
     * screen. An earlier version pushed a "Turning on Bluetooth..."
     * interstitial here, shared with wifi/bt scan's own overlay -- see git
     * history if that's ever needed again, but it was also the source of a
     * real, repeatedly-hit stuck-screen bug (multiple uncoordinated users of
     * one shared overlay), which not having an overlay at all sidesteps
     * entirely. */
    pthread_create(&bt_toggle_thread, NULL, bt_pending_now ? bt_pending_enable_thread_func : bt_toggle_thread_func, NULL);
}

static void poll_bt_toggle(void) {
    if (!bt_toggle_active || !bt_toggle_done_flag) return;
    bt_toggle_active = false;
    pthread_join(bt_toggle_thread, NULL);

    if (bt_toggle_forced_dac_off) {
        current_settings.bt_dac_mode_enabled = false;
        settings_save(&current_settings);
        /* Bluetooth just got disabled out from under DAC mode -- if its
         * overlay is the screen currently showing, staying on it is
         * meaningless (there's no Bluetooth left to receive audio over),
         * so close it automatically instead of leaving a "Bluetooth DAC
         * mode" screen up with nothing backing it. */
        if (lv_screen_active() == bt_dac_overlay_screen) nav_pop();
    }

    /* start_refresh_bt_icon() only starts the background check -- it
     * hasn't updated bt_is_powered_cached yet by the time this returns, so
     * populate_bt_screen() (which now reads that cache, not a fresh
     * bt_control_is_powered() call -- see its own comment) can't be called
     * here too or the Bluetooth screen's toggle row would show stale
     * (pre-toggle) state until something else happens to repopulate it.
     * poll_refresh_bt_icon() calls populate_bt_screen() itself once the
     * cache is actually fresh. */
    start_refresh_bt_icon(); /* re-reads the real state -- updates the status bar/drawer icons and (once done) the Bluetooth screen's toggle row */
}

/* Real-device incident: bt_dac_enable_row_cb() is the ONLY call site for
 * bt_control_apply_output_settings() -- current_settings.bt_dac_mode_enabled
 * is persisted to disk, so if it was left on at the end of a previous
 * session, this app's UI comes back up on next launch already showing the
 * toggle as on (and correctly blocking local playback via
 * external_dac_block_reason()), but the actual bluealsa/bt-agent processes
 * that make a Bluetooth DAC connection possible were never started this
 * boot -- confirmed on a real device: the toggle read "on", yet no device
 * could connect at all (bluealsa/bt-agent simply weren't running). This
 * reapplies the persisted state once at startup, mirroring
 * bt_toggle_thread_func() (chip init can take ~10-13s on a fresh boot, so
 * this can't block gui_init() / the UI thread). */
static pthread_t bt_dac_startup_reapply_thread;
static bool bt_dac_startup_reapply_active = false;
static volatile bool bt_dac_startup_reapply_done_flag = false;

static void * bt_dac_startup_reapply_thread_func(void * arg) {
    (void) arg;
    bt_control_init_chip();
    bt_control_enable();
    bt_control_apply_output_settings(true, current_settings.bt_volume_sync_enabled);
    bt_dac_startup_reapply_done_flag = true; /* written last -- poll_bt_dac_startup_reapply only checks this flag */
    return NULL;
}

/* Called once from gui_init(), only if bt_dac_mode_enabled was already true
 * at load time (a fresh toggle-on tap already goes through
 * bt_dac_toggle_cb() directly and doesn't need this). */
static void start_bt_dac_startup_reapply_if_needed(void) {
    if (!current_settings.bt_dac_mode_enabled) return;
    bt_dac_startup_reapply_active = true;
    bt_dac_startup_reapply_done_flag = false;
    pthread_create(&bt_dac_startup_reapply_thread, NULL, bt_dac_startup_reapply_thread_func, NULL);
}


static void poll_bt_dac_startup_reapply(void) {
    if (!bt_dac_startup_reapply_active || !bt_dac_startup_reapply_done_flag) return;
    bt_dac_startup_reapply_active = false;
    pthread_join(bt_dac_startup_reapply_thread, NULL);
    start_refresh_bt_icon();
}

/* Real-device incident: bt_dac_toggle_cb(), bt_volume_sync_toggle_cb(), and
 * airplay_toggle_cb() (when turning AirPlay on forces Bluetooth DAC off)
 * all called bt_control_apply_output_settings() directly from their LVGL
 * click handlers, i.e. on the UI thread -- despite that function's own doc
 * comment already saying "blocking, call off the UI thread". Confirmed on a
 * real device: it kills and respawns bluealsa/bt-agent/bluealsa-aplay with
 * two separate 500ms sleeps plus several subprocess spawns baked in, easily
 * 1-3+ seconds of pure blocking -- since lv_timer_handler() runs on this
 * same thread, that froze the entire UI (no redraws, no touch input at all)
 * for the whole duration, with whatever screen happened to be showing at
 * that instant stuck on screen looking unresponsive, no way to back out of
 * it, until the call finally returned. This backgrounds it the same way
 * every other slow Bluetooth/Wi-Fi operation in this file already is. */
static pthread_t bt_apply_output_settings_thread;
static bool bt_apply_output_settings_active = false;
static volatile bool bt_apply_output_settings_done_flag = false;

typedef struct {
    bool dac_mode_enabled;
    bool volume_sync_enabled;
} bt_apply_output_settings_request_t;

static void * bt_apply_output_settings_thread_func(void * arg) {
    bt_apply_output_settings_request_t * req = (bt_apply_output_settings_request_t *) arg;
    bt_control_apply_output_settings(req->dac_mode_enabled, req->volume_sync_enabled);
    free(req);
    bt_apply_output_settings_done_flag = true; /* written last -- poll_bt_apply_output_settings only checks this flag */
    return NULL;
}

/* Silently ignores overlap (another apply already in flight) rather than
 * queuing -- same "ignore taps until it lands" treatment as
 * quick_drawer_bt_event_cb()/quick_drawer_wifi_event_cb() use for their own
 * slow operations, and the current_settings values the caller already wrote
 * before calling this are what the eventually-scheduled apply would use
 * anyway once the in-flight one finishes and the screen is re-populated. */
static void start_bt_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    if (bt_apply_output_settings_active) return;
    bt_apply_output_settings_request_t * req = malloc(sizeof(*req));
    req->dac_mode_enabled = dac_mode_enabled;
    req->volume_sync_enabled = volume_sync_enabled;
    bt_apply_output_settings_done_flag = false;
    bt_apply_output_settings_active = true;
    pthread_create(&bt_apply_output_settings_thread, NULL, bt_apply_output_settings_thread_func, req);
}

static void populate_bt_dac_screen(void); /* defined with the rest of the Bluetooth DAC screen, below */

static void poll_bt_apply_output_settings(void) {
    if (!bt_apply_output_settings_active || !bt_apply_output_settings_done_flag) return;
    bt_apply_output_settings_active = false;
    pthread_join(bt_apply_output_settings_thread, NULL);
    populate_bt_dac_screen(); /* the DAC screen's own toggle rows need the post-apply state */
}

/* pull_down/bg.png bakes in two fixed rounded panels (measured directly off
 * the asset: a vertical scan for opaque-color transitions at x=240 finds
 * flat color from y=59-338, a gap, then y=363-730; a horizontal scan finds
 * the fill spanning x=19-459 either way) -- everything below is positioned
 * against those measured bounds, not guessed, since anything placed outside
 * them draws on the plain black gap/margin around the panels instead of
 * inside the rounded box that's supposed to contain it (this is what was
 * actually wrong before: row 1's icons started at STATUS_BAR_CLEARANCE-8=40,
 * 19px above the real panel top of 59, and the card started at
 * STATUS_BAR_CLEARANCE+250=298, 65px above the real second panel's top of
 * 363). */
#define QUICK_DRAWER_PANEL1_TOP 59
#define QUICK_DRAWER_PANEL1_BOTTOM 338
#define QUICK_DRAWER_PANEL2_TOP 363

static void quick_drawer_brightness_changed_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = (lv_obj_t *) lv_event_get_target(e);
    int32_t percent = lv_slider_get_value(slider);
    lv_obj_t * label = (lv_obj_t *) lv_event_get_user_data(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        backlight_set_percent((int) percent); /* live feedback while dragging */
        lv_label_set_text_fmt(label, "%d%%", (int) percent);
    } else if (code == LV_EVENT_RELEASED) {
        /* Only persist once the drag settles, not on every intermediate
         * tick -- same as volume_popup_track_event_cb/volume_slider_event_cb. */
        current_settings.brightness_percent = (int) percent;
        settings_save(&current_settings);
    }
}

static void build_quick_drawer(void) {
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    quick_drawer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(quick_drawer);
    lv_obj_set_size(quick_drawer, w, h);
    lv_obj_set_pos(quick_drawer, 0, -h); /* fully off-screen above until opened */
    lv_obj_set_style_bg_image_src(quick_drawer, asset_path("pull_down/bg.png"), 0);
    lv_obj_set_style_bg_opa(quick_drawer, LV_OPA_COVER, 0);
    lv_obj_remove_flag(quick_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_CLICKABLE); /* swallow touches to whatever's behind while open */

    /* Row 1: every toggle icon (Bluetooth / Wifi / sleep timer / output
     * gain) together in one row -- 4 icons x 84px + 5 gaps of 21px exactly
     * fills the panel's measured 440px content width (19 to 459). No clock,
     * no volume slider here anymore (clock duplicated the always-visible
     * main status bar; a second volume control duplicated the hardware
     * volume buttons' own popup) -- brightness (row 2 below) is the only
     * slider left in this drawer. */
    quick_drawer_wifi_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_wifi_icon, asset_path("pull_down/wifi.png"));
    lv_obj_align(quick_drawer_wifi_icon, LV_ALIGN_TOP_LEFT, 40, QUICK_DRAWER_PANEL1_TOP + 30);
    lv_obj_add_flag(quick_drawer_wifi_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_wifi_icon, quick_drawer_wifi_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(quick_drawer_wifi_icon, quick_drawer_wifi_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    quick_drawer_bt_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_bt_icon, asset_path("pull_down/bt.png"));
    lv_obj_align(quick_drawer_bt_icon, LV_ALIGN_TOP_LEFT, 145, QUICK_DRAWER_PANEL1_TOP + 30);
    lv_obj_add_flag(quick_drawer_bt_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_bt_icon, quick_drawer_bt_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(quick_drawer_bt_icon, quick_drawer_bt_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    quick_drawer_sleep_icon = lv_image_create(quick_drawer);
    lv_image_set_src(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
    lv_obj_align(quick_drawer_sleep_icon, LV_ALIGN_TOP_LEFT, 250, QUICK_DRAWER_PANEL1_TOP + 30);
    lv_obj_add_flag(quick_drawer_sleep_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_sleep_icon, quick_drawer_sleep_event_cb, LV_EVENT_CLICKED, NULL);

    /* Countdown while armed -- see quick_drawer_sleep_event_cb()/
     * poll_sleep_timer()'s own comments. Hidden until armed, centered under
     * the 84px-wide icon above it. */
    quick_drawer_sleep_label = lv_label_create(quick_drawer);
    lv_obj_add_style(quick_drawer_sleep_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_sleep_label, ui_size_16, 0);
    lv_obj_set_width(quick_drawer_sleep_label, 84);
    lv_obj_set_style_text_align(quick_drawer_sleep_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(quick_drawer_sleep_label, LV_ALIGN_TOP_LEFT, 250, QUICK_DRAWER_PANEL1_TOP + 30 + 84 + 4);
    lv_obj_add_flag(quick_drawer_sleep_label, LV_OBJ_FLAG_HIDDEN);

    quick_drawer_crossfade_icon = lv_image_create(quick_drawer);
    lv_obj_align(quick_drawer_crossfade_icon, LV_ALIGN_TOP_LEFT, 355, QUICK_DRAWER_PANEL1_TOP + 30);
    lv_obj_add_flag(quick_drawer_crossfade_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_crossfade_icon, quick_drawer_crossfade_event_cb, LV_EVENT_CLICKED, NULL);
    refresh_quick_drawer_crossfade_icon();

    /* Row 2: screen brightness -- real control, via the standard Linux
     * backlight sysfs class (backlight.h), no dedicated slider-track asset
     * in this theme so it reuses the (generic-looking) volume slider's own. */
    lv_obj_t * brightness_icon = lv_image_create(quick_drawer);
    lv_image_set_src(brightness_icon, asset_path("pull_down/blk.png"));
    lv_obj_align(brightness_icon, LV_ALIGN_TOP_LEFT, 40, QUICK_DRAWER_PANEL1_TOP + 174);

    quick_drawer_brightness_label = lv_label_create(quick_drawer);
    lv_obj_add_style(quick_drawer_brightness_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_brightness_label, ui_size_20, 0);
    lv_obj_align(quick_drawer_brightness_label, LV_ALIGN_TOP_RIGHT, -20, QUICK_DRAWER_PANEL1_TOP + 177);

    quick_drawer_brightness_track = lv_slider_create(quick_drawer);
    lv_obj_set_size(quick_drawer_brightness_track, 300, 12);
    lv_obj_align(quick_drawer_brightness_track, LV_ALIGN_TOP_LEFT, 90, QUICK_DRAWER_PANEL1_TOP + 185);
    /* Full 0-100 -- backlight.c now maps this logical range to its own safe
     * raw range internally (see backlight.h's own comment), so the slider
     * itself is free to show a clean, honest 0%-100% again. */
    lv_slider_set_range(quick_drawer_brightness_track, 0, 100);
    /* Initial value set below by refresh_quick_drawer_brightness() (also
     * called on every open_quick_drawer(), see its own comment) --
     * defined here just so it runs once at build time too, same as
     * every other quick-drawer widget's own initial state. */
    lv_obj_set_style_bg_color(quick_drawer_brightness_track, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(quick_drawer_brightness_track, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(quick_drawer_brightness_track, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_bg_image_src(quick_drawer_brightness_track, asset_path("volume/vol_bg.png"), LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(quick_drawer_brightness_track, asset_path("volume/vol_progress.png"), LV_PART_INDICATOR);
    lv_obj_set_style_bg_image_src(quick_drawer_brightness_track, asset_path("volume/cursor.png"), LV_PART_KNOB);
    /* Real-device bug report: accent color didn't apply here -- see
     * apply_accent_color()'s own comment on why an image-art slider needs
     * bg_image_recolor, not just bg_color. */
    lv_obj_add_style(quick_drawer_brightness_track, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(quick_drawer_brightness_track, &style_accent, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(quick_drawer_brightness_track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(quick_drawer_brightness_track, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(quick_drawer_brightness_track, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(quick_drawer_brightness_track, 26, LV_PART_KNOB);
    lv_obj_set_style_height(quick_drawer_brightness_track, 26, LV_PART_KNOB);
    lv_obj_add_event_cb(quick_drawer_brightness_track, quick_drawer_brightness_changed_cb, LV_EVENT_ALL,
                         quick_drawer_brightness_label);
    refresh_quick_drawer_brightness();

    /* Mini now-playing card: real track title/artist/transport, reusing the
     * exact same callbacks as the player screen's own buttons. Transparent
     * -- it sits directly on the second bg.png panel (which already is a
     * rounded dark box) rather than drawing a second, slightly-differently
     * colored rounded rect on top of that one.
     *
     * Real-device feedback comparing against the stock drawer: this used
     * to be only 200px tall, well short of the second panel's own measured
     * 367px height (y=363-730, see build_quick_drawer()'s own panel-bounds
     * comment) -- leaving most of the panel empty and the transport row
     * sitting high up rather than low in the card like the stock
     * reference. 330 leaves a comparable margin above the panel's bottom
     * edge instead. */
    lv_obj_t * card = lv_obj_create(quick_drawer);
    lv_obj_set_size(card, 440, 330);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, QUICK_DRAWER_PANEL2_TOP + 12);
    lv_obj_set_style_bg_opa(card, 0, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title/artist centered (matching the stock drawer's own mini card,
     * confirmed via a real-device screenshot comparison -- this was
     * previously left-aligned with the favorite icon pinned separately in
     * the top-right corner, not part of the transport row at all). Both
     * need an explicit width for LV_TEXT_ALIGN_CENTER to have something to
     * center within. */
    quick_drawer_title_label = lv_label_create(card);
    lv_label_set_text(quick_drawer_title_label, "No track loaded");
    lv_obj_add_style(quick_drawer_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(quick_drawer_title_label, &app_font_16, 0); /* see song_title_label's own comment */
    lv_obj_set_width(quick_drawer_title_label, lv_pct(100));
    lv_obj_set_style_text_align(quick_drawer_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(quick_drawer_title_label, LV_ALIGN_TOP_MID, 0, 14);

    quick_drawer_artist_label = lv_label_create(card);
    lv_label_set_text(quick_drawer_artist_label, "");
    lv_obj_add_style(quick_drawer_artist_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(quick_drawer_artist_label, &app_font_16, 0);
    lv_obj_set_width(quick_drawer_artist_label, lv_pct(100));
    lv_obj_set_style_text_align(quick_drawer_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(quick_drawer_artist_label, LV_ALIGN_TOP_MID, 0, 44);

    /* Transport row: order/prev/play/next/favorite, all five in one row --
     * matching the stock drawer exactly (shuffle-style icon leftmost,
     * favorite heart rightmost, same as the reference screenshot). This
     * copy of the order icon is a visual-only mirror of the main player
     * screen's own (see order_icon_event_cb) -- not independently
     * clickable, just kept in sync so the drawer doesn't show a stale mode. */
    lv_obj_t * controls_row = lv_obj_create(card);
    /* 84, not 70 -- btn_play.png/btn_pause.png are 84x84 (confirmed via the
     * actual asset files), and a shorter row was clipping the top/bottom of
     * that icon, confirmed on a real device. */
    lv_obj_set_size(controls_row, lv_pct(100), 84);
    lv_obj_align(controls_row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(controls_row, 0, 0);
    lv_obj_set_style_border_width(controls_row, 0, 0);
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(controls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    quick_drawer_order_icon = lv_image_create(controls_row);
    lv_image_set_src(quick_drawer_order_icon, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));

    lv_obj_t * prev_btn = lv_image_create(controls_row);
    lv_image_set_src(prev_btn, asset_path("playing_plane/btn_prev.png"));
    lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);

    quick_drawer_play_btn = lv_image_create(controls_row);
    lv_image_set_src(quick_drawer_play_btn, asset_path("playing_plane/btn_play.png"));
    lv_obj_add_flag(quick_drawer_play_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * next_btn = lv_image_create(controls_row);
    lv_image_set_src(next_btn, asset_path("playing_plane/btn_next.png"));
    lv_obj_add_flag(next_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);

    quick_drawer_favorite_icon = lv_image_create(controls_row);
    lv_image_set_src(quick_drawer_favorite_icon, asset_path("playing_plane/collect_out.png"));
    lv_obj_add_flag(quick_drawer_favorite_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_favorite_icon, favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);
}

static void refresh_clock_label(void) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tm_info);

    /* buf is always "HH:MM" (5 chars) from strftime's %H/%M, so this maps
     * 1:1 onto the 5 fixed slots -- no leading-slot-hiding needed here,
     * unlike the volume/battery readouts. */
    for (int i = 0; i < 5; i++) {
        char asset[24];
        if (buf[i] == ':') {
            snprintf(asset, sizeof(asset), "topbar/colon.png");
        } else {
            snprintf(asset, sizeof(asset), "topbar/%c.png", buf[i]);
        }
        lv_image_set_src(clock_topbar_digit[i], asset_path(asset));
    }
}

static void free_playlist(void) {
    for (int i = 0; i < playlist_count; i++) free(playlist[i]);
    free(playlist);
    playlist = NULL;
    playlist_count = 0;
    playlist_index = -1;
}

static const char * play_mode_icon_asset(play_mode_t mode) {
    switch (mode) {
        case PLAY_MODE_REPEAT_ALL: return "playing_plane/loop.png";
        case PLAY_MODE_REPEAT_ONE: return "playing_plane/single.png";
        case PLAY_MODE_SHUFFLE:    return "playing_plane/random.png";
        default:                   return "playing_plane/order.png";
    }
}

/* Fisher-Yates shuffle of a fresh 0..count-1 permutation. */
static void fisher_yates_shuffle(int * arr, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* (Re)generates shuffle_order if it's stale (playlist size changed, or it
 * was never generated) and points shuffle_pos at wherever the currently
 * playing track landed in the new order -- switching into shuffle mode, or
 * the playlist changing size mid-shuffle, should never itself jump away
 * from whatever's already playing. */
static void ensure_shuffle_order_current(void) {
    if (shuffle_order && shuffle_order_count == playlist_count) return;

    static bool rng_seeded = false;
    if (!rng_seeded) {
        srand((unsigned int) time(NULL));
        rng_seeded = true;
    }

    free(shuffle_order);
    shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
    for (int i = 0; i < playlist_count; i++) shuffle_order[i] = i;
    fisher_yates_shuffle(shuffle_order, playlist_count);
    shuffle_order_count = playlist_count;

    shuffle_pos = 0;
    if (playlist_index >= 0) {
        for (int i = 0; i < playlist_count; i++) {
            if (shuffle_order[i] == playlist_index) {
                shuffle_pos = i;
                break;
            }
        }
    }
}

/* Set only when compute_auto_advance_index() has to precompute a reshuffled
 * continuation bag for the shuffle-wrap case -- see both functions' own
 * comments. */
static int * pending_shuffle_order = NULL;

/* What track to move to when the current one finishes naturally (auto-
 * advance) -- as opposed to compute_manual_step_index() below, for an
 * explicit Prev/Next tap. Returns -1 for "stop, end of queue" (only
 * possible in Sequential mode).
 *
 * Deliberately read-only (never mutates shuffle_pos/shuffle_order) --
 * arm_next_track_for_audio() calls this speculatively, well before the
 * transition it's asking about is actually confirmed (gapless preload/
 * crossfade prep), so it must be safe to call more than once for the same
 * `index` and always get the same answer. commit_auto_advance() below is
 * the only thing allowed to actually advance the shuffle state, called
 * exactly once at the point a transition is confirmed real. */
static int compute_auto_advance_index(int index) {
    if (index < 0 || playlist_count <= 0) return -1;

    /* A queued song always plays next, ahead of play_mode entirely (Repeat
     * One included -- queueing something is an explicit override of
     * whatever's currently looping) -- see queued_pending_count's own
     * comment. */
    if (queued_pending_count > 0 && index + 1 < playlist_count) return index + 1;

    switch ((play_mode_t) current_settings.play_mode) {
        case PLAY_MODE_REPEAT_ONE:
            return index;
        case PLAY_MODE_REPEAT_ALL:
            return (index + 1) % playlist_count;
        case PLAY_MODE_SHUFFLE: {
            ensure_shuffle_order_current();
            int peek_pos = shuffle_pos + 1;
            if (peek_pos < playlist_count) return shuffle_order[peek_pos];

            /* Bag exhausted -- precompute the reshuffled continuation now
             * rather than waiting for commit_auto_advance(), so whatever
             * gets armed for gapless preload here and whatever that commit
             * later confirms are guaranteed to be the same track. */
            free(pending_shuffle_order);
            pending_shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
            memcpy(pending_shuffle_order, shuffle_order, sizeof(int) * (size_t) playlist_count);
            fisher_yates_shuffle(pending_shuffle_order, playlist_count);
            return pending_shuffle_order[0];
        }
        case PLAY_MODE_SEQUENTIAL:
        default:
            return (index + 1 < playlist_count) ? index + 1 : -1;
    }
}

/* Call exactly once, right when an auto-advance computed by
 * compute_auto_advance_index() actually happens (the playback thread really
 * moved on, or the hard-restart fallback is about to play that index after
 * a true end-of-playlist/failed-file) -- advances shuffle_pos, swapping in
 * the reshuffled bag precomputed above if a wrap happened. No-op outside
 * Shuffle mode, where compute_auto_advance_index() is already pure/stateless. */
static void commit_auto_advance(void) {
    /* Matches compute_auto_advance_index()'s own queue-priority check --
     * a queue-jump never steps through the shuffle bag (shuffle_pos is
     * untouched), so it's handled here first and returns before any of the
     * shuffle bookkeeping below. */
    if (queued_pending_count > 0) {
        queued_pending_count--;
        if (queued_pending_count == 0) queue_next_insert_index = -1;
        return;
    }

    if ((play_mode_t) current_settings.play_mode != PLAY_MODE_SHUFFLE) return;

    int peek_pos = shuffle_pos + 1;
    if (peek_pos < playlist_count) {
        shuffle_pos = peek_pos;
        return;
    }
    if (pending_shuffle_order) {
        free(shuffle_order);
        shuffle_order = pending_shuffle_order;
        pending_shuffle_order = NULL;
        shuffle_order_count = playlist_count;
        shuffle_pos = 0;
    }
}

/* What track an explicit Prev/Next button tap should move to. direction is
 * +1 (Next) or -1 (Prev). Repeat One doesn't affect manual skipping (only
 * auto-advance-at-end) -- a deliberate Next tap should never mean "restart
 * this same track". Returns -1 for "no-op, already at that edge". */
static int compute_manual_step_index(int index, int direction) {
    if (index < 0 || playlist_count <= 0) return -1;

    /* Same queue-priority override as compute_auto_advance_index()/
     * commit_auto_advance() -- a manual Next tap should land on a queued
     * song too, not skip past it into whatever play_mode would otherwise
     * pick. Every caller of this function (direction=+1 case) is a real,
     * one-shot commit (touchscreen/hw button/BT remote/phone remote Next,
     * each behind its own edge-triggered "consume" flag) -- never a
     * speculative preview call -- so decrementing here is safe. */
    if (direction > 0 && queued_pending_count > 0 && index + 1 < playlist_count) {
        queued_pending_count--;
        if (queued_pending_count == 0) queue_next_insert_index = -1;
        return index + 1;
    }

    if ((play_mode_t) current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        int new_pos = shuffle_pos + direction;
        if (new_pos < 0) return -1; /* no history to go back past */
        if (new_pos >= playlist_count) {
            fisher_yates_shuffle(shuffle_order, playlist_count);
            new_pos = 0;
        }
        shuffle_pos = new_pos;
        return shuffle_order[shuffle_pos];
    }

    if ((play_mode_t) current_settings.play_mode == PLAY_MODE_REPEAT_ALL) {
        return (index + direction + playlist_count) % playlist_count;
    }

    int next = index + direction;
    return (next >= 0 && next < playlist_count) ? next : -1;
}

/* Splits a full path into a display title (filename, no extension) and the
 * name of its containing folder. No tag/metadata parsing yet, so the
 * filename is the best "song title" available. */
static void get_display_names(const char * path, char * title_out, size_t title_size,
                               char * folder_out, size_t folder_size) {
    const char * slash = strrchr(path, '/');
    const char * filename = slash ? slash + 1 : path;

    const char * dot = strrchr(filename, '.');
    size_t len = dot ? (size_t) (dot - filename) : strlen(filename);
    if (len >= title_size) len = title_size - 1;
    memcpy(title_out, filename, len);
    title_out[len] = '\0';

    folder_out[0] = '\0';
    if (slash) {
        char dir_path[PATH_MAX];
        size_t dir_len = (size_t) (slash - path);
        if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';

        const char * folder_slash = strrchr(dir_path, '/');
        const char * folder_name = folder_slash ? folder_slash + 1 : dir_path;
        snprintf(folder_out, folder_size, "%s", folder_name);
    }
}

static void format_time(double seconds, char * buf, size_t buf_size) {
    if (seconds < 0) seconds = 0;
    int total = (int) seconds;
    snprintf(buf, buf_size, "%d:%02d", total / 60, total % 60);
}

static void set_play_button_state(bool is_playing) {
    lv_image_set_src(play_btn, asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png"));
    if (quick_drawer_play_btn) {
        lv_image_set_src(quick_drawer_play_btn,
                         asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png"));
    }
}

/* File extension (already known synchronously from the filename) plus the
 * live sample rate (only known once the decoder's actually opened, on the
 * playback thread -- may briefly read 0 right at track-start, self-corrects
 * on the next timer tick same as position/duration do). */
static void refresh_format_badge(void) {
    if (playlist_index < 0) {
        lv_label_set_text(format_badge_label, "");
        return;
    }

    const char * path = playlist[playlist_index];
    const char * dot = strrchr(path, '.');
    char ext[16] = "";
    if (dot) {
        size_t i = 0;
        for (const char * p = dot + 1; *p && i < sizeof(ext) - 1; p++, i++) {
            ext[i] = (char) toupper((unsigned char) *p);
        }
        ext[i] = '\0';
    }

    unsigned int sample_rate = audio_get_sample_rate();
    if (sample_rate > 0) {
        lv_label_set_text_fmt(format_badge_label, "%s  %.1fkHz", ext, sample_rate / 1000.0);
    } else {
        lv_label_set_text(format_badge_label, ext);
    }
}

#define COVER_ART_WIDTH 480
#define COVER_ART_HEIGHT 480

/* Feature request: reuse the stock firmware's own "frosted mirror" look --
 * a heavily blurred, darkened, vertically-mirrored copy of the album art
 * filling the panel behind the transport controls (player_overlay_panel,
 * built in build_player_screen()), rather than that panel's plain flat
 * buttom.png background. Confirmed via investigation that the stock
 * firmware itself doesn't ship this as a static asset (every buttom.png
 * across both themes is a flat opaque solid color, no gradient or baked-in
 * art) and no now-playing layout JSON was available to inspect directly --
 * so this is generated fresh here, from the same per-track RGB565 buffer
 * cover_decode_to_rgb565() already decodes, rather than reverse-engineered
 * from an asset that doesn't exist in this codebase's copy of the firmware. */
#define REFLECTION_WIDTH 480
#define REFLECTION_HEIGHT 320
#define REFLECTION_BLUR_RADIUS 16
/* Kept as an integer ratio (channel * NUM / DEN) rather than a float --
 * matches this codebase's general preference for integer arithmetic on the
 * embedded target, and there's no accuracy need here that would justify a
 * float. 1/4 reads as "mostly faded into black" without the panel going
 * fully flat. */
#define REFLECTION_DARKEN_NUM 1
#define REFLECTION_DARKEN_DEN 4

/* 1D box blur via a sliding running sum -- O(length) regardless of radius,
 * rather than O(length * radius) from re-summing the whole window at every
 * pixel. `stride` is measured in elements, not bytes, so the same function
 * serves both passes of the separable 2D blur generate_reflection() below
 * does: 1 for a horizontal pass along a contiguous row, `width` for a
 * vertical pass down a column. Edges are clamped (repeats the edge pixel)
 * rather than wrapping or zero-padding, so the blur doesn't darken/lighten
 * near the image boundary. */
static void box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius) {
    int window = radius * 2 + 1;
    int sum = 0;
    for (int i = -radius; i <= radius; i++) {
        int idx = i < 0 ? 0 : (i >= length ? length - 1 : i);
        sum += src[idx * stride];
    }
    for (int i = 0; i < length; i++) {
        dst[i * stride] = (uint8_t) (sum / window);
        int add_idx = i + radius + 1;
        if (add_idx >= length) add_idx = length - 1;
        int rem_idx = i - radius;
        if (rem_idx < 0) rem_idx = 0;
        sum += src[add_idx * stride] - src[rem_idx * stride];
    }
}

/* Pure pixel math -- no LVGL/shared-state touch, so it's safe to call from
 * a background thread (see the async cover-decode pipeline below). Builds
 * the reflection from a given COVER_ART_WIDTH x COVER_ART_HEIGHT RGB565
 * buffer and returns a freshly malloc'd REFLECTION_WIDTH x REFLECTION_HEIGHT
 * RGB565 buffer, or NULL on allocation failure. Caller owns the result. */
static uint8_t * compute_reflection_bytes(const uint8_t * cover_bytes) {
    int w = REFLECTION_WIDTH, h = REFLECTION_HEIGHT;
    uint8_t * r = malloc((size_t) w * h);
    uint8_t * g = malloc((size_t) w * h);
    uint8_t * b = malloc((size_t) w * h);
    uint8_t * tmp = malloc((size_t) w * h);
    if (!r || !g || !b || !tmp) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }

    /* Unpack RGB565 -> separate 8-bit-per-channel planes (blurring packed
     * 5/6/5 values directly isn't meaningful -- each channel needs its own
     * running sum), cropping to the source's bottom `h` rows and flipping
     * them vertically at the same time: source row (COVER_ART_HEIGHT-1-y)
     * becomes reflection row y, so the real album art's very last row
     * (right at the seam with this panel) becomes the reflection's first
     * row, mirroring exactly at the boundary between the two. */
    for (int y = 0; y < h; y++) {
        int src_y = COVER_ART_HEIGHT - 1 - y;
        const uint16_t * src_row = (const uint16_t *) (cover_bytes + (size_t) src_y * COVER_ART_WIDTH * 2);
        for (int x = 0; x < w; x++) {
            uint16_t px = src_row[x];
            r[y * w + x] = (uint8_t) ((px >> 11) & 0x1F);
            g[y * w + x] = (uint8_t) ((px >> 5) & 0x3F);
            b[y * w + x] = (uint8_t) (px & 0x1F);
        }
    }

    /* Separable box blur: one horizontal pass (per row) then one vertical
     * pass (per column), on each channel independently -- a full 2D blur
     * at a fraction of the cost of a real 2D kernel. Heavy (radius 16)
     * deliberately, per the "frosted glass" look this is going for rather
     * than a sharp mirror image. */
    for (int y = 0; y < h; y++) {
        box_blur_1d(r + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(r + y * w, tmp + y * w, (size_t) w);
        box_blur_1d(g + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(g + y * w, tmp + y * w, (size_t) w);
        box_blur_1d(b + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(b + y * w, tmp + y * w, (size_t) w);
    }
    for (int x = 0; x < w; x++) box_blur_1d(r + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
    memcpy(r, tmp, (size_t) w * h);
    for (int x = 0; x < w; x++) box_blur_1d(g + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
    memcpy(g, tmp, (size_t) w * h);
    for (int x = 0; x < w; x++) box_blur_1d(b + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
    memcpy(b, tmp, (size_t) w * h);

    uint8_t * out_bytes = malloc((size_t) w * h * 2);
    if (!out_bytes) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }

    /* Darken (mostly faded into player_overlay_panel's black background)
     * and repack into RGB565, same channel layout current_cover_dsc uses. */
    uint16_t * out = (uint16_t *) out_bytes;
    for (int i = 0; i < w * h; i++) {
        int rv = (r[i] * REFLECTION_DARKEN_NUM) / REFLECTION_DARKEN_DEN;
        int gv = (g[i] * REFLECTION_DARKEN_NUM) / REFLECTION_DARKEN_DEN;
        int bv = (b[i] * REFLECTION_DARKEN_NUM) / REFLECTION_DARKEN_DEN;
        out[i] = (uint16_t) ((rv << 11) | (gv << 5) | bv);
    }
    free(r); free(g); free(b); free(tmp);
    return out_bytes;
}

/* Real-device bug report: entering the player screen right after picking a
 * song left its on-screen play/pause/next/prev buttons unresponsive for a
 * couple of seconds. Root cause: apply_track_metadata_to_ui() used to
 * decode the embedded cover art (cover_decode_to_rgb565(), a full native-
 * resolution JPEG/PNG decode -- see cover_decode.c's own comment on why
 * this can't use tjpgd's faster scaled decode) and run the reflection blur
 * above synchronously, on the UI thread, before play_track_at_from() ever
 * reached nav_push(player_screen) -- easily 1-3+ seconds of pure blocking
 * on this hardware for a large embedded image, during which
 * lv_timer_handler() never runs, so nothing redraws and no touch input is
 * processed at all. Exact same shape of bug (and fix) as the real-device
 * incident documented at bt_apply_output_settings_thread_func()'s own
 * comment above -- backgrounded here the same way: launch_cover_decode()
 * kicks off the decode+blur on a pthread (cover_decode_to_rgb565() and
 * compute_reflection_bytes() touch no LVGL/shared state, so this is safe),
 * and poll_cover_decode() (called from update_timer_cb every tick) applies
 * the result to the actual widgets once it's ready -- so nav_push() and
 * audio_play_file_at() in play_track_at_from() now run immediately, with
 * the cover art/reflection catching up a moment later instead of blocking
 * everything else on the way there. */
typedef struct {
    int for_index;
    uint8_t * picture_data; /* owned; NULL if the track has no embedded art */
    uint32_t picture_size;

    /* Set instead of picture_data/picture_size for a Subsonic stream's cover
     * art (see launch_cover_decode_from_url()) -- there's no local file to
     * have already extracted embedded art from, so the compressed image
     * bytes are fetched here, on the same background thread, before falling
     * into the same decode path local art already uses. Empty string for
     * every other caller. */
    char stream_url[1536];
    bool stream_verify_tls;
} cover_decode_request_t;

static pthread_t cover_decode_thread;
static bool cover_decode_active = false;
static volatile bool cover_decode_done_flag = false;

/* Result fields, written by cover_decode_thread_func() on the background
 * thread and consumed (freed or applied) by poll_cover_decode() on the main
 * thread once cover_decode_done_flag is seen true -- never touched by both
 * at once, same "background thread writes then sets the flag last, main
 * thread only reads after seeing the flag" contract as every other _done_
 * flag in this file. */
static int cover_decode_result_for_index;
static bool cover_decode_result_ok;
static uint16_t * cover_decode_result_pixels;    /* COVER_ART_WIDTH x COVER_ART_HEIGHT RGB565, owned */
static uint8_t * cover_decode_result_reflection; /* REFLECTION_WIDTH x REFLECTION_HEIGHT RGB565, owned */

static void launch_cover_decode(int for_index, uint8_t * picture_data, uint32_t picture_size);

/* Holds at most one superseded request -- see launch_cover_decode()'s own
 * comment on why a track change that arrives while a decode is already in
 * flight can't just be dropped (that would leave the cover permanently
 * stuck showing an earlier track's art). */
static bool cover_decode_pending_valid = false;
static cover_decode_request_t cover_decode_pending;

static void * cover_decode_thread_func(void * arg) {
    cover_decode_request_t * req = (cover_decode_request_t *) arg;

    if (req->stream_url[0] != '\0') {
        int status = 0;
        uint8_t * body = NULL;
        size_t body_size = 0;
        if (http_get_to_buffer(req->stream_url, req->stream_verify_tls, &status, &body, &body_size) && status == 200) {
            req->picture_data = body;
            req->picture_size = (uint32_t) body_size;
        } else {
            free(body);
            /* Left NULL/0 -- decode below no-ops, same as "no embedded art"
             * for a local file. No error surfaced beyond that; a failed
             * cover-art fetch isn't worth blocking or retrying playback
             * over. */
        }
    }

    uint16_t * pixels = NULL;
    bool ok = req->picture_data && req->picture_size > 0 &&
              cover_decode_to_rgb565(req->picture_data, req->picture_size, COVER_ART_WIDTH, COVER_ART_HEIGHT, &pixels);
    free(req->picture_data); /* the compressed bytes are no longer needed once decoded (or decode failed) */

    uint8_t * reflection = ok ? compute_reflection_bytes((const uint8_t *) pixels) : NULL;

    cover_decode_result_for_index = req->for_index;
    cover_decode_result_ok = ok;
    cover_decode_result_pixels = pixels;
    cover_decode_result_reflection = reflection;
    free(req);
    cover_decode_done_flag = true; /* written last -- poll_cover_decode() only checks this flag */
    return NULL;
}

/* Takes ownership of picture_data (a malloc()'d buffer from metadata_read(),
 * or NULL). If a decode is already running, this doesn't launch a second
 * one (cover_decode_to_rgb565()/compute_reflection_bytes() are reentrant --
 * no shared state -- but there's no value in two decodes racing when only
 * the newest one's result will ever get applied) -- it instead replaces
 * cover_decode_pending (freeing whatever was queued there before), and
 * poll_cover_decode() launches that pending request the moment the active
 * one finishes. A rapid sequence of track changes only ever actually
 * decodes the first and the final one, which is exactly the set of results
 * that matters. */
static void launch_cover_decode_req(cover_decode_request_t r) {
    if (cover_decode_active) {
        free(cover_decode_pending.picture_data);
        cover_decode_pending = r;
        cover_decode_pending_valid = true;
        return;
    }

    cover_decode_request_t * req = malloc(sizeof(*req));
    *req = r;
    cover_decode_done_flag = false;
    cover_decode_active = true;
    pthread_create(&cover_decode_thread, NULL, cover_decode_thread_func, req);
}

static void launch_cover_decode(int for_index, uint8_t * picture_data, uint32_t picture_size) {
    cover_decode_request_t r = { .for_index = for_index, .picture_data = picture_data, .picture_size = picture_size,
                                  .stream_url = "", .stream_verify_tls = false };
    launch_cover_decode_req(r);
}

/* Subsonic streaming's own art source -- see cover_decode_request_t's own
 * comment. url is subsonic_build_cover_art_url()'s output, fetched on the
 * same background thread that would otherwise be decoding already-local
 * bytes, so a slow/flaky connection can't block the UI here either, same
 * reasoning as launch_cover_decode() itself. */
static void launch_cover_decode_from_url(int for_index, const char * url, bool verify_tls) {
    cover_decode_request_t r = { .for_index = for_index, .picture_data = NULL, .picture_size = 0,
                                  .stream_verify_tls = verify_tls };
    snprintf(r.stream_url, sizeof(r.stream_url), "%s", url);
    launch_cover_decode_req(r);
}

/* Called every tick from update_timer_cb. Applies the finished decode to
 * cover_img/player_overlay_panel -- unless playlist_index has already moved
 * on to a different track by the time this lands (another launch_cover_
 * decode() call superseded it via cover_decode_pending, in which case that
 * one is now either running or about to be), in which case the result is
 * just discarded rather than briefly flashing a stale track's art. */
static void poll_cover_decode(void) {
    if (!cover_decode_active || !cover_decode_done_flag) return;
    cover_decode_active = false;
    pthread_join(cover_decode_thread, NULL);

    if (cover_decode_result_for_index != playlist_index) {
        free(cover_decode_result_pixels);
        free(cover_decode_result_reflection);
    } else if (!cover_decode_result_ok) {
        free(current_cover_bytes);
        current_cover_bytes = NULL;
        lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
        /* No in-memory raw bitmap to reflect for the static placeholder
         * cover -- reset the panel back to its plain background rather
         * than leaving a stale reflection from whatever track played
         * before this one. */
        free(current_reflection_bytes);
        current_reflection_bytes = NULL;
        lv_obj_set_style_bg_image_src(player_overlay_panel, asset_path("playing_plane/buttom.png"), 0);
    } else {
        free(current_cover_bytes);
        current_cover_bytes = (uint8_t *) cover_decode_result_pixels;
        free(current_reflection_bytes);
        current_reflection_bytes = cover_decode_result_reflection;

        /* Real-device incident: "severe noise/corruption" on the actual on-
         * screen cover art, root-caused via a raw dump of current_cover_bytes
         * taken the instant cover_decode_to_rgb565() returns (before this
         * point) -- a correct, undamaged image every time, on the real
         * target hardware, not just a host-build test, which ruled out the
         * decode pipeline entirely. An intermediate theory (LVGL's image
         * cache, keyed on the source pointer, serving stale tiles since
         * current_cover_dsc is a reused static struct) turned out to be
         * wrong too -- lv_image_cache_drop() before every reassignment made
         * no difference, and the same corruption showed up on a completely
         * different track's art, not just a stale leftover from the
         * previous one.
         *
         * The real bug: lv_image_header_t.magic (lv_image_dsc.h) must be
         * LV_IMAGE_HEADER_MAGIC -- lv_bin_decoder.c (the decoder this hand-
         * constructed raw descriptor actually goes through) treats any other
         * value as an old-format header from before the magic field existed,
         * and "fixes it up" in place via `header->cf = header->magic;
         * header->magic = LV_IMAGE_HEADER_MAGIC;`. memset()ing the whole
         * descriptor to 0 before setting cf/w/h/stride left magic at 0, so
         * this quirks-mode shim fired on literally every track change,
         * silently overwriting our just-set LV_COLOR_FORMAT_RGB565 with 0
         * right before the image ever got drawn -- corrupting the color
         * format used to interpret every pixel, not the pixel data itself,
         * which is exactly why the underlying buffer always dumped correctly
         * but the screen never showed it right. */
        memset(&current_cover_dsc, 0, sizeof(current_cover_dsc));
        current_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        current_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        current_cover_dsc.header.w = COVER_ART_WIDTH;
        current_cover_dsc.header.h = COVER_ART_HEIGHT;
        current_cover_dsc.header.stride = COVER_ART_WIDTH * 2;
        current_cover_dsc.data = current_cover_bytes;
        current_cover_dsc.data_size = (uint32_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2;
        lv_image_set_src(cover_img, &current_cover_dsc);

        /* Same LV_IMAGE_HEADER_MAGIC requirement as current_cover_dsc above. */
        memset(&current_reflection_dsc, 0, sizeof(current_reflection_dsc));
        current_reflection_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        current_reflection_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        current_reflection_dsc.header.w = REFLECTION_WIDTH;
        current_reflection_dsc.header.h = REFLECTION_HEIGHT;
        current_reflection_dsc.header.stride = REFLECTION_WIDTH * 2;
        current_reflection_dsc.data = current_reflection_bytes;
        current_reflection_dsc.data_size = (uint32_t) REFLECTION_WIDTH * REFLECTION_HEIGHT * 2;
        lv_obj_set_style_bg_image_src(player_overlay_panel, &current_reflection_dsc, 0);

        /* TEMPORARY DIAGNOSTIC -- forcing a full-screen invalidate (not just
         * cover_img's own, which lv_image_set_src() already does internally)
         * to test whether this device's custom direct-render/page-flip fbdev
         * patch (lv_conf.h, search "pan_double_buffer") has a bug specific to
         * how the dirty region is computed for a single large (480x480)
         * widget update, vs however a full-screen invalidate's dirty region
         * ends up differing. Remove once the real album-art corruption bug
         * is found. */
        lv_obj_invalidate(lv_screen_active());
    }

    cover_decode_result_pixels = NULL;
    cover_decode_result_reflection = NULL;

    if (cover_decode_pending_valid) {
        cover_decode_pending_valid = false;
        launch_cover_decode_req(cover_decode_pending); /* not launch_cover_decode() -- must carry stream_url too, see that field's own comment */
    }
}

static void favorite_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    favorite_is_set = !favorite_is_set;
    metadata_db_song_favorite_set(playlist[playlist_index], favorite_is_set);
    lv_image_set_src(favorite_icon, asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
    if (quick_drawer_favorite_icon) {
        lv_image_set_src(quick_drawer_favorite_icon,
                         asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
    }
}

static void arm_next_track_for_audio(int index);

/* Shared by the player screen's own order_icon tap and the Remote Control
 * web UI's mode-cycle button (remote_control_consume_mode_cycle()) -- same
 * single 4-state cycle either way, so both surfaces stay in sync and
 * neither reimplements the mode-advance/persist/re-arm logic. */
static void cycle_play_mode(void) {
    play_mode_t mode = (play_mode_t) current_settings.play_mode;
    mode = (play_mode_t) ((mode + 1) % 4);
    current_settings.play_mode = (int) mode;
    settings_save(&current_settings);

    lv_image_set_src(order_icon, asset_path(play_mode_icon_asset(mode)));
    if (quick_drawer_order_icon) lv_image_set_src(quick_drawer_order_icon, asset_path(play_mode_icon_asset(mode)));

    /* What comes after the current track changes with the mode (e.g.
     * entering Shuffle picks a random next instead of index+1) -- re-arm the
     * gapless/crossfade target immediately rather than waiting for whatever
     * was armed under the old mode to turn out wrong. */
    if (playlist_index >= 0) arm_next_track_for_audio(playlist_index);
}

static void order_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cycle_play_mode();
}

/* find_song_index_by_path() is defined down with the rest of the Favorites/
 * Most Played machinery (needs all_songs_paths already in scope there);
 * refresh_now_playing_indicators() is defined down with the compact-list
 * screens it updates (Artists/Albums/All Songs/group songs). Both are
 * forward-declared here since apply_track_metadata_to_ui() below is the
 * single place a real "this track started playing" event is known to have
 * happened, for every playback source (local library, Group Songs, Files,
 * .m3u playlists). */
static int find_song_index_by_path(const char * path);
static void refresh_now_playing_indicators(void);

/* Shared by both an explicit track pick (play_track_at_from) and the audio
 * thread autonomously advancing into a queued next track on its own
 * (on_track_auto_advanced) -- title/folder/art/format-badge/progress-reset
 * are identical either way. Returns the metadata it read (out_meta) so
 * callers that also need ReplayGain (play_track_at_from, to hand it to
 * audio_play_file_at) don't have to read the file's tags a second time. */
/* Set by subsonic_song_row_click_cb() (gui.c, further down) right before a
 * streamed mp3/flac Subsonic queue starts playing -- metadata_read() can't
 * read tags from a network URL the way it does a local file, but the real
 * title/artist/album (and where to fetch cover art) for every song in the
 * queue are already known from the API responses that built it, so they're
 * stashed here, one entry per playlist[] slot (same indexing, built and
 * freed together), and matched by exact URL string in apply_track_metadata_
 * to_ui() below -- the string match (not just the index) is what makes it
 * safe to leave this array up indefinitely rather than needing to track
 * every other place a new, unrelated playlist might start: subsonic_build_
 * stream_url() bakes a fresh random salt into every URL it builds, so a
 * later, different playlist's paths can never coincidentally match one of
 * these even if this array outlives its own playlist (the array is only
 * ever replaced, on the next Subsonic streaming tap, not proactively freed
 * when some other playback source starts). Forward-declared here (rather
 * than moving apply_track_metadata_to_ui() itself) since the Subsonic click
 * handler that WRITES these lives much further down this file, alongside
 * the rest of the Subsonic screens. */
typedef struct {
    char url[1536]; /* the exact playlist[i] this entry describes */
    char title[128];
    char artist[128];
    char album[128];
    char cover_url[1536];
    bool verify_tls;
} subsonic_stream_song_meta_t;
static subsonic_stream_song_meta_t * subsonic_stream_meta = NULL; /* parallel array, NULL when no Subsonic stream queue is loaded */
static int subsonic_stream_meta_count = 0;

static void apply_track_metadata_to_ui(int index, track_metadata_t * out_meta) {
    char title[128];
    char folder[128];
    get_display_names(playlist[index], title, sizeof(title), folder, sizeof(folder));

    bool is_subsonic_stream = subsonic_stream_meta && index < subsonic_stream_meta_count &&
                               strcmp(playlist[index], subsonic_stream_meta[index].url) == 0;

    if (is_subsonic_stream) {
        memset(out_meta, 0, sizeof(*out_meta));
        if (subsonic_stream_meta[index].title[0]) {
            snprintf(out_meta->title, sizeof(out_meta->title), "%s", subsonic_stream_meta[index].title);
            out_meta->has_title = true;
        }
        if (subsonic_stream_meta[index].artist[0]) {
            snprintf(out_meta->artist, sizeof(out_meta->artist), "%s", subsonic_stream_meta[index].artist);
            out_meta->has_artist = true;
        }
        if (subsonic_stream_meta[index].album[0]) {
            snprintf(out_meta->album, sizeof(out_meta->album), "%s", subsonic_stream_meta[index].album);
            out_meta->has_album = true;
        }
    } else {
        metadata_read(playlist[index], out_meta);
    }

    /* -1 (not found) is a real, expected outcome, not just "library not
     * scanned yet" -- Group Songs/Files/.m3u playback can hand play_track_
     * at_from() a path outside MUSIC_ROOT_DIR, or the library scan simply
     * hasn't indexed this file for some other reason. refresh_now_playing_
     * indicators() below treats -1 as "clear every indicator". */
    now_playing_song_index = find_song_index_by_path(playlist[index]);
    refresh_now_playing_indicators();

    const char * title_text = out_meta->has_title ? out_meta->title : title;
    const char * folder_text = out_meta->has_artist ? out_meta->artist : folder;

    lv_label_set_text(song_title_label, title_text);
    lv_label_set_text(song_folder_label, folder_text);
    if (quick_drawer_title_label) {
        lv_label_set_text(quick_drawer_title_label, title_text);
        lv_label_set_text(quick_drawer_artist_label, folder_text);
    }
    refresh_format_badge();
    if (is_subsonic_stream && subsonic_stream_meta[index].cover_url[0]) {
        launch_cover_decode_from_url(index, subsonic_stream_meta[index].cover_url, subsonic_stream_meta[index].verify_tls);
    } else {
        launch_cover_decode(index, out_meta->picture_data, out_meta->picture_size); /* takes ownership, applied async */
    }

    favorite_is_set = metadata_db_song_favorite_is_set(playlist[index]);
    const char * favorite_icon_asset = favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png";
    lv_image_set_src(favorite_icon, asset_path(favorite_icon_asset));
    if (quick_drawer_favorite_icon) lv_image_set_src(quick_drawer_favorite_icon, asset_path(favorite_icon_asset));

    /* Once per real "this track started playing" event -- apply_track_
     * metadata_to_ui() is called exactly here for both an explicit pick
     * (play_track_at_from) and a gapless auto-advance (on_track_
     * auto_advanced), never on a repeat UI refresh of the same still-
     * playing track, so this can't double-count. Backs the Most Played
     * auto-generated playlist (Music > Playlists). */
    metadata_db_song_play_count_increment(playlist[index]);

    /* The real position/duration come from audio_get_*_seconds() once the
     * decoder's opened -- the timer picks that up within its next tick. */
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    lv_label_set_text(pos_label, "0:00");
    lv_label_set_text(dur_label, "0:00");

    lv_label_set_text_fmt(song_count_label, "%d/%d", index + 1, playlist_count);
}

/* Tells audio.c what comes after `index` (per the current play mode -- see
 * compute_auto_advance_index()) so its playback thread can gapless-handoff
 * or crossfade into it near `index`'s natural end without a GUI round-trip.
 * Must be re-called (from on_track_auto_advanced) every time the thread
 * advances on its own, or the chain of automatic transitions stops after
 * one hop. */
static void arm_next_track_for_audio(int index) {
    int next_index = compute_auto_advance_index(index);
    if (next_index < 0) {
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
        return;
    }
    track_metadata_t next_meta;
    metadata_read(playlist[next_index], &next_meta);
    audio_set_next_track(playlist[next_index], next_meta.has_replaygain, next_meta.replaygain_gain_db,
                          next_meta.has_replaygain_peak, next_meta.replaygain_peak);
    free(next_meta.picture_data); /* only needed the gain/peak fields, not the art */
}

/* Song long-press context menu's "Add to Queue" -- splices `path` into the
 * live playlist right after whatever was queued last (or right after the
 * currently-playing track, if the queue's currently empty), so repeated
 * Add to Queue taps play back in the order they were added. See
 * queued_pending_count's own comment for why this reuses playlist[]
 * directly instead of a separate list. No-op with a toast if nothing's
 * playing -- there's no "currently playing track" position to queue
 * after. */
static void queue_add_song(const char * path) {
    if (playlist_index < 0 || !playlist) {
        show_error_toast("Nothing is playing");
        return;
    }

    int pos = (queue_next_insert_index >= 0 && queue_next_insert_index <= playlist_count) ? queue_next_insert_index
                                                                                            : playlist_index + 1;

    char ** grown = realloc(playlist, sizeof(char *) * (size_t) (playlist_count + 1));
    playlist = grown;
    memmove(&playlist[pos + 1], &playlist[pos], sizeof(char *) * (size_t) (playlist_count - pos));
    playlist[pos] = strdup(path);
    playlist_count++;
    queued_pending_count++;
    queue_next_insert_index = pos + 1;

    lv_label_set_text_fmt(song_count_label, "%d/%d", playlist_index + 1, playlist_count);
    /* Re-arm gapless preload -- what comes right after playlist_index may
     * have just changed (a brand new queue, or this insert landing exactly
     * there). */
    arm_next_track_for_audio(playlist_index);

    show_info_toast("Added to queue");
}

/* Bluetooth DAC mode and AirPlay receive mode both feed real-time audio
 * from another device straight into this device's own physical ALSA
 * hardware (hw:0,0) -- confirmed by checking what each one's consumer
 * process (aplay -D bluealsa; shairport -o ot) actually targets. Local
 * playback uses that same hardware directly via tinyalsa, so all three are
 * mutually exclusive: only one may be "using" the speaker/DAC output at a
 * time. bt_dac_toggle_cb()/airplay_toggle_cb() turn each other off when
 * either is enabled; this covers local playback's side of the same rule.
 *
 * bt_dac_mode_enabled is also gated on bt_is_powered_cached: it's a
 * persisted setting, so it can still read true from a previous session
 * after a reboot where Bluetooth was never turned back on (chip
 * re-init isn't automatic -- see TESTING.md). With Bluetooth actually off,
 * bt_control_apply_output_settings() never started bluealsa/aplay, so
 * nothing is really using the DAC output -- blocking playback in that case
 * was a real dead end: the Bluetooth DAC toggle to turn it back off only
 * appears once Bluetooth is powered, which needs a chip re-init the app
 * itself has no way to trigger. */
static const char * external_dac_block_reason(void) {
    if (current_settings.bt_dac_mode_enabled && bt_is_powered_cached) return "Turn off Bluetooth DAC to play music on this device";
    if (current_settings.wifi_dac_mode_enabled) return "Turn off AirPlay to play music on this device";
    if (current_settings.usb_mode == USB_MODE_DAC) return "Exit USB DAC mode to play music on this device";
    return NULL;
}

/* Cached now-playing metadata for plugin.get_now_playing() -- populated at
 * the same two call sites that fire the "track_started" plugin event
 * (notify_plugin_track_started() below), so there's no separate tracking
 * needed. plugin_now_playing_loaded distinguishes "nothing has ever played
 * this session" from "a track is playing with an empty title" -- the title
 * buffer alone can't tell those apart. */
static char plugin_now_playing_title[128];
static char plugin_now_playing_artist[128];
static char plugin_now_playing_album[128];
static double plugin_now_playing_duration;
static bool plugin_now_playing_loaded = false;

/* Shared by play_track_at_from()/on_track_auto_advanced() below -- caches
 * meta into the plugin_now_playing_* globals above and fires the
 * "track_started" plugin event. Called after audio_play_file_at()/the
 * gapless handoff has already happened in both call sites, so
 * audio_get_duration_seconds() reflects the new track. */
static void notify_plugin_track_started(const track_metadata_t * meta) {
    snprintf(plugin_now_playing_title, sizeof(plugin_now_playing_title), "%s", meta->title);
    snprintf(plugin_now_playing_artist, sizeof(plugin_now_playing_artist), "%s", meta->artist);
    snprintf(plugin_now_playing_album, sizeof(plugin_now_playing_album), "%s", meta->album);
    plugin_now_playing_duration = audio_get_duration_seconds();
    plugin_now_playing_loaded = true;

    plugin_manager_notify_track_started(meta->title, meta->artist, meta->album, plugin_now_playing_duration);
}

static void play_track_at_from(int index, double start_seconds) {
    if (index < 0 || index >= playlist_count) return;

    const char * block_reason = external_dac_block_reason();
    if (block_reason) {
        show_error_toast(block_reason);
        return;
    }

    playlist_index = index;

    track_metadata_t meta;
    apply_track_metadata_to_ui(index, &meta);
    audio_play_file_at(playlist[index], start_seconds, meta.has_replaygain, meta.replaygain_gain_db,
                        meta.has_replaygain_peak, meta.replaygain_peak);
    arm_next_track_for_audio(index);

#ifndef HOST_BUILD
    hiby_sys_server_report_metadata(meta.title, meta.artist, meta.album, meta.genre,
                                     (long) (audio_get_duration_seconds() * 1000.0));
    hiby_sys_server_report_position((long) (start_seconds * 1000.0));
#endif
    notify_plugin_track_started(&meta);

    set_play_button_state(true);
    nav_push(player_screen);

    snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", playlist[index]);
    current_settings.last_position = start_seconds;
    settings_save(&current_settings);
}

/* Called from update_timer_cb when audio_consume_track_advanced() reports
 * the playback thread moved on to the queued next track by itself (gapless
 * handoff or a completed crossfade) -- audio is already playing it, so
 * unlike play_track_at_from() this must NOT call audio_play_file_at()
 * (that would hard-restart audio that's already mid-track). */
static void on_track_auto_advanced(int index) {
    if (index < 0 || index >= playlist_count) return;

    playlist_index = index;

    track_metadata_t meta;
    apply_track_metadata_to_ui(index, &meta); /* audio.c already applied this track's ReplayGain during the handoff */
    arm_next_track_for_audio(index);

#ifndef HOST_BUILD
    hiby_sys_server_report_metadata(meta.title, meta.artist, meta.album, meta.genre,
                                     (long) (audio_get_duration_seconds() * 1000.0));
    hiby_sys_server_report_position(0);
#endif
    notify_plugin_track_started(&meta);

    set_play_button_state(true);

    snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", playlist[index]);
    current_settings.last_position = 0.0;
    settings_save(&current_settings);
}

static void play_track_at(int index) {
    play_track_at_from(index, 0.0);
}

static void on_file_selected(char ** new_playlist, int count, int selected_index) {
    free_playlist();
    playlist = new_playlist;
    playlist_count = count;
    /* A brand new playback context (a fresh song tapped from any list)
     * invalidates whatever was queued against the OLD playlist[] -- those
     * array positions no longer mean anything once the array itself has
     * been replaced. Matches every other music app: starting something new
     * clears "Up Next". */
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    play_track_at(selected_index);
}

/* set_player_source_group_songs() is defined further down, right after
 * group_songs_indices/count/title_label -- it needs those already in
 * scope. These three don't. */
static void clear_player_source(void) {
    player_source_kind = PLAYER_SOURCE_NONE;
    player_source_all_songs_index = -1;
    free(player_source_group_title);
    player_source_group_title = NULL;
    free(player_source_group_indices);
    player_source_group_indices = NULL;
    player_source_group_count = 0;
    player_source_group_pos = -1;
    player_source_file_browser_row = -1;
}

static void set_player_source_all_songs(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_ALL_SONGS;
    player_source_all_songs_index = display_index;
}

static void set_player_source_file_browser(const char * dir, int row) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_FILE_BROWSER;
    snprintf(player_source_file_browser_dir, sizeof(player_source_file_browser_dir), "%s", dir);
    player_source_file_browser_row = row;
}

/* Wraps on_file_selected() as file_browser_init()'s select_cb, rather than
 * passing on_file_selected directly, so the source snapshot above only
 * ever gets set for an actual folder-browse tap -- on_file_selected()
 * itself is shared by every play-launch path (All Songs, group songs,
 * Subsonic downloads, DLNA casts, ...) and has no way to tell which of
 * them is calling it. file_browser_get_last_selected_dir()/_row() are
 * only valid synchronously within file_browser.c's own select_cb() call,
 * which is exactly where this reads them. */
static void on_file_browser_selected(char ** new_playlist, int count, int selected_index) {
    set_player_source_file_browser(file_browser_get_last_selected_dir(), file_browser_get_last_selected_row());
    on_file_selected(new_playlist, count, selected_index);
}

static void toggle_play_pause(void) {
    if (playlist_index < 0) return; /* nothing loaded yet */
    /* Same DAC-mode exclusion as play_track_at_from() -- only blocks
     * resuming (paused -> playing), pausing an already-playing track always
     * goes through (though bt_dac_toggle_cb()/airplay_toggle_cb() already
     * stop playback the moment either DAC mode turns on, so in practice
     * there's nothing left playing to pause by the time this could
     * matter). */
    if (!audio_is_playing()) {
        const char * block_reason = external_dac_block_reason();
        if (block_reason) {
            show_error_toast(block_reason);
            return;
        }
    }
    audio_toggle_pause();
    bool now_playing = audio_is_playing();
    set_play_button_state(now_playing);

    if (now_playing) {
        plugin_manager_notify_resumed();
    } else {
        /* Checkpoint the resume position on pause -- a natural point to
         * persist, and far less write-heavy than saving on every tick. */
        current_settings.last_position = audio_get_position_seconds();
        settings_save(&current_settings);
        plugin_manager_notify_paused();
    }
}

static void play_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    toggle_play_pause();
}

/* Standard CD-player/iPod convention: a tap partway into a track restarts
 * it, and only a tap already near the start (real device feedback: "first
 * press rewind, second press goes to previous song") moves to the actual
 * previous track -- no separate double-tap timer needed, since "already
 * near the start" is naturally true right after the first tap rewound it. */
#define PREV_BUTTON_REWIND_THRESHOLD_SECONDS 3.0

static void prev_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0) return;

    if (audio_get_position_seconds() > PREV_BUTTON_REWIND_THRESHOLD_SECONDS) {
        audio_seek(0.0);
        return;
    }

    int prev_index = compute_manual_step_index(playlist_index, -1);
    if (prev_index >= 0) play_track_at(prev_index);
}

static void next_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0) return;
    int next_index = compute_manual_step_index(playlist_index, 1);
    if (next_index >= 0) play_track_at(next_index);
}

static void library_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Functionally "go back to wherever playback was started from" -- the
     * player screen is always reached via nav_push(), so popping is correct
     * here rather than a hardcoded jump to a fixed screen. */
    nav_pop();
}

static void crossfade_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.crossfade_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    audio_set_crossfade_enabled(current_settings.crossfade_enabled);
    settings_save(&current_settings);
    refresh_quick_drawer_crossfade_icon(); /* see its own comment -- keeps the drawer icon in sync */
}

static void car_mode_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.car_mode_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    if (current_settings.car_mode_enabled) {
        show_info_toast("Car Mode powers the device off when it loses external power, and "
                         "automatically resumes playback once power is restored.");
    }
}

static void swipe_up_home_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.swipe_up_home_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (current_settings.swipe_up_home_enabled) {
        lv_obj_remove_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
    }
    settings_save(&current_settings);
}

static void led_indicator_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.led_indicator_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    led_control_apply(current_settings.led_indicator_enabled);
    settings_save(&current_settings);
}

static void charge_limiter_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.charge_limiter_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    settings_save(&current_settings);
}

/* Log-scale mapping so the slider gives fine control at low frequencies
 * (where the ear is more sensitive to small Hz changes) instead of wasting
 * most of the slider's travel on the top octave. */
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

/* Forward-declared: show_text_entry() and its callback typedef aren't
 * defined until much later in this file (the shared text-entry screen,
 * also used by Subsonic/Wi-Fi setup), but the PEQ tap-to-edit handlers
 * below need to call it. */
typedef void (*text_entry_done_cb_t)(const char * text, void * user_data);
static void show_text_entry(const char * title, const char * initial_text, bool is_password, bool numeric,
                             text_entry_done_cb_t on_done, void * user_data);

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

    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        bool selected = (i == current_eq_band);
        /* Toggling which SHARED style is attached (rather than setting a
         * one-off direct color) so this keeps tracking the accent color
         * live if the user changes it later -- a direct
         * lv_obj_set_style_text_color() would sit in the object's local
         * style, which takes priority over any added style and would
         * silently defeat style_accent. */
        lv_obj_remove_style(eq_band_button_labels[i], &style_accent, 0);
        lv_obj_remove_style(eq_band_button_labels[i], &style_muted_text, 0);
        lv_obj_add_style(eq_band_button_labels[i], selected ? &style_accent : &style_muted_text, 0);
    }

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

static void eq_band_button_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_eq_band = (int) (intptr_t) lv_event_get_user_data(e);
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

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, freq, band->gain_db, band->q);
        lv_label_set_text_fmt(eq_freq_value_label, "%.0f Hz", freq);
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
        lv_label_set_text_fmt(eq_gain_value_label, "%+.1f dB", gain);
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
        lv_label_set_text_fmt(eq_q_value_label, "Q %.2f", q);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void progress_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        user_seeking = true;
    } else if (code == LV_EVENT_RELEASED) {
        double duration = audio_get_duration_seconds();
        int32_t percent = lv_slider_get_value(slider);
        audio_seek(duration * ((double) percent / 100.0));
        user_seeking = false;
    }
}

static void volume_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        audio_set_volume((float) percent / 100.0f); /* live feedback while dragging */
    } else if (code == LV_EVENT_RELEASED) {
        /* Only persist once the drag settles, not on every intermediate tick. */
        current_settings.volume = (float) percent / 100.0f;
        settings_save(&current_settings);
    }
}

/* Defined with the rest of the Subsonic streaming logic further down;
 * forward-declared here since polling for a finished background download
 * belongs alongside every other per-tick "did a background thing finish"
 * check in update_timer_cb (hw_buttons, audio_consume_track_advanced, ...). */
static void poll_subsonic_download(void);
static void poll_subsonic_library_download(void);
static void poll_dlna_control(void);
static void poll_subsonic_connect(void);
/* Defined alongside library_scan_once()'s own background-thread wrapper,
 * much further down -- forward-declared here since subsonic_library_
 * download_thread_func() (defined well before that point) needs to trigger
 * a rescan once its own batch download finishes. */
static void start_library_rescan(void);
static void poll_wifi_scan(void);
static void poll_wifi_connect(void);
static void poll_wifi_connect_saved(void);
static void poll_wifi_disconnect(void);
static void poll_wifi_forget(void);
static void poll_bt_scan(void);
static void poll_bt_connect(void);
static void poll_bt_forget(void);
static void poll_library_rescan(void);
static void poll_usb_mode_switch(void);
static void poll_sd_card_hotplug(void);
static void poll_sd_format(void);
static void poll_import_web_stop(void);

/* Full radio suspend after a long stretch with the screen off and nothing
 * going on -- deliberately separate from (and much longer than) the
 * screen-timeout backlight-off state itself: that can fire as often as
 * every 30s from ordinary momentary inactivity during otherwise-continuous
 * use, and WiFi/Bluetooth reassociation has a real cost, so tying radio
 * suspend to the same short timer would punish a user who's still actively
 * using the device, just with the screen dimming between taps. Skipped
 * entirely while music is playing, either DAC receive mode is on (both need
 * their radio to stay up to receive), or the device is charging (no battery
 * pressure to justify the reassociation cost). Whichever radios were
 * actually on get suspended, and only those get resumed on wake -- a radio
 * the user already had off before sleeping stays off. */
#define RADIO_SUSPEND_DELAY_MS (10 * 60 * 1000)
static uint32_t screen_off_since_tick = 0;
static bool radios_suspended = false;
static bool wifi_was_on_before_suspend = false;
static bool bt_was_on_before_suspend = false;

/* Idle shutdown: a full poweroff (see idle_shutdown.h) after
 * current_settings.idle_shutdown_minutes with the screen off and nothing
 * going on -- same gating as radio-suspend above (not playing, not
 * charging, no DAC receive mode active) since none of those should ever be
 * interrupted by the device turning itself off. idle_shutdown_attempted
 * guards against retrying every single tick if idle_shutdown_now() doesn't
 * actually terminate the process for some reason (e.g. /sbin/poweroff
 * missing) -- reset on wake so a genuine idle stretch always gets a fresh
 * attempt rather than being permanently given up on after one failure. */
static bool idle_shutdown_attempted = false;

/* Real-device bug report: waking from suspend needed two power-button
 * presses -- see the resume fixup below (right after power_suspend_now())
 * for the full mechanism, and its own comment for the actual confirmed
 * root cause (an unreset LVGL inactivity clock, not this grace window --
 * this window handles a separate, secondary race only). Short grace
 * window, not a one-shot drain: the physical press that wakes the kernel
 * is captured by hw_buttons.c's own independent evdev reader thread, whose
 * timing relative to this (main) thread's own resume handling isn't
 * guaranteed -- a drain attempted too early could miss a flag that thread
 * hadn't set yet. Any power-button press consumed within this window of a
 * resume is treated as an echo of the wake press and silently discarded
 * rather than toggling the screen; long enough to comfortably cover
 * realistic evdev delivery latency, short enough that a genuinely
 * deliberate second press to go back to sleep right after waking is a rare
 * enough tradeoff to accept. */
#define RESUME_POWER_DRAIN_WINDOW_MS 1000
static uint32_t resumed_from_suspend_tick = 0;
static bool resumed_from_suspend_pending = false;

/* Everything a caller of power_suspend_now() needs to do immediately after
 * it returns, factored out so Car Mode (below) can share it with the
 * idle-shutdown "suspend instead of power off" path this was originally
 * written for -- see that call site's own comment for the full history of
 * each individual fixup (backlight/indev resync, the inactivity-timer
 * flash-on-then-off bug, the two-presses-to-wake bug). Doesn't call
 * power_suspend_now() itself: the two callers gate entry into suspend
 * differently (a screen-off idle timer vs. a charging-edge transition), so
 * each still makes that call directly, right before calling this. */
static void resume_from_suspend_fixups(void) {
    backlight_set_screen_on(true);
    lv_indev_enable(NULL, true);
    lv_obj_invalidate(lv_screen_active());
    lv_display_trigger_activity(NULL);
    resumed_from_suspend_tick = lv_tick_get();
    resumed_from_suspend_pending = true;
    DBG_LOG("resume: grace window armed at tick=%u\n", resumed_from_suspend_tick);
}

/* Defined with the rest of the All Songs screen, much further down --
 * forward-declared here since update_timer_cb() (just below) needs it for
 * the remote-control play-by-index consumer. */
static void all_songs_row_click_cb(int index);
static int all_songs_count;
static int * all_songs_sort_order;
/* Defined with the rest of the remote-control scoped-play machinery, much
 * further down (needs find_song_index_by_path()/artist_groups/etc. already
 * in scope) -- forward-declared here since update_timer_cb() (just below)
 * needs it for the remote-control play-by-index consumer. */
static void play_remote_control_song(int song_index, const char * playlist_name, const char * artist_filter,
                                      const char * album_artist_filter, const char * album_filter);

/* Defined with the rest of the power-off countdown popup, much further
 * down -- forward-declared here since update_timer_cb() (just below) needs
 * them for the power-button long-press consumer. */
static void start_power_off_countdown(void);
static void poll_power_off_countdown(void);

static void update_timer_cb(lv_timer_t * timer) {
    (void) timer;

    /* Physical volume/skip/play-pause buttons: applied here, on the one
     * thread allowed to touch LVGL widgets (see hw_buttons.h). */
    bool played_paused = hw_buttons_consume_play_pause();
    if (played_paused) {
        toggle_play_pause();
    }
    /* Real-device bug report: shuffle "sometimes shuffles, sometimes just
     * plays the next song," and physical-button skip never shuffled at all.
     * Root cause -- this block used to step playlist_index by a plain +-1
     * regardless of play mode, rather than going through
     * compute_manual_step_index() (the same shuffle-aware/repeat-all-wrap
     * logic the touchscreen Prev/Next buttons already used, further down in
     * next_btn_event_cb/prev_btn_event_cb) -- so a physical button press
     * always played the literal next track in playlist order no matter what
     * current_settings.play_mode was. In Shuffle mode specifically, the
     * *touchscreen* buttons genuinely did shuffle every time; the
     * "sometimes" in the report was almost certainly this exact
     * inconsistency between the two input paths, not real randomness in
     * the shuffle logic itself. */
    bool skipped_next = hw_buttons_consume_next();
    if (skipped_next) {
        int next_index = compute_manual_step_index(playlist_index, 1);
        if (next_index >= 0) play_track_at(next_index);
    }
    bool skipped_prev = hw_buttons_consume_prev();
    if (skipped_prev) {
        int prev_index = compute_manual_step_index(playlist_index, -1);
        if (prev_index >= 0) play_track_at(prev_index);
    }

#ifndef HOST_BUILD
    /* Bluetooth accessory's own play/pause/next/previous buttons -- same
     * "background thread sets a flag, this is the one thread allowed to
     * touch LVGL/playlist state" pattern as the physical hw_buttons above,
     * since bt_media_player.c's D-Bus dispatch runs on its own thread. Now
     * shares the same shuffle-aware compute_manual_step_index() stepping as
     * hw_buttons and the touchscreen buttons -- see the shuffle bug comment
     * just above; a remote's buttons should behave like every other
     * "skip" input, not bypass play mode entirely. */
    if (bt_media_player_consume_play_pause()) {
        toggle_play_pause();
    }
    if (bt_media_player_consume_next()) {
        int next_index = compute_manual_step_index(playlist_index, 1);
        if (next_index >= 0) play_track_at(next_index);
    }
    if (bt_media_player_consume_prev()) {
        int prev_index = compute_manual_step_index(playlist_index, -1);
        if (prev_index >= 0) play_track_at(prev_index);
    }
    /* Keeps PlaybackStatus accurate for whenever BlueZ/the accessory next
     * queries it -- cheap (no subprocess, just a mutex-protected bool),
     * safe to do unconditionally every tick rather than hunting down every
     * call site that can change play state. */
    bt_media_player_notify_playback_state(audio_is_playing());
#endif

    int volume_delta = hw_buttons_consume_volume_delta();
    if (volume_delta != 0) {
        int32_t new_percent = lv_slider_get_value(volume_slider) + volume_delta;
        if (new_percent < 0) new_percent = 0;
        if (new_percent > 100) new_percent = 100;
        lv_slider_set_value(volume_slider, new_percent, LV_ANIM_OFF);
        audio_set_volume((float) new_percent / 100.0f);
        current_settings.volume = (float) new_percent / 100.0f;
        settings_save(&current_settings);
        show_volume_popup(new_percent);
        refresh_volume_topbar(new_percent);
    }

    /* Phone remote-control (Phase 2, remote_control.h): same "background
     * thread sets a flag, this is the one thread allowed to touch LVGL/
     * audio state" pattern as hw_buttons/bt_media_player just above --
     * reuses the exact same shuffle-aware stepping and volume-persistence
     * shape as those, rather than a separate implementation. */
    if (remote_control_consume_play_pause()) {
        toggle_play_pause();
    }
    if (remote_control_consume_next()) {
        int next_index = compute_manual_step_index(playlist_index, 1);
        if (next_index >= 0) play_track_at(next_index);
    }
    if (remote_control_consume_prev()) {
        int prev_index = compute_manual_step_index(playlist_index, -1);
        if (prev_index >= 0) play_track_at(prev_index);
    }
    if (remote_control_consume_mode_cycle()) {
        cycle_play_mode();
    }
    int remote_seek_seconds;
    if (remote_control_consume_seek(&remote_seek_seconds)) {
        audio_seek((double) remote_seek_seconds);
    }
    int remote_volume_percent;
    if (remote_control_consume_volume(&remote_volume_percent)) {
        lv_slider_set_value(volume_slider, remote_volume_percent, LV_ANIM_OFF);
        audio_set_volume((float) remote_volume_percent / 100.0f);
        current_settings.volume = (float) remote_volume_percent / 100.0f;
        settings_save(&current_settings);
        show_volume_popup(remote_volume_percent);
        refresh_volume_topbar(remote_volume_percent);
    }
    int remote_play_index;
    char remote_play_playlist[128], remote_play_artist[128], remote_play_album_artist[128], remote_play_album[128];
    if (remote_control_consume_play_index(&remote_play_index, remote_play_playlist, sizeof(remote_play_playlist),
                                           remote_play_artist, sizeof(remote_play_artist), remote_play_album_artist,
                                           sizeof(remote_play_album_artist), remote_play_album,
                                           sizeof(remote_play_album))) {
        /* remote_play_index is a raw scan-order index into all_songs_paths/
         * all_song_tags (remote_control.c's own library index space -- see
         * remote_control_sync_library()'s doc comment). play_remote_control_
         * song() (defined with the rest of the remote-control scoped-play
         * machinery, much further down) resolves both this and the
         * playlist/artist/album context into the right playlist and
         * position, falling back to the whole library (same translation
         * this used to do inline) when no scope applies. */
        play_remote_control_song(remote_play_index, remote_play_playlist, remote_play_artist,
                                  remote_play_album_artist, remote_play_album);
    }

    /* Auto-stop on headphone-output loss: this hardware has no built-in
     * speaker, so a mid-playback disconnect (headphone jack pulled, or the
     * connected Bluetooth A2DP headphone drops) leaves nowhere for the
     * audio to go -- stop outright rather than leaving it silently playing
     * into nothing. Checked every tick, deliberately NOT gated on
     * screen_on_now below (background, screen-off playback needs the same
     * protection). headphone_is_connected() is a cheap sysfs read (no
     * subprocess); the A2DP half reuses whatever poll_refresh_bt_icon()
     * last found rather than re-querying bluealsa-cli here every tick --
     * that's already throttled to its own ~5s cadence (see
     * refresh_bt_icon_thread_func()), and a few seconds of extra latency on
     * just the Bluetooth half is an acceptable tradeoff against forking a
     * process every single tick.
     *
     * Target-only: HOST_BUILD has no real jack/BT hardware to detect at
     * all (headphone_is_connected() is unconditionally false there, see
     * headphone_status.h), so this would otherwise fire a spurious "stop"
     * on the very first tick after starting playback in the simulator --
     * there's no real disconnect to protect against on host, since the dev
     * machine's own speakers are the actual output regardless of this
     * app's simulated jack/BT state. */
#ifndef HOST_BUILD
    /* Real-device bug report: "Bluetooth dropping connection randomly."
     * refresh_bt_icon_result_a2dp_connected comes from a subprocess-backed
     * poll (bluealsa-cli list-pcms, gated on bt_control_is_powered()) that
     * this file's own Bluetooth wedge-recovery logic (see
     * bt_control_recover_wedged_daemon()'s doc comment) already documents,
     * with a real-device strace to back it up, as prone to a single false
     * "not connected"/"No default controller" reading during a normal,
     * several-second-long bluetoothd/bluealsa busy window -- not a genuine
     * disconnect. That existing recovery logic requires several CONSECUTIVE
     * failures (TIMEOUT_RECOVERY_THRESHOLD) before concluding it's a real
     * wedge, specifically to filter out that blip. This auto-stop check
     * used to have no such filter at all -- a single bad poll cut playback
     * immediately, which is a far more likely explanation for "random"
     * drops than the radio itself actually losing the connection.
     *
     * Debounced on wall-clock time (not a "how many ticks in a row" counter
     * -- refresh_bt_icon_result_a2dp_connected only actually changes value
     * once every ~5s poll cycle, so counting 500ms update_timer_cb ticks
     * would just recount the same stale reading many times over) rather
     * than the raw signal, and only the Bluetooth half: headphone_is_
     * connected() is a direct, instant sysfs read with no such ambiguity,
     * so a wired jack pull still pauses playback immediately, unchanged --
     * this debounce only ever gated the Bluetooth reading going into the
     * same shared output_connected/audio_toggle_pause() check below, which
     * a wired pull reaches exactly the same way BT does. */
#define BT_OUTPUT_DISCONNECT_DEBOUNCE_MS 12000
    {
        static uint32_t bt_disconnected_since_tick = 0; /* 0 = currently connected (or never sampled) */
        if (refresh_bt_icon_result_a2dp_connected) {
            bt_disconnected_since_tick = 0;
        } else if (bt_disconnected_since_tick == 0) {
            bt_disconnected_since_tick = lv_tick_get();
        }
        /* Fast path: bt_control_output_disconnect_watch_start() (started
         * alongside audio_set_bt_output() -- see poll_refresh_bt_icon())
         * catches a real disconnect via bluealsa's own D-Bus signal in well
         * under a second, instead of waiting on this debounce's full 12s (on
         * top of refresh_bt_icon_result_a2dp_connected's own ~5s poll
         * cadence) -- see bluetooth_control.h's own comment on why the
         * debounce itself still has to stay, as the fallback for if this
         * monitor subprocess dies or bluealsa doesn't emit the signal.
         * Edge-triggered (consumed once), so this only forces the debounced
         * read false for the one tick right after a real removal -- harmless
         * if refresh_bt_icon_result_a2dp_connected is still stale-true a tick
         * later (nothing auto-resumes playback off output_connected going
         * back to true, so there's no user-visible flicker, just the stop
         * below firing sooner than the plain poll+debounce alone would have). */
        bool bt_connected_debounced = !bt_control_output_disconnect_consume() &&
            (refresh_bt_icon_result_a2dp_connected ||
             lv_tick_elaps(bt_disconnected_since_tick) < BT_OUTPUT_DISCONNECT_DEBOUNCE_MS);

        static bool last_output_connected = true; /* starts true so nothing fires before any real state has been sampled */
        bool output_connected = headphone_is_connected() || bt_connected_debounced;
        /* Real-device bug report: this used to call audio_stop() -- not
         * audio_toggle_pause() -- on a disconnect. audio_stop() (audio.c)
         * doesn't just silence output: its playback thread fully unwinds
         * the current track (decoder_close(), free()s the path, have_current
         * = false), the same as if the user had never opened it. That left
         * no way to resume once reconnected -- confirmed by a live test
         * report ("stops the music, instead of pause, so it can't be
         * resumed later"). audio_toggle_pause() keeps the decoder/position
         * intact and is what toggle_play_pause() itself already uses for an
         * ordinary pause tap, so resuming afterward (headphones reconnected,
         * tap play) picks up exactly where it left off. Only fires while
         * actually playing -- audio_is_paused() removed from the trigger
         * condition entirely, since toggling pause on an ALREADY-paused
         * track would incorrectly resume it into a dead/disconnected
         * output, the opposite of what this is for. */
        bool was_playing = audio_is_playing();
        if (last_output_connected && !output_connected && was_playing) {
            DBG_LOG("gui: output disconnected while playing -- pausing playback\n");
            audio_toggle_pause();
            set_play_button_state(false);
            show_error_toast("Paused: headphones disconnected");
        }
        last_output_connected = output_connected;
    }
#endif

    /* Car Mode (Settings toggle, off by default). Matches the stock
     * firmware's own real behavior (confirmed by real-device report):
     * unplugging power (car ignition off) checkpoints position and powers
     * the device off; plugging power back in (ignition on) powers it back
     * on and auto-resumes the same track/position (see gui_init()'s
     * resume-on-launch block, below).
     *
     * Reworked back to a full poweroff (idle_shutdown_now()) rather than
     * power_suspend_now() -- an earlier version of this used suspend-to-RAM
     * for a near-instant resume with no boot splash, but real-device
     * testing found two problems with that neither had a fix: suspend
     * never actually woke back up when power returned (this board's
     * PMIC/kernel wakeup-source support for VBUS insert, as opposed to a
     * power-button press, couldn't be confirmed -- see this function's own
     * prior git history for the sysfs probe that would be needed), and
     * suspending with an actively-connected Bluetooth output could reboot
     * the device outright. A full poweroff sidesteps both: /sbin/poweroff
     * and a cold power-on are both already proven-reliable on this
     * hardware (idle_shutdown_now() itself already uses poweroff as its
     * own default, for the same jzfb suspend/resume reason -- see
     * idle_shutdown.h), at the cost of a real boot splash + library check
     * on every resume instead of an instant one.
     *
     * Edge-triggered on battery_is_charging() so a normal desk charge with
     * Car Mode left on doesn't repeatedly retrigger, and gated on something
     * actually being loaded (audio_is_playing() || audio_is_paused()) --
     * no track loaded means nothing to "continue playing," so no reason to
     * force a shutdown cycle. Target-only, same reasoning as the auto-stop
     * block above -- battery_is_charging() is unconditionally false on
     * host. */
#ifndef HOST_BUILD
    {
        static bool last_charging = true; /* starts true so nothing fires before any real state has been sampled */
        bool charging = battery_is_charging();
        if (current_settings.car_mode_enabled && last_charging && !charging &&
            (audio_is_playing() || audio_is_paused())) {
            current_settings.last_position = audio_get_position_seconds();
            settings_save(&current_settings);
            idle_shutdown_now(); /* full poweroff -- does not return, see idle_shutdown.h */
        }
        last_charging = charging;
    }
#endif

    /* Auto screen-timeout. Physical button presses don't go through LVGL's
     * own indev system at all (see hw_buttons.h's own comment on why this
     * needs a raw evdev thread), so lv_display_get_inactive_time() alone
     * would only track touches -- feed it button activity too via
     * lv_display_trigger_activity(), or a session spent purely skipping
     * tracks would still auto-sleep the screen despite being actively used.
     * This only delays the NEXT auto-sleep, though -- it deliberately does
     * NOT wake an already-off screen (see the power-button block below for
     * why: touches don't either, real-device feedback was that a screen
     * meant to be off shouldn't relight itself from a pocket touch/bump,
     * only a deliberate power-button press should turn it back on). */
    if (played_paused || skipped_next || skipped_prev || volume_delta != 0) {
        lv_display_trigger_activity(NULL);
    }

    bool screen_was_on = backlight_screen_is_on();

    /* The ONLY way the screen turns back on, whether it went off from the
     * auto-timeout below or a previous manual power-button press -- deliberately
     * not touch, and not the hw button presses above either, even though both
     * feed the same LVGL inactivity clock: real-device feedback was that
     * touching a screen that's meant to be off (auto-timeout or manual) was
     * turning it back on, which isn't the expected "screen off means off
     * until you explicitly wake it" behavior. Applied here rather than
     * straight from the hw_buttons reader thread so the backlight toggle and
     * the LVGL inactivity clock (used below for the auto-timeout check)
     * update atomically on this thread -- toggling the backlight from the
     * other thread without also resetting the inactivity clock left it stuck
     * expired, so the auto-timeout check just below immediately re-fired and
     * turned the screen straight back off on the very next tick, a
     * press-to-wake that looked like a frozen black screen. */
    if (resumed_from_suspend_pending && lv_tick_elaps(resumed_from_suspend_tick) >= RESUME_POWER_DRAIN_WINDOW_MS) {
        DBG_LOG("resume: grace window expired unused at tick=%u\n", lv_tick_get());
        resumed_from_suspend_pending = false;
    }
    if (hw_buttons_consume_power()) {
        DBG_LOG("resume: hw_buttons_consume_power() true at tick=%u, pending=%d\n", lv_tick_get(), resumed_from_suspend_pending);
        if (resumed_from_suspend_pending) {
            /* Echo of the press that woke the device from suspend -- see
             * resumed_from_suspend_pending's own comment. Swallowed once
             * (not the whole window's worth of presses): only the wake
             * press itself should ever land here, so there's nothing more
             * to drain after the first one arrives, and continuing to
             * suppress every press for the rest of the window would make a
             * deliberate quick re-sleep tap right after waking do nothing. */
            resumed_from_suspend_pending = false;
        } else {
            lv_display_trigger_activity(NULL);
            backlight_set_screen_on(!backlight_screen_is_on());
        }
    }
    if (hw_buttons_consume_power_long_press()) {
        /* Same resumed_from_suspend_pending guard as the short-tap consumer
         * just above -- a long hold that woke the device from suspend
         * shouldn't also immediately pop up a power-off countdown the
         * instant the screen comes back on. power_off_countdown_active's
         * own guard (inside start_power_off_countdown() isn't needed here
         * since hw_buttons.c already only fires this once per physical
         * press) still lets a *second* long-press restart the countdown
         * from the top while one is already showing, which is fine. */
        if (resumed_from_suspend_pending) {
            resumed_from_suspend_pending = false;
        } else {
            /* Real-device bug report: the countdown popped up invisibly if
             * the screen was already asleep (from timeout or a previous
             * short tap) when the hold crossed the long-press threshold --
             * lv_display_trigger_activity() alone only resets the
             * inactivity clock, it doesn't touch the backlight. Explicit
             * backlight_set_screen_on(true) here (unconditional, same
             * idiom as resume_from_suspend_fixups()'s own wake-up) makes
             * screen_on_now below come out true, which is what already
             * drives lv_indev_enable(NULL, true) further down -- so this
             * also re-enables touch for the Cancel button, not just the
             * backlight. */
            backlight_set_screen_on(true);
            lv_display_trigger_activity(NULL);
            start_power_off_countdown();
        }
    }
    poll_power_off_countdown();

    uint32_t screen_inactive_ms = lv_display_get_inactive_time(NULL);
    if (current_settings.screen_timeout_enabled && backlight_screen_is_on() &&
        screen_inactive_ms >= (uint32_t) current_settings.screen_timeout_seconds * 1000) {
        backlight_set_screen_on(false);
    }

    /* Battery: skip every bit of UI-only refresh work below (label updates,
     * and especially the wpa_cli/bluetoothctl subprocess forks behind
     * refresh_wifi_icon()/refresh_bt_icon()) while nobody can see the
     * screen -- otherwise a music session left playing overnight with the
     * screen asleep still forks a bluetoothctl process every
     * WIFI_POLL_TICKS ticks for pixels nobody's looking at. Force one
     * refresh right on wake so nothing looks stale afterwards instead of
     * waiting up to WIFI_POLL_TICKS ticks to catch up. */
    bool screen_on_now = backlight_screen_is_on();
    bool screen_just_woke = screen_on_now && !screen_was_on;

    /* Touch input, not just the backlight, needs to follow screen on/off --
     * real-device feedback: a touch on a screen that's meant to be off was
     * both relighting it (fixed above, by no longer waking on plain
     * activity) AND, independent of that, still being delivered to whatever
     * was underneath in the dark (a blind touch could still press a button
     * or navigate a screen the user can't see). Disabling every indev while
     * off blocks both: LVGL never processes the touch at all, so there's
     * nothing left to wake the screen OR act on. Physical hardware buttons
     * are unaffected -- hw_buttons.c reads them on its own raw evdev thread
     * outside LVGL's indev system entirely, so play/pause/skip/volume/power
     * keep working with the screen dark, matching a normal DAP. */
    if (screen_on_now != screen_was_on) {
        lv_indev_enable(NULL, screen_on_now);
    }

    if (screen_just_woke) {
        idle_shutdown_attempted = false;
        if (radios_suspended) {
            /* Only resume a radio we actually suspended, and only if it's
             * still off -- reuses the same toggle-thread infrastructure as
             * the quick-drawer tap-to-toggle (wifi_toggle_thread_func()/
             * bt_toggle_thread_func() just flip whatever the current state
             * is), guarded the same way against a toggle already in
             * flight. */
            if (wifi_was_on_before_suspend && !wifi_control_is_enabled() && !wifi_toggle_active) {
                wifi_toggle_active = true;
                wifi_toggle_done_flag = false;
                pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL);
            }
            if (bt_was_on_before_suspend && !bt_is_powered_cached && !bt_toggle_active) {
                bt_toggle_active = true;
                bt_toggle_done_flag = false;
                pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, NULL);
            }
            radios_suspended = false;
        }
    } else if (!screen_on_now) {
        if (screen_was_on) {
            /* Just went to sleep this tick -- the 10-minute countdown starts
             * from here, not from whenever the inactivity that caused it
             * began. */
            screen_off_since_tick = lv_tick_get();
        }
        /* bt_is_powered_cached, not bt_control_is_powered(), deliberately --
         * the latter forks a process, and this condition is checked every
         * tick while the screen is off, not throttled like refresh_bt_icon()
         * is. The cached value is frozen at whatever it was when the screen
         * went dark, which is fine: nothing in this app can change BT power
         * state without a UI the user can't reach with the screen off. */
        if (!radios_suspended && !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            !audio_is_playing() && !battery_is_charging() &&
            lv_tick_elaps(screen_off_since_tick) >= RADIO_SUSPEND_DELAY_MS) {
            wifi_was_on_before_suspend = wifi_control_is_enabled();
            bt_was_on_before_suspend = bt_is_powered_cached;
            if (wifi_was_on_before_suspend && !wifi_toggle_active) {
                wifi_toggle_active = true;
                wifi_toggle_done_flag = false;
                pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL);
            }
            if (bt_was_on_before_suspend && !bt_toggle_active) {
                bt_toggle_active = true;
                bt_toggle_done_flag = false;
                pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, NULL);
            }
            radios_suspended = true;
        }

        /* Deliberately independent of radios_suspended -- idle_shutdown_minutes
         * is a separate, normally-longer setting than RADIO_SUSPEND_DELAY_MS,
         * and shutdown should still happen on its own schedule even if radio
         * suspend is impossible right now (e.g. wifi_dac_mode_enabled) --
         * except it explicitly shares the same DAC-mode/playing/charging
         * gate, since none of those should ever be interrupted by the
         * device powering itself off out from under them. */
        if (!idle_shutdown_attempted && current_settings.idle_shutdown_enabled &&
            !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            !audio_is_playing() && !battery_is_charging() &&
            lv_tick_elaps(screen_off_since_tick) >= (uint32_t) current_settings.idle_shutdown_minutes * 60 * 1000) {
            if (current_settings.idle_suspend_enabled) {
                power_suspend_now();

                /* Real-device bug report: after resuming from suspend, the
                 * backlight itself came back on (the kernel's own fbdev/
                 * backlight power-notifier chain does that automatically in
                 * response to power_suspend_now()'s raw `/sys/class/
                 * graphics/fb0/blank` write) but the UI itself was never
                 * visible again -- confirmed root cause: that raw sysfs
                 * write completely bypasses this app's OWN screen-on state
                 * (backlight.c's screen_on static, only ever flipped by
                 * backlight_set_screen_on()) and this function's own
                 * indev-enable logic just above, which only reacts to a
                 * screen_on_now/screen_was_on transition it detects BETWEEN
                 * ticks -- but both are read from the exact same
                 * backlight_screen_is_on() call, so flipping that state
                 * mid-tick here (rather than waiting for a real transition
                 * to be observed across two separate ticks) would never be
                 * seen as an "edge" and that logic would never fire again.
                 *
                 * Also folds in the fix for a related real-device report --
                 * waking from suspend needed two power-button presses, the
                 * first visibly flashing the backlight on then straight back
                 * off. Root cause: lv_display_get_inactive_time(NULL), read
                 * by the auto-screen-timeout check just below, is fed only
                 * by lv_display_trigger_activity() calls, none of which
                 * happen anywhere during the entire suspend duration (the
                 * whole app, including this timer, is frozen) -- so the
                 * instant the screen is force-enabled, that check sees an
                 * inactivity duration spanning the ENTIRE sleep, always over
                 * screen_timeout_seconds, and immediately flips the screen
                 * back off again on this same tick. The second press only
                 * "fixed" it as a side effect (a genuine press resets that
                 * clock in the toggle branch below) -- resetting it
                 * explicitly here is the real fix.
                 *
                 * See resume_from_suspend_fixups()'s own comment for why
                 * this is shared with Car Mode's own suspend call below. */
                resume_from_suspend_fixups();

                /* Unlike idle_shutdown_now() (which never returns --
                 * poweroff ends the process), this call CAN legitimately
                 * return without a real user wake (e.g. a spurious IRQ) --
                 * screen_just_woke won't fire to reset this flag in that
                 * case, so reset it here instead. Worst case if the device
                 * really did just wake for a moment is one immediate
                 * re-suspend on the next tick, not getting stuck awake
                 * indefinitely after a single spurious wake. */
                idle_shutdown_attempted = false;
            } else {
                idle_shutdown_attempted = true;
                idle_shutdown_now();
            }
        }
    }

    if (screen_on_now) {
        refresh_clock_label();
        refresh_battery_topbar();
        refresh_headphone_icon();
        poll_usb_audio_output(); /* same cheap direct-file-read class as refresh_headphone_icon() above, safe every tick */
        refresh_play_pause_topbar(); /* cheap in-process state check, safe to call every tick unlike the BT/wifi subprocess calls below */
        if (screen_just_woke || ++wifi_poll_tick_counter >= WIFI_POLL_TICKS) {
            wifi_poll_tick_counter = 0;
            refresh_wifi_icon();
            start_refresh_bt_icon(); /* also forks a process (bluetoothctl show), same low cadence as wifi */
        }
    }

    /* Also unconditional on screen state -- charging happens with the
     * screen off far more often than on, so gating this the same way as the
     * topbar refreshes above would leave it capped at whatever percent the
     * screen happened to be on last. charge_limiter_poll() throttles its
     * own actual sysfs work internally, so calling it every tick here is
     * cheap. */
    charge_limiter_poll(current_settings.charge_limiter_enabled, false);
    led_control_poll(current_settings.led_indicator_enabled);

    if (current_settings.remote_control_enabled) {
        /* No separate now-playing metadata cache exists in this app beyond
         * what's already on screen -- song_title_label/song_folder_label
         * are this app's own single source of truth for title/artist (see
         * apply_track_metadata_to_ui()), so read them back rather than
         * standing up a second copy of the same state just for this.
         * Album isn't tracked anywhere after the initial metadata_read()
         * call, so it's left blank here -- a real gap, not an oversight,
         * see remote_control.h's own Phase 1 scope note. */
        const char * now_playing_path =
            (playlist_count > 0 && playlist_index >= 0 && playlist_index < playlist_count) ? playlist[playlist_index]
                                                                                             : NULL;
        remote_control_notify_status(audio_is_playing(), audio_is_paused(), lv_label_get_text(song_title_label),
                                      lv_label_get_text(song_folder_label), "", now_playing_path,
                                      (int) audio_get_position_seconds(), (int) audio_get_duration_seconds(),
                                      audio_get_volume(), current_settings.play_mode);
    }

    /* Polling for in-flight async operations (wifi/bt connect, library scan,
     * subsonic sync, ...) keeps running regardless of screen state -- these
     * only do real work when a request the user already made is still
     * pending, so gating them on screen state would leave that operation
     * stuck until the user wakes the screen back up. */
    poll_subsonic_download();
    poll_subsonic_library_download();
    poll_dlna_control();
    poll_subsonic_connect();
    poll_wifi_scan();
    poll_wifi_connect();
    poll_wifi_connect_saved();
    poll_wifi_disconnect();
    poll_wifi_forget();
    poll_wifi_toggle();
    poll_bt_toggle();
    poll_refresh_bt_icon();
    poll_bt_dac_startup_reapply();
    poll_bt_apply_output_settings();
    poll_bt_scan();
    poll_bt_connect();
    poll_bt_forget();
    poll_library_rescan();
    poll_sd_format();
    poll_import_web_stop();
    poll_sleep_timer();
    poll_usb_mode_switch();
    poll_sd_card_hotplug();
    poll_cover_decode();

    if (audio_consume_track_advanced()) {
        /* The playback thread already moved on to the queued next track by
         * itself (gapless handoff or a completed crossfade) -- just sync
         * the GUI's own index/labels to match, don't restart audio. The
         * target must match exactly what arm_next_track_for_audio() armed
         * for this same `playlist_index`, so commit_auto_advance() (which
         * only actually mutates shuffle state) is called right after. */
        int advanced_index = compute_auto_advance_index(playlist_index);
        if (advanced_index >= 0) {
            commit_auto_advance();
            on_track_auto_advanced(advanced_index);
        }
    }

    if (audio_consume_track_finished()) {
        /* A true end-of-playlist, OR the queued next track failed to open
         * (e.g. a corrupt file) -- fall back to the old hard-restart-based
         * skip so a single bad file in a playlist doesn't stall it. Only
         * Sequential mode can actually reach "nothing next" here (every
         * other mode always has somewhere to go -- see
         * compute_auto_advance_index()). */
        int finished_index = compute_auto_advance_index(playlist_index);
        if (finished_index >= 0) {
            commit_auto_advance();
            play_track_at(finished_index);
        } else {
            set_play_button_state(false);
        }
    }

    if (playlist_index < 0 || user_seeking) return;

    double position = audio_get_position_seconds();
    double duration = audio_get_duration_seconds();

    if (duration > 0) {
        int32_t percent = (int32_t) ((position / duration) * 100.0);
        lv_slider_set_value(progress_slider, percent, LV_ANIM_OFF);
    }

    char pos_str[16];
    char dur_str[16];
    format_time(position, pos_str, sizeof(pos_str));
    format_time(duration, dur_str, sizeof(dur_str));
    lv_label_set_text(pos_label, pos_str);
    lv_label_set_text(dur_label, dur_str);

    refresh_format_badge();
}

typedef struct {
    lv_obj_t * img;
    const char * normal_path;
    const char * pressed_path;
} transport_btn_ctx_t;

static void transport_btn_press_event_cb(lv_event_t * e) {
    transport_btn_ctx_t * ctx = (transport_btn_ctx_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_image_set_src(ctx->img, asset_path(ctx->pressed_path));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_image_set_src(ctx->img, asset_path(ctx->normal_path));
    }
}

/* ---- Player screen "more" menu: Add to Playlist / EQ / Delete -----------
 * Same hand-built top-layer overlay shape as every other popup in this file
 * (bt_action_popup, eq_reset_popup, firmware_update_popup -- this codebase
 * doesn't use LVGL's lv_msgbox anywhere). ---- */

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"

static lv_obj_t * add_to_playlist_screen;
static lv_obj_t * add_to_playlist_list;

/* Which song this screen adds to whatever playlist is picked -- set by
 * open_add_to_playlist_for() right before nav_push(), not always the
 * currently-playing track anymore (see that function's own comment: the
 * song long-press context menu reaches this same screen for an arbitrary
 * row, not just the player's own "..." menu). */
static char add_to_playlist_target_path[512] = "";

static void new_playlist_name_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (text[0] == '\0' || add_to_playlist_target_path[0] == '\0') return;

    char created_path[512];
    bool ok = playlist_files_create(PLAYLISTS_DIR, text, add_to_playlist_target_path, created_path, sizeof(created_path));
    if (ok) metadata_db_playlist_insert_one(created_path);
    show_error_toast(ok ? "Playlist created" : "Failed to create playlist");
    nav_pop(); /* leave the Add to Playlist picker too, back to the player */
}

static void new_playlist_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Playlist Name", "", false, false, new_playlist_name_done_cb, NULL);
}

static void existing_playlist_row_cb(lv_event_t * e) {
    const char * path = (const char *) lv_event_get_user_data(e);
    if (add_to_playlist_target_path[0] == '\0') return;

    /* Playlist design change: adding a song already in the target playlist
     * used to just append a second copy with no feedback -- confirmed by
     * playlist_files_append()'s own unconditional fprintf(). Checked here
     * rather than inside playlist_files_append() itself so that function
     * stays a plain, unconditional "add this line" primitive other callers
     * (e.g. new_playlist_name_done_cb() below, adding a brand-new file's
     * first song) don't pay an unnecessary duplicate scan for. */
    if (playlist_files_contains(path, add_to_playlist_target_path)) {
        show_error_toast("Song already added");
        nav_pop();
        return;
    }

    bool ok = playlist_files_append(path, add_to_playlist_target_path);
    show_error_toast(ok ? "Added to playlist" : "Failed to add to playlist");
    nav_pop();
}

static void populate_add_to_playlist_screen(void) {
    lv_obj_clean(add_to_playlist_list);

    lv_obj_t * new_row = lv_obj_create(add_to_playlist_list);
    lv_obj_set_size(new_row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
    lv_obj_set_style_radius(new_row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(new_row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(new_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(new_row, 0, 0);
    lv_obj_remove_flag(new_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(new_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(new_row, new_playlist_row_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * new_label = lv_label_create(new_row);
    lv_label_set_text(new_label, "+ New Playlist");
    lv_obj_set_style_text_color(new_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(new_label, &LIST_ROW_FONT, 0);
    lv_obj_align(new_label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

    char ** paths;
    int count;
    /* Persistent cache read, not a live scan -- same reasoning as
     * populate_playlists_screen()'s own comment; this screen is reachable
     * from the player's "more" menu, so it was paying the same ~5s SD-card
     * walk on every open too. */
    metadata_db_load_all_playlists(&paths, &count);
    if (count == 0) return;

    for (int i = 0; i < count; i++) {
        lv_obj_t * row = lv_obj_create(add_to_playlist_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, existing_playlist_row_cb, LV_EVENT_CLICKED, paths[i]); /* not freed here -- same lives-for-the-screen's-lifetime tradeoff as eq_profile_row_cb's own paths[i] */

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, basename_of(paths[i]));
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    }
    free(paths);
}

static lv_obj_t * build_add_to_playlist_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen("Add to Playlist", &title_label, &add_to_playlist_list);
}

/* Shared entry point into the Add to Playlist picker -- the player's own
 * "..." menu (more_menu_add_to_playlist_cb() below) and the song long-press
 * context menu (song_context_menu_add_to_playlist_cb(), defined with the
 * rest of that popup further down) both reach this screen for whatever
 * song path they have, not necessarily the currently-playing one. */
static void open_add_to_playlist_for(const char * path) {
    snprintf(add_to_playlist_target_path, sizeof(add_to_playlist_target_path), "%s", path);
    populate_add_to_playlist_screen();
    nav_push(add_to_playlist_screen);
}

/* ---- Queue ("Up Next") screen -- reachable from the player's "..." menu
 * and as a special row in Playlists, same treatment as Favorites/Most
 * Played there. Shows whatever queued_pending_count currently tracks
 * (playlist[playlist_index+1 .. +queued_pending_count]), in play order.
 * Tapping a row jumps straight to that song -- play_track_at() naturally
 * treats it as a manual pick, no special-casing needed since these are
 * already just ordinary playlist[] positions (see queued_pending_count's
 * own comment on why the queue is implemented as a splice, not a separate
 * list). Rebuilt fresh on every open (queue sizes are always small, no
 * virtualization needed, same as the Group Songs screen). ---- */
static lv_obj_t * queue_screen;
static lv_obj_t * queue_list;

static void queue_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int offset = (int) (intptr_t) lv_event_get_user_data(e); /* 0-based position within the queued run */
    if (playlist_index < 0) return;
    int target = playlist_index + 1 + offset;
    if (target < playlist_count) play_track_at(target);
}

static void populate_queue_screen(void) {
    lv_obj_clean(queue_list);

    if (playlist_index < 0 || queued_pending_count <= 0) {
        lv_obj_t * label = lv_label_create(queue_list);
        lv_label_set_text(label, "Queue is empty");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }

    for (int i = 0; i < queued_pending_count; i++) {
        int idx = playlist_index + 1 + i;
        if (idx >= playlist_count) break;

        lv_obj_t * row = lv_label_create(queue_list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char title[128], folder[128];
        get_display_names(playlist[idx], title, sizeof(title), folder, sizeof(folder));
        lv_label_set_text(row, title);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, queue_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static lv_obj_t * build_queue_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen("Queue", &title_label, &queue_list);
}

static void open_queue_screen(void) {
    populate_queue_screen();
    nav_push(queue_screen);
}

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
#define PLUGIN_LIST_SCREEN_POOL_SIZE 4
static lv_obj_t * plugin_list_screens[PLUGIN_LIST_SCREEN_POOL_SIZE];
static lv_obj_t * plugin_list_title_labels[PLUGIN_LIST_SCREEN_POOL_SIZE];
static lv_obj_t * plugin_list_lists[PLUGIN_LIST_SCREEN_POOL_SIZE];
static int plugin_list_pool_next = 0;

static void plugin_list_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_list_item_selected(index);
}

void gui_plugin_show_list(const char * title, const char * const * labels, const char * const * icon_paths,
                           const char * const * text_sizes, int32_t height, int count) {
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
    bool use_container_rows = any_icon || height > 0;

    int32_t row_h = LIST_ROW_HEIGHT;
    if (height > 0) {
        row_h = height;
        if (row_h < PILL_ROW_HEIGHT_MIN) row_h = PILL_ROW_HEIGHT_MIN;
        if (row_h > PILL_ROW_HEIGHT_MAX) row_h = PILL_ROW_HEIGHT_MAX;
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
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, plugin_list_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
            continue;
        }

        lv_obj_t * row = lv_obj_create(list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, row_h);
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

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, plugin_list_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }

    nav_push(plugin_list_screens[slot]);
}

void gui_plugin_play_paths(const char * const * paths, int count, int start_index) {
    if (count <= 0) return;
    if (start_index < 0) start_index = 0;
    if (start_index >= count) start_index = count - 1;

    char ** new_playlist = malloc(sizeof(char *) * count);
    for (int i = 0; i < count; i++) new_playlist[i] = strdup(paths[i]);
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
    int next_index = compute_manual_step_index(playlist_index, 1);
    if (next_index >= 0) play_track_at(next_index);
}

void gui_plugin_prev_track(void) {
    int prev_index = compute_manual_step_index(playlist_index, -1);
    if (prev_index >= 0) play_track_at(prev_index);
}

void gui_plugin_seek(double seconds) {
    audio_seek(seconds);
}

void gui_plugin_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    lv_slider_set_value(volume_slider, percent, LV_ANIM_OFF);
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
static void plugin_text_entry_done_cb(const char * text, void * user_data) {
    (void) user_data;
    plugin_manager_text_input_submitted(text);
}

void gui_plugin_show_text_input(const char * title, const char * initial_text, bool is_password) {
    show_text_entry(title, initial_text, is_password, false, plugin_text_entry_done_cb, NULL);
}

/* ---- Delete confirmation popup ---- */
static lv_obj_t * delete_song_popup;
static lv_obj_t * delete_song_popup_backdrop;
static lv_obj_t * delete_song_popup_title;

static void hide_delete_song_popup(void) {
    lv_obj_add_flag(delete_song_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(delete_song_popup, LV_OBJ_FLAG_HIDDEN);
}

static void delete_song_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
}

static void delete_song_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
}

static void delete_song_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    char * to_delete = strdup(playlist[playlist_index]);
    int del_index = playlist_index;

    /* What to play next is decided BEFORE touching the file or the array --
     * whatever ends up sitting at del_index once the deleted entry is
     * removed (i.e. what used to be right after it), or the new last track
     * if the deleted one was last. Doesn't try to honor Shuffle/Repeat here
     * -- a delete is a one-off structural change to the queue, not a
     * "what's next" playback decision. */
    free(playlist[del_index]);
    for (int i = del_index; i < playlist_count - 1; i++) playlist[i] = playlist[i + 1];
    playlist_count--;

    if (playlist_count == 0) {
        audio_stop();
        plugin_manager_notify_stopped();
        free(playlist);
        playlist = NULL;
        playlist_index = -1;
        clear_player_source();
        set_play_button_state(false);
        lv_label_set_text(song_title_label, "No track loaded");
        nav_pop(); /* nothing left to show on the player screen */
    } else {
        int new_index = (del_index < playlist_count) ? del_index : playlist_count - 1;
        play_track_at(new_index);
    }

    unlink(to_delete); /* only after playback has moved off it */
    free(to_delete);
    show_error_toast("Song deleted");
}

static void build_delete_song_popup(void) {
    lv_obj_t * top = lv_layer_top();

    delete_song_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(delete_song_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(delete_song_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(delete_song_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(delete_song_popup_backdrop, 0, 0);
    lv_obj_remove_flag(delete_song_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(delete_song_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(delete_song_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(delete_song_popup_backdrop, delete_song_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    delete_song_popup = lv_obj_create(top);
    lv_obj_set_size(delete_song_popup, 420, 220);
    lv_obj_align(delete_song_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(delete_song_popup, 16, 0);
    lv_obj_add_style(delete_song_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(delete_song_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(delete_song_popup, 0, 0);
    lv_obj_remove_flag(delete_song_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(delete_song_popup, LV_OBJ_FLAG_HIDDEN);

    delete_song_popup_title = lv_label_create(delete_song_popup);
    lv_obj_set_width(delete_song_popup_title, lv_pct(90));
    lv_label_set_long_mode(delete_song_popup_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(delete_song_popup_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(delete_song_popup_title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(delete_song_popup_title, ui_size_22, 0);
    lv_obj_align(delete_song_popup_title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * delete_row = lv_obj_create(delete_song_popup);
    lv_obj_set_size(delete_row, lv_pct(90), 56);
    lv_obj_align(delete_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(delete_row, 12, 0);
    lv_obj_set_style_bg_opa(delete_row, 0, 0);
    lv_obj_set_style_border_width(delete_row, 0, 0);
    lv_obj_remove_flag(delete_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(delete_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(delete_row, delete_song_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * delete_label = lv_label_create(delete_row);
    lv_label_set_text(delete_label, "Delete");
    lv_obj_set_style_text_color(delete_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(delete_label, ui_size_20, 0);
    lv_obj_center(delete_label);

    lv_obj_t * cancel_row = lv_obj_create(delete_song_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, delete_song_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, ui_size_20, 0);
    lv_obj_center(cancel_label);
}

/* ---- The "more" 3-row menu itself ---- */
static lv_obj_t * more_menu_popup;
static lv_obj_t * more_menu_popup_backdrop;

static void hide_more_menu_popup(void) {
    lv_obj_add_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

static void more_menu_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
}

/* Defined further down, right after populate_group_songs_rows() -- needs
 * group_songs_screen/list/indices/count/title_label and
 * compact_list_scroll_to_index()/file_browser_navigate_to() all already
 * in scope, none of which are declared yet this early in the file.
 * Forward-declared here so build_more_menu_popup()'s rows table (right
 * below) can wire it up as a row's click handler. */
static void more_menu_list_cb(lv_event_t * e);

static void more_menu_add_to_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (playlist_index < 0) return;
    open_add_to_playlist_for(playlist[playlist_index]);
}

static void more_menu_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    open_queue_screen();
}

static void more_menu_eq_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    nav_push(eq_screen);
}

static void more_menu_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    lv_label_set_text_fmt(delete_song_popup_title, "Delete %s?\nThis cannot be undone.", basename_of(playlist[playlist_index]));
    lv_obj_remove_flag(delete_song_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(delete_song_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(delete_song_popup_backdrop);
    lv_obj_move_foreground(delete_song_popup);
}

static void more_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0) return;

    lv_obj_remove_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(more_menu_popup_backdrop);
    lv_obj_move_foreground(more_menu_popup);
}

static void build_more_menu_popup(void) {
    lv_obj_t * top = lv_layer_top();

    more_menu_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(more_menu_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(more_menu_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(more_menu_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(more_menu_popup_backdrop, 0, 0);
    lv_obj_remove_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(more_menu_popup_backdrop, more_menu_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    more_menu_popup = lv_obj_create(top);
    lv_obj_set_size(more_menu_popup, 400, 376); /* +66 over the old 310 -- one more row, same per-row spacing */
    lv_obj_align(more_menu_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(more_menu_popup, 16, 0);
    lv_obj_add_style(more_menu_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(more_menu_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(more_menu_popup, 0, 0);
    lv_obj_remove_flag(more_menu_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);

    static const struct {
        const char * label;
        lv_event_cb_t cb;
        bool destructive;
    } rows[] = {
        { "List", more_menu_list_cb, false },
        { "Queue", more_menu_queue_cb, false },
        { "Add to Playlist", more_menu_add_to_playlist_cb, false },
        { "EQ", more_menu_eq_cb, false },
        { "Delete", more_menu_delete_cb, true },
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t * row = lv_obj_create(more_menu_popup);
        lv_obj_set_size(row, lv_pct(90), 56);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 20 + (int) i * 66);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, rows[i].cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, rows[i].label);
        lv_obj_set_style_text_color(label, rows[i].destructive ? lv_color_make(255, 120, 120) : accent_lv_color(), 0);
        lv_obj_set_style_text_font(label, ui_size_20, 0);
        lv_obj_center(label);
    }
}

/* ---- Song long-press context menu: Add to Queue / Add to Playlist / Cancel
 * Same hand-built popup shape as build_more_menu_popup() just above, for a
 * long-pressed row in any song list (All Songs, an Artist's/Album's songs,
 * a Playlist, Favorites, Most Played) rather than only the player screen's
 * own currently-playing track. ---- */

static lv_obj_t * song_context_menu_popup;
static lv_obj_t * song_context_menu_popup_backdrop;
/* Set right before showing the popup (open_song_context_menu()) -- which
 * song "Add to Queue"/"Add to Playlist" act on, since a long-pressed row
 * isn't necessarily the currently-playing track. */
static char song_context_menu_target_path[512] = "";

static void hide_song_context_menu_popup(void) {
    lv_obj_add_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

static void song_context_menu_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
}

static void song_context_menu_add_to_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] != '\0') queue_add_song(song_context_menu_target_path);
}

static void song_context_menu_add_to_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] != '\0') open_add_to_playlist_for(song_context_menu_target_path);
}

static void song_context_menu_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
}

/* Called from every song list's long-press handler (all_songs_row_long_
 * press_cb(), group_song_row_long_press_cb()) with that row's actual song
 * path. */
static void open_song_context_menu(const char * path) {
    snprintf(song_context_menu_target_path, sizeof(song_context_menu_target_path), "%s", path);
    lv_obj_remove_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(song_context_menu_popup_backdrop);
    lv_obj_move_foreground(song_context_menu_popup);
}

static void build_song_context_menu_popup(void) {
    lv_obj_t * top = lv_layer_top();

    song_context_menu_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(song_context_menu_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(song_context_menu_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(song_context_menu_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(song_context_menu_popup_backdrop, 0, 0);
    lv_obj_remove_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(song_context_menu_popup_backdrop, song_context_menu_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    song_context_menu_popup = lv_obj_create(top);
    lv_obj_set_size(song_context_menu_popup, 400, 244);
    lv_obj_align(song_context_menu_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(song_context_menu_popup, 16, 0);
    lv_obj_add_style(song_context_menu_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(song_context_menu_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(song_context_menu_popup, 0, 0);
    lv_obj_remove_flag(song_context_menu_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);

    static const struct {
        const char * label;
        lv_event_cb_t cb;
        bool destructive;
    } rows[] = {
        { "Add to Queue", song_context_menu_add_to_queue_cb, false },
        { "Add to Playlist", song_context_menu_add_to_playlist_cb, false },
        { "Cancel", song_context_menu_cancel_cb, false },
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t * row = lv_obj_create(song_context_menu_popup);
        lv_obj_set_size(row, lv_pct(90), 56);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 20 + (int) i * 66);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, rows[i].cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, rows[i].label);
        lv_obj_set_style_text_color(label, rows[i].destructive ? lv_color_make(255, 120, 120) : accent_lv_color(), 0);
        lv_obj_set_style_text_font(label, ui_size_20, 0);
        lv_obj_center(label);
    }
}

static lv_obj_t * build_player_screen(uint32_t screen_width, uint32_t screen_height) {
    (void) screen_width;
    (void) screen_height;
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_make(0, 0, 0), 0);

    /* Full-bleed album art (real per-track art is Task #16 -- this is the
     * firmware's own default cover placeholder, 480x480, top-aligned) plus
     * a matching 480x320 gradient panel that exactly fills the remaining
     * screen height below it (480 + 320 = 800), giving the seamless
     * art-fades-to-dark backdrop from the reference photo without needing
     * any distortion/stretching of the square art. */
    cover_img = lv_image_create(scr);
    lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
    lv_obj_align(cover_img, LV_ALIGN_TOP_MID, 0, 0);

    player_overlay_panel = lv_obj_create(scr);
    lv_obj_t * overlay = player_overlay_panel; /* short local alias, rest of this function was written against this name */
    lv_obj_set_size(overlay, 480, 320);
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_image_src(overlay, asset_path("playing_plane/buttom.png"), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(overlay, 16, 0);
    /* Extra bottom padding, on top of pad_all's 16 -- SPACE_BETWEEN below
     * packs controls_row (the transport row: prev/play/next) flush against
     * this panel's own bottom padding edge, which otherwise put it directly
     * under the home indicator bar (see build_home_indicator_bar()) and its
     * swipe-up hit zone, confirmed overlapping on a real screenshot. Only
     * the bottom side changes -- top/left/right stay at the plain 16 set
     * above. */
    lv_obj_set_style_pad_bottom(overlay, 16 + HOME_INDICATOR_BAND_HEIGHT, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(overlay, 6, 0);
    /* Without an explicit main-axis alignment, flex defaults to packing
     * children at the top, leaving the rest of this 320px panel empty below
     * the transport row -- SPACE_BETWEEN spreads title/artist/progress/time/
     * controls out to fill the whole panel instead, controls_row landing at
     * the very bottom edge. */
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Dismiss affordance over the album art, top-left -- same left-pointing
     * back arrow as every other screen's back button, for a consistent
     * back-button convention across the app. */
    /* Hitbox is deliberately larger than the visual icon (64x64 vs the
     * icon's native size) -- real-hardware testing showed taps aimed at this
     * corner landing a handful of pixels below a tight 44x44 box (finger
     * imprecision on a small corner target), so the touch area is padded out
     * generously while the icon itself stays centered at its normal size. */
    lv_obj_t * dismiss_btn = lv_obj_create(scr);
    lv_obj_set_size(dismiss_btn, 64, 64);
    lv_obj_align(dismiss_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(dismiss_btn, 0, 0);
    lv_obj_set_style_border_width(dismiss_btn, 0, 0);
    lv_obj_remove_flag(dismiss_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dismiss_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dismiss_btn, library_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * dismiss_arrow = lv_image_create(dismiss_btn);
    lv_image_set_src(dismiss_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(dismiss_arrow);

    /* Title row: song title (left) + favorite icon (right) -- matches the
     * reference layout, where the 3-dot "more" menu lives in the transport
     * row below instead (repeat/prev/play/next/more), not up here. */
    lv_obj_t * title_row = lv_obj_create(overlay);
    lv_obj_set_size(title_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_row, 0, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_remove_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    song_title_label = lv_label_create(title_row);
    lv_label_set_text(song_title_label, "No track loaded");
    lv_obj_add_style(song_title_label, &style_theme_text_primary, 0);
    /* Explicit rather than relying on LV_FONT_DEFAULT -- see fallback_font.h,
     * this is one of the handful of labels that needs the non-Latin
     * fallback but was never otherwise styled. */
    lv_obj_set_style_text_font(song_title_label, &app_font_16, 0);

    favorite_icon = lv_image_create(title_row);
    lv_image_set_src(favorite_icon, asset_path("playing_plane/collect_out.png"));
    lv_obj_add_flag(favorite_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(favorite_icon, favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);

    /* Artist row: artist (left) + format/quality badge (right). */
    lv_obj_t * artist_row = lv_obj_create(overlay);
    lv_obj_set_size(artist_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(artist_row, 0, 0);
    lv_obj_set_style_border_width(artist_row, 0, 0);
    lv_obj_set_style_pad_all(artist_row, 0, 0);
    lv_obj_remove_flag(artist_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(artist_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(artist_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    song_folder_label = lv_label_create(artist_row);
    lv_label_set_text(song_folder_label, "");
    lv_obj_add_style(song_folder_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(song_folder_label, &app_font_16, 0); /* see song_title_label's own comment above */

    format_badge_label = lv_label_create(artist_row);
    lv_label_set_text(format_badge_label, "");
    lv_obj_add_style(format_badge_label, &style_theme_text_muted, 0);

    /* Real seek bar: progress_bg/progress/cursor are all sized to their own
     * fixed 440x12 (track) / 30x30 (knob) pixel art, so the slider is given
     * those exact dimensions rather than a percentage width -- stretching
     * would blur/tile the asset. */
    progress_slider = lv_slider_create(overlay);
    lv_obj_set_size(progress_slider, 440, 12);
    lv_obj_align(progress_slider, LV_ALIGN_TOP_MID, 0, 0);
    lv_slider_set_range(progress_slider, 0, 100);
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_image_src(progress_slider, asset_path("playing_plane/progress_bg.png"), LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(progress_slider, asset_path("playing_plane/progress.png"), LV_PART_INDICATOR);
    lv_obj_set_style_bg_image_src(progress_slider, asset_path("playing_plane/cursor.png"), LV_PART_KNOB);
    /* Real-device bug report: accent color didn't apply here -- see
     * apply_accent_color()'s own comment on why an image-art slider needs
     * bg_image_recolor, not just bg_color. */
    lv_obj_add_style(progress_slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(progress_slider, &style_accent, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(progress_slider, 30, LV_PART_KNOB);
    lv_obj_set_style_height(progress_slider, 30, LV_PART_KNOB);
    lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t * time_row = lv_obj_create(overlay);
    lv_obj_set_size(time_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_row, 0, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    pos_label = lv_label_create(time_row);
    lv_label_set_text(pos_label, "0:00");
    lv_obj_add_style(pos_label, &style_theme_text_muted, 0);

    dur_label = lv_label_create(time_row);
    lv_label_set_text(dur_label, "0:00");
    lv_obj_add_style(dur_label, &style_theme_text_muted, 0);

    /* "N/M" position within the current queue -- centered, between the
     * progress bar and the transport row below it. */
    song_count_label = lv_label_create(overlay);
    lv_label_set_text(song_count_label, "");
    lv_obj_add_style(song_count_label, &style_theme_text_muted, 0);

    /* Transport row: prev / play-pause / next, centered. */
    lv_obj_t * controls_row = lv_obj_create(overlay);
    lv_obj_set_size(controls_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(controls_row, 0, 0);
    lv_obj_set_style_border_width(controls_row, 0, 0);
    lv_obj_set_style_pad_all(controls_row, 0, 0);
    lv_obj_set_style_pad_top(controls_row, 10, 0);
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(controls_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(controls_row, 36, 0);

    /* Play-mode icon (sequential/repeat/shuffle) -- leftmost, matching the
     * reference layout (repeat / prev / play / next / more). Tapping cycles
     * Sequential -> Repeat All -> Repeat One -> Shuffle (order_icon_event_cb). */
    order_icon = lv_image_create(controls_row);
    lv_image_set_src(order_icon, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));
    lv_obj_add_flag(order_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(order_icon, 16);
    lv_obj_add_event_cb(order_icon, order_icon_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * prev_btn = lv_image_create(controls_row);
    lv_image_set_src(prev_btn, asset_path("playing_plane/btn_prev.png"));
    lv_obj_add_flag(prev_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);
    transport_btn_ctx_t * prev_ctx = malloc(sizeof(transport_btn_ctx_t));
    *prev_ctx = (transport_btn_ctx_t){ prev_btn, "playing_plane/btn_prev.png", "playing_plane/btn_prev_s.png" };
    lv_obj_add_event_cb(prev_btn, transport_btn_press_event_cb, LV_EVENT_PRESSED, prev_ctx);
    lv_obj_add_event_cb(prev_btn, transport_btn_press_event_cb, LV_EVENT_RELEASED, prev_ctx);
    lv_obj_add_event_cb(prev_btn, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, prev_ctx);

    play_btn = lv_image_create(controls_row);
    lv_image_set_src(play_btn, asset_path("playing_plane/btn_play.png"));
    lv_obj_add_flag(play_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * next_btn = lv_image_create(controls_row);
    lv_image_set_src(next_btn, asset_path("playing_plane/btn_next.png"));
    lv_obj_add_flag(next_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);
    transport_btn_ctx_t * next_ctx = malloc(sizeof(transport_btn_ctx_t));
    *next_ctx = (transport_btn_ctx_t){ next_btn, "playing_plane/btn_next.png", "playing_plane/btn_next_s.png" };
    lv_obj_add_event_cb(next_btn, transport_btn_press_event_cb, LV_EVENT_PRESSED, next_ctx);
    lv_obj_add_event_cb(next_btn, transport_btn_press_event_cb, LV_EVENT_RELEASED, next_ctx);
    lv_obj_add_event_cb(next_btn, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, next_ctx);

    /* 3-dot "more" menu -- rightmost, matching the reference layout. Opens
     * more_menu_popup (Add to Playlist / EQ / Delete). */
    lv_obj_t * more_icon = lv_image_create(controls_row);
    lv_image_set_src(more_icon, asset_path("playing_plane/ic_more.png"));
    lv_obj_add_flag(more_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(more_icon, 16);
    lv_obj_add_event_cb(more_icon, more_icon_event_cb, LV_EVENT_CLICKED, NULL);

    /* Volume is controlled via hardware buttons (see update_timer_cb) and,
     * per the real device, shown only as a transient overlay rather than a
     * permanently visible slider (Task #28) -- kept alive here, just
     * invisible, so the existing hw-button volume logic keeps working
     * unchanged until that overlay lands. */
    volume_slider = lv_slider_create(scr);
    lv_obj_add_flag(volume_slider, LV_OBJ_FLAG_HIDDEN);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, (int32_t) (audio_get_volume() * 100.0f), LV_ANIM_OFF);
    lv_obj_add_event_cb(volume_slider, volume_slider_event_cb, LV_EVENT_ALL, NULL);

    finalize_screen_navigation(scr);
    return scr;
}

/* Same true-black default screen_builders.c's own style_theme_screen_bg is
 * initialized with. Kept as a separate plain literal (not that shared
 * style) specifically for gui_show_boot_splash() below -- that function
 * runs from main.c before gui_init() (and screen_builders_init_list_row_style(),
 * which initializes style_theme_screen_bg) has ever run, so the shared
 * style would still be all-zero memory at that point. */
#define SCREEN_BG_COLOR lv_color_make(0, 0, 0)

/* Real device uptime (lv_tick_get() is CLOCK_MONOTONIC-backed, see main.c's
 * custom_tick_get()) at which gui_show_boot_splash() below was called --
 * read back by gui_init() near the end of its own setup to decide how much
 * longer, if any, the splash needs to stay up. 0 is never a real tick value
 * this far into boot, so it doubles as "splash was never shown". */
static uint32_t boot_splash_start_tick = 0;

/* Stock player's own boot image stays up at least this long -- see
 * gui_show_boot_splash()'s comment. Used by gui_init()'s own settle-wait,
 * further below.
 *
 * Deliberately does NOT wait for /etc/init.d/S80_bt_init's own
 * /usr/bin/bt_init to finish -- real-device confirmed requirement:
 * Bluetooth must read "off" right when the splash lifts and STAY off
 * (no auto-enable of any kind) until the user explicitly turns it on
 * themselves. Two earlier attempts at extending this wait to hide
 * bt_init's own boot-time Bluetooth behavior, and a later attempt at
 * having this app auto-enable Bluetooth once bt_init finished in the
 * background, were all tried and reverted -- this app now does neither:
 * it just leaves Bluetooth exactly where bt_init's own (patched) script
 * leaves it, off, and never touches it again until the user does. */
#define BOOT_SPLASH_MIN_DISPLAY_MS 3000

/* Task #44 (stock-UX request): the stock firmware holds its own boot image
 * on screen for a few seconds after the very first kernel/bootloader logo
 * (S11jpeg_display_shell), before its player is interactive -- this app had
 * no equivalent, racing straight into screen-building and Bluetooth polling
 * within ~2s of process start. Called from main.c immediately after the
 * framebuffer is ready, before any of gui_init()'s own (much heavier) setup
 * work, so this is the very first thing this app ever paints, and so the
 * elapsed-time math in gui_init()'s own wait (see boot_splash_start_tick's
 * use below) is measured from as close to true process start as possible.
 *
 * Uses asset_path("boot_animation/en/0.png") rather than a hardcoded path:
 * this app's own THEME_ROOT (theme2) has no boot_animation asset at all --
 * only theme1 does, and that file is the stock "HIBY" wordmark, which this
 * project deliberately doesn't ship (see 89d7ca6d9, "rename app off the
 * HiBy trademark"). asset_path()'s existing THEME_OVERRIDE_ROOT check
 * (assets.c) means a non-trademarked replacement dropped at
 * /usr/data/theme_overrides/boot_animation/en/0.png is picked up
 * automatically with no code change or reflash. Until that file exists,
 * the underlying decode simply fails and lv_image renders nothing --
 * SCREEN_BG_COLOR alone still gives a clean black screen instead of
 * whatever the framebuffer previously held, so this is safe either way.
 *
 * Real-device bug report: the status bar (build_status_bar(), and every
 * other overlay this app builds the same way -- popups, quick drawer,
 * volume popup) parents itself onto lv_layer_top(), LVGL's global overlay
 * layer that renders above whichever screen is active *regardless* of
 * lv_screen_load() -- it's not scoped to home_screen at all. Since
 * gui_init() builds the status bar (and starts populating its icons) while
 * this splash is still the loaded screen, it was appearing on top of the
 * splash within about a second of boot, well before gui_init()'s own
 * settle-wait even finished -- reading as "the splash barely showed" even
 * though the splash screen object itself stayed loaded for the full
 * BOOT_SPLASH_MIN_DISPLAY_MS (confirmed via boot_debug.log timestamps).
 * Hiding the whole top layer here and revealing it once gui_init()'s wait
 * is done (see its own call site further below) keeps every one of those
 * overlays off-screen for exactly as long as the splash itself is up,
 * without needing to touch each overlay builder individually. */
void gui_show_boot_splash(void) {
    boot_splash_start_tick = lv_tick_get();

    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * scr = lv_obj_create(NULL);
    /* Plain literal, not style_theme_screen_bg -- this runs from main.c
     * before gui_init() (and its lv_style_init(&style_theme_screen_bg))
     * has ever run, so that style object would still be all-zero memory
     * here. */
    lv_obj_set_style_bg_color(scr, SCREEN_BG_COLOR, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t * img = lv_image_create(scr);
    lv_image_set_src(img, asset_path("boot_animation/en/0.png"));
    lv_obj_center(img);

    lv_screen_load(scr);
    lv_timer_handler(); /* force an immediate render -- nothing else pumps the loop until main.c's own main loop starts */
}

static lv_obj_t * build_files_screen(void) {
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
    lv_label_set_text(title, "Files");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_28, 0);

    file_browser_init(scr, MUSIC_ROOT_DIR, on_file_browser_selected);

    finalize_screen_navigation(scr);
    return scr;
}

static void music_files_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(files_screen);
}

/* Library-wide (not one-directory) views -- All Songs, Artists, Albums --
 * all built from one scan-and-tag-read pass over the whole music root,
 * done once at startup (library_scan_once(), called from gui_init before
 * any of these screens are built) rather than every time a screen is
 * entered: simpler, and a personal music library isn't expected to change
 * shape mid-session the way a single directory being actively browsed
 * might. */
static char ** all_songs_paths = NULL;
static int all_songs_count = 0;

static const char * basename_of(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Parallel array to all_songs_paths -- title/artist/album tag per song
 * (reading all of them from the same metadata_read() call, rather than once
 * per grouping, since that's the expensive part of indexing a whole
 * library). Untagged artist/album/album_artist/genre fall back to an
 * explicit "Unknown" bucket rather than silently dropping the song from
 * these views the way a missing group would -- title is the odd one out: it
 * isn't a grouping key, just a display string, so it's left empty on a
 * missing tag and show_group_songs() falls back to the filename itself at
 * render time instead of baking a fake title into the cache. */
typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char album_artist[128];
    char genre[128];
} song_tags_t;

static song_tags_t * all_song_tags = NULL;

/* Real song title (from tags) everywhere navigation is metadata-driven
 * (Artists/Albums/Album Artist/Genre drill-downs, All Songs) -- falls back
 * to the filename only when the file genuinely has no title tag, never as
 * the default. The plain file/folder browser (file_browser.c) is a
 * different, deliberately filename-based view and does NOT go through this
 * -- browsing raw files on the card needs to show what's actually on disk. */
static const char * song_display_title_of(int index) {
    if (all_song_tags[index].title[0] != '\0') return all_song_tags[index].title;
    return basename_of(all_songs_paths[index]);
}

/* One artist/album/album-artist/genre's songs, as indices into
 * all_songs_paths/all_song_tags (not owned copies -- those arrays already
 * own the strings and outlive every group). */
typedef struct {
    char name[128];
    int * indices;
    int count;
} group_t;

static group_t * artist_groups = NULL;
static int artist_group_count = 0;
static group_t * album_groups = NULL;
static int album_group_count = 0;
static group_t * album_artist_groups = NULL;
static int album_artist_group_count = 0;

static const char * artist_key_of(int index) { return all_song_tags[index].artist; }
static const char * album_key_of(int index) { return all_song_tags[index].album; }
static const char * album_artist_key_of(int index) { return all_song_tags[index].album_artist; }

static int compare_index_by_artist(const void * a, const void * b) {
    int ia = *(const int *) a, ib = *(const int *) b;
    int cmp = strcasecmp(artist_key_of(ia), artist_key_of(ib));
    return cmp != 0 ? cmp : strcasecmp(all_songs_paths[ia], all_songs_paths[ib]);
}

static int compare_index_by_album(const void * a, const void * b) {
    int ia = *(const int *) a, ib = *(const int *) b;
    int cmp = strcasecmp(album_key_of(ia), album_key_of(ib));
    return cmp != 0 ? cmp : strcasecmp(all_songs_paths[ia], all_songs_paths[ib]);
}

static int compare_index_by_album_artist(const void * a, const void * b) {
    int ia = *(const int *) a, ib = *(const int *) b;
    int cmp = strcasecmp(album_artist_key_of(ia), album_artist_key_of(ib));
    return cmp != 0 ? cmp : strcasecmp(all_songs_paths[ia], all_songs_paths[ib]);
}

/* All Songs' own display order -- indices into all_songs_paths/all_song_tags
 * sorted by display title, unlike artist_groups/album_groups/
 * album_artist_groups this isn't a group_t array (nothing to collapse:
 * every song is its own row, including duplicate titles) so a plain sorted
 * index array is all that's needed. Powers both build_all_songs_screen()'s
 * display order and its A-Z browse index. */
static int * all_songs_sort_order = NULL;

static int compare_index_by_title(const void * a, const void * b) {
    int ia = *(const int *) a, ib = *(const int *) b;
    int cmp = strcasecmp(song_display_title_of(ia), song_display_title_of(ib));
    return cmp != 0 ? cmp : strcasecmp(all_songs_paths[ia], all_songs_paths[ib]);
}

static void rebuild_all_songs_sort_order(void) {
    free(all_songs_sort_order);
    all_songs_sort_order = NULL;
    if (all_songs_count <= 0) return;
    all_songs_sort_order = malloc(sizeof(int) * (size_t) all_songs_count);
    for (int i = 0; i < all_songs_count; i++) all_songs_sort_order[i] = i;
    qsort(all_songs_sort_order, (size_t) all_songs_count, sizeof(int), compare_index_by_title);
}

/* Sorts the given song indices by key_of (via cmp, which also tie-breaks by
 * path so each resulting group's members come out path-sorted for free) and
 * slices the sorted run into one group_t per distinct key. Used both for
 * whole-library grouping (build_groups_by, below) and for regrouping a
 * single group's members by a second key -- e.g. an artist's songs by
 * album, for show_artist_albums(). */
static void build_groups_by_indices(const int * indices, int count,
                                     const char * (*key_of)(int), int (*cmp)(const void *, const void *),
                                     group_t ** out_groups, int * out_count) {
    if (count <= 0) { *out_groups = NULL; *out_count = 0; return; }

    int * order = malloc(sizeof(int) * (size_t) count);
    memcpy(order, indices, sizeof(int) * (size_t) count);
    qsort(order, (size_t) count, sizeof(int), cmp);

    group_t * groups = NULL;
    int group_count = 0;
    int group_capacity = 0;

    int i = 0;
    while (i < count) {
        const char * key = key_of(order[i]);
        int j = i;
        while (j < count && strcasecmp(key_of(order[j]), key) == 0) j++;
        int member_count = j - i;

        if (group_count == group_capacity) {
            group_capacity = group_capacity ? group_capacity * 2 : 16;
            groups = realloc(groups, sizeof(group_t) * (size_t) group_capacity);
        }
        group_t * g = &groups[group_count++];
        snprintf(g->name, sizeof(g->name), "%s", key);
        g->count = member_count;
        g->indices = malloc(sizeof(int) * (size_t) member_count);
        memcpy(g->indices, &order[i], sizeof(int) * (size_t) member_count);

        i = j;
    }

    free(order);
    *out_groups = groups;
    *out_count = group_count;
}

static void build_groups_by(const char * (*key_of)(int), int (*cmp)(const void *, const void *),
                             group_t ** out_groups, int * out_count) {
    if (all_songs_count <= 0) { *out_groups = NULL; *out_count = 0; return; }

    int * order = malloc(sizeof(int) * (size_t) all_songs_count);
    for (int i = 0; i < all_songs_count; i++) order[i] = i;
    build_groups_by_indices(order, all_songs_count, key_of, cmp, out_groups, out_count);
    free(order);
}

static void free_group_array(group_t * groups, int count) {
    for (int i = 0; i < count; i++) free(groups[i].indices);
    free(groups);
}

/* Undoes everything library_scan_once() below allocates -- needed before a
 * rescan (Settings > Update Music Database) can safely repopulate these from
 * scratch, not just on first run. */
static void library_free_scan_state(void) {
    for (int i = 0; i < all_songs_count; i++) free(all_songs_paths[i]);
    free(all_songs_paths);
    all_songs_paths = NULL;
    free(all_song_tags);
    all_song_tags = NULL;
    all_songs_count = 0;

    free_group_array(artist_groups, artist_group_count);
    artist_groups = NULL;
    artist_group_count = 0;
    free_group_array(album_groups, album_group_count);
    album_groups = NULL;
    album_group_count = 0;
    free_group_array(album_artist_groups, album_artist_group_count);
    album_artist_groups = NULL;
    album_artist_group_count = 0;
}

/* A stuck read on a corrupted SD card block (confirmed on a real device via
 * /proc/<pid>/task/<tid>/status+wchan showing the main thread parked in
 * __bread_gfp, a kernel block-device read, after the card came back from an
 * unclean unmount) lands the calling thread in an uninterruptible (D-state)
 * kernel wait. That can't be interrupted by a signal, alarm(), or
 * pthread_cancel() -- the only way to bound it is to run the risky call on
 * its own throwaway thread and simply stop waiting on it if it doesn't
 * finish in time. On timeout, the thread and its heap-allocated work struct
 * below are deliberately never joined or freed: the worker may still be
 * blocked in the kernel indefinitely, joining it would just reintroduce the
 * exact hang this exists to avoid, and freeing memory it might still write
 * to would be a use-after-free. This leaks one thread and one small
 * allocation per genuinely stuck path for the life of the process, which
 * only happens on real filesystem corruption -- a bounded cost, and far
 * better than the whole UI freezing. */
#define LIBRARY_SCAN_FILE_TIMEOUT_MS 5000
/* Real-device bug report: a library of a few thousand tracks spread across
 * many subfolders (nested Artist/Album directories on a plain SD card
 * labeled "Music") scanned as completely empty -- no error shown anywhere
 * a real deployment could surface one (stderr has no reader outside a
 * TEST_BUILD_TAG debug session, see debug_log.h), just a silently empty
 * library. Root cause: this used to be a flat wall-clock budget for the
 * ENTIRE recursive walk (file_browser.c's scan_all_songs_recursive()) --
 * fine for the genuine-corruption case it was built for (a single lstat()
 * stuck in an uninterruptible kernel wait, same reasoning as
 * LIBRARY_SCAN_FILE_TIMEOUT_MS above), but it couldn't tell that apart from
 * a large, healthy, deeply-nested library on a slow card just legitimately
 * taking longer than the flat budget to finish readdir()+lstat() on every
 * entry -- the whole scan was discarded either way. Now a stall timeout
 * instead (see the progress-counter poll loop below): as long as
 * file_browser_scan_all_songs() keeps making forward progress (any real
 * directory entry examined, not just playable files -- see its own
 * progress parameter's doc comment in file_browser.h), no matter how long
 * the walk takes in total, it's never treated as stuck. Only a stretch of
 * this many ms with zero new progress -- the actual "something is
 * genuinely wedged" signal -- gives up. */
#define LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS 30000

typedef struct {
    char root[600];
    volatile bool done;
    bool ok;
    char ** paths;
    int count;
    volatile int progress;
} scan_walk_work_t;

static void * scan_walk_worker(void * arg) {
    scan_walk_work_t * w = (scan_walk_work_t *) arg;
    w->ok = file_browser_scan_all_songs(w->root, &w->paths, &w->count, &w->progress);
    w->done = true; /* written last -- the only field polled below */
    return NULL;
}

/* Real-device incident: scan_walk_worker() recurses into every subdirectory
 * (file_browser.c's scan_all_songs_recursive(), a few KB of stack per level
 * for its own PATH_MAX buffer), and a plain pthread_create() with no
 * explicit attr gets musl's fairly small default stack -- confirmed live as
 * a real crash (stack overflow surfacing as a SIGSEGV inside vsnprintf) even
 * after that function grew its own depth cap and stopped following
 * symlinks. 1MB is cheap on this device for one short-lived worker thread
 * (a rescan finishes long before it would ever hold that memory for long)
 * and leaves enormous headroom over the worst case even at the cap. Falls
 * back to the default stack size if setting this fails, rather than not
 * spawning the scan at all -- attribute setup failing is not itself a
 * reason to skip the whole feature. */
#define SCAN_WALK_THREAD_STACK_SIZE (1024 * 1024)
static bool scan_all_songs_with_timeout(const char * root, char *** out_paths, int * out_count) {
    scan_walk_work_t * w = calloc(1, sizeof(*w));
    snprintf(w->root, sizeof(w->root), "%s", root);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, SCAN_WALK_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }

    pthread_t thread;
    bool created = pthread_create(&thread, attr_ptr, scan_walk_worker, w) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
        free(w);
        return false;
    }
    pthread_detach(thread); /* never joined either way -- see block comment above */

    /* Stall detection, not a flat budget -- see LIBRARY_SCAN_WALK_STALL_
     * TIMEOUT_MS's own comment. Same shape as scan_songs_range()'s own
     * completed_index polling further down, just watching w->progress
     * instead. */
    int last_seen_progress = 0;
    int stalled_ms = 0;
    for (;;) {
        if (w->done) {
            bool ok = w->ok;
            *out_paths = w->paths;
            *out_count = w->count;
            free(w);
            return ok;
        }
        int progress = w->progress;
        if (progress != last_seen_progress) {
            last_seen_progress = progress;
            stalled_ms = 0;
        } else {
            stalled_ms += 20;
            if (stalled_ms >= LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS) break;
        }
        usleep(20000);
    }

    fprintf(stderr, "Warning: scan of %s stalled with no progress for %ds (possible filesystem corruption) -- treating library as empty\n",
            root, LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS / 1000);
    return false;
}

/* Runs the original (pre-timeout) stat + cache-lookup + metadata_read body
 * for every file from start_index to all_songs_count, writing straight into
 * the shared all_song_tags array -- exactly like a plain for loop, no
 * per-file thread spawning. An earlier version of this function ran each
 * file's stat()/metadata_read() on its own throwaway thread; that bounded a
 * stuck read correctly, but on this device (single core, 56MB RAM) spawning
 * two threads per file made a real ~2000-song library visibly slow, purely
 * from thread-create overhead on files that were never actually going to
 * hang. completed_index is updated after each file so scan_songs_range()
 * (below) can tell this worker apart from a genuinely stuck one without
 * touching any per-file timing itself. */
typedef struct {
    int start_index;
    volatile int completed_index; /* last index this worker fully finished; written last within each iteration */
    volatile bool done;
} scan_range_work_t;

/* Overall scan progress, polled by update_timer_cb while library_rescan_active
 * is true to drive the "Updating music database..." screen's progress bar
 * (Settings > Update Music Database) -- purely cosmetic, for the user's
 * peace of mind that a rescan is actually moving rather than stuck, since
 * the underlying incremental scan (metadata_db.c's mtime/size cache) is
 * already fast on an unchanged library. _total is set once all_songs_count
 * is known (library_scan_once(), before scan_songs_range() starts); _done
 * only ever increases across however many sequential scan_range_worker
 * threads a scan actually uses (see scan_songs_range()'s own comment on
 * why there can be more than one), so a single running total here is safe
 * without needing to know which worker is currently active. */
static volatile int library_scan_progress_done = 0;
static int library_scan_progress_total = 0;

static void * scan_range_worker(void * arg) {
    scan_range_work_t * w = (scan_range_work_t *) arg;
    w->completed_index = w->start_index - 1;

    for (int i = w->start_index; i < all_songs_count; i++) {
        struct stat st;
        bool have_stat = stat(all_songs_paths[i], &st) == 0;
        int64_t mtime = have_stat ? (int64_t) st.st_mtime : 0;
        int64_t size = have_stat ? (int64_t) st.st_size : 0;

        cached_tags_t cached;
        /* cached.title[0] == '\0' also forces a fresh read, not just a
         * missing/changed cache row -- rows written before the title column
         * existed got backfilled to '' by metadata_db.c's ALTER TABLE
         * migration, and would otherwise stay blank forever (mtime/size
         * still match, so metadata_db_get() would keep "hitting" that same
         * stale empty title on every future scan too). The ongoing cost for
         * a file that genuinely has no title tag is one extra
         * metadata_read() per scan for that file only, not the whole
         * library -- worth it to self-heal every already-cached library
         * the first time it's rescanned under the new schema. */
        if (have_stat && metadata_db_get(all_songs_paths[i], mtime, size, &cached) && cached.title[0] != '\0') {
            snprintf(all_song_tags[i].title, sizeof(all_song_tags[i].title), "%s", cached.title);
            snprintf(all_song_tags[i].artist, sizeof(all_song_tags[i].artist), "%s", cached.artist);
            snprintf(all_song_tags[i].album, sizeof(all_song_tags[i].album), "%s", cached.album);
            snprintf(all_song_tags[i].album_artist, sizeof(all_song_tags[i].album_artist), "%s", cached.album_artist);
            snprintf(all_song_tags[i].genre, sizeof(all_song_tags[i].genre), "%s", cached.genre);
            w->completed_index = i;
            library_scan_progress_done = i + 1;
            continue;
        }

        /* Isolated (forked + hard-killed on timeout), not a plain
         * metadata_read() call -- real-device incident: a handful of FLAC
         * files with a malformed leading ID3v2 tag made the underlying
         * parser (dr_flac, misparsing garbage as fake metadata blocks --
         * see metadata_read_isolated()'s own comment) pathologically slow
         * on this single-core device, and since the stuck-worker handling
         * below can only ever ABANDON a stuck thread (never actually stop
         * it), several such files in a row each left one more runaway
         * thread permanently competing for the one core -- which read as
         * the whole device freezing even though no single file was
         * technically stuck forever. This still leaves the stuck-worker
         * handling below in place as a backstop, but it should now rarely
         * or never trigger, since a single file's parse can no longer
         * block this worker indefinitely. */
        track_metadata_t meta;
        metadata_read_isolated(all_songs_paths[i], &meta, LIBRARY_SCAN_FILE_TIMEOUT_MS);
        snprintf(all_song_tags[i].title, sizeof(all_song_tags[i].title), "%s", meta.has_title ? meta.title : "");
        snprintf(all_song_tags[i].artist, sizeof(all_song_tags[i].artist), "%s", meta.has_artist ? meta.artist : "Unknown Artist");
        snprintf(all_song_tags[i].album, sizeof(all_song_tags[i].album), "%s", meta.has_album ? meta.album : "Unknown Album");
        /* Album artist falls back to the track artist (not "Unknown") when
         * there's no explicit ALBUMARTIST/TPE2/aART tag -- for the common
         * non-compilation case, the album artist genuinely IS the track
         * artist, and a tagger simply not bothering to write a redundant
         * second tag shouldn't dump the song into an "Unknown" bucket the
         * user has to know to check. */
        const char * album_artist_value = meta.has_album_artist ? meta.album_artist
                                         : (meta.has_artist ? meta.artist : "Unknown Artist");
        snprintf(all_song_tags[i].album_artist, sizeof(all_song_tags[i].album_artist), "%s", album_artist_value);
        snprintf(all_song_tags[i].genre, sizeof(all_song_tags[i].genre), "%s", meta.has_genre ? meta.genre : "Unknown Genre");
        free(meta.picture_data); /* only the tags were needed, not the art */

        if (have_stat) {
            cached_tags_t fresh;
            snprintf(fresh.title, sizeof(fresh.title), "%s", all_song_tags[i].title);
            snprintf(fresh.artist, sizeof(fresh.artist), "%s", all_song_tags[i].artist);
            snprintf(fresh.album, sizeof(fresh.album), "%s", all_song_tags[i].album);
            snprintf(fresh.album_artist, sizeof(fresh.album_artist), "%s", all_song_tags[i].album_artist);
            snprintf(fresh.genre, sizeof(fresh.genre), "%s", all_song_tags[i].genre);
            metadata_db_put(all_songs_paths[i], mtime, size, &fresh);
        }
        w->completed_index = i; /* written last -- the only field scan_songs_range() polls */
        library_scan_progress_done = i + 1;
    }

    w->done = true;
    return NULL;
}

/* Covers [start_index, all_songs_count) by handing the whole range to one
 * worker thread -- the common case (a healthy card) pays for exactly one
 * thread for the entire scan, not one per file. If the worker goes
 * LIBRARY_SCAN_FILE_TIMEOUT_MS without finishing another file, it's
 * considered stuck on completed_index+1 (a corrupted block -- see the block
 * comment further up on why this can't be interrupted, only abandoned): that
 * worker is leaked exactly like the timeout cases above, the stuck file's
 * tags are filled with a placeholder, and a fresh worker picks up one file
 * further along. Thread creation only happens once per scan plus once per
 * actual stall, so a large healthy library costs the same as the original
 * unbounded loop. */
static void scan_songs_range(int start_index) {
    while (start_index < all_songs_count) {
        scan_range_work_t * w = calloc(1, sizeof(*w));
        w->start_index = start_index;

        pthread_t thread;
        if (pthread_create(&thread, NULL, scan_range_worker, w) != 0) {
            free(w);
            return; /* can't spawn a thread at all -- remaining tags stay at their malloc'd (garbage) contents, same failure mode as the rest of this call chain already tolerates */
        }
        pthread_detach(thread); /* never joined either way -- see block comment above */

        int last_seen = start_index - 1;
        int waited_ms = 0;
        for (;;) {
            if (w->done) { free(w); return; }
            int completed = w->completed_index;
            if (completed != last_seen) {
                last_seen = completed;
                waited_ms = 0;
            } else {
                waited_ms += 20;
                if (waited_ms >= LIBRARY_SCAN_FILE_TIMEOUT_MS) break;
            }
            usleep(20000);
        }

        int stuck_index = last_seen + 1; /* w is now abandoned -- never touch it again */
        fprintf(stderr, "Warning: timed out reading tags from %s (possible filesystem corruption) -- skipping\n", all_songs_paths[stuck_index]);
        all_song_tags[stuck_index].title[0] = '\0';
        snprintf(all_song_tags[stuck_index].artist, sizeof(all_song_tags[stuck_index].artist), "Unknown Artist");
        snprintf(all_song_tags[stuck_index].album, sizeof(all_song_tags[stuck_index].album), "Unknown Album");
        snprintf(all_song_tags[stuck_index].album_artist, sizeof(all_song_tags[stuck_index].album_artist), "Unknown Artist");
        snprintf(all_song_tags[stuck_index].genre, sizeof(all_song_tags[stuck_index].genre), "Unknown Genre");
        start_index = stuck_index + 1;
    }
}

/* Defined later, alongside the rest of the Books screen (needs
 * books_scan_txt_files_with_timeout(), the live-walk fallback it shares
 * with populate_books_files_screen()'s old scanning code). Folds the
 * now-removed "Scanning" row's job into this same rescan, per real-device
 * feedback -- one rescan action, not two separate ones for music and
 * books. */
static void rescan_books(void);

/* Refreshes the persistent playlist cache (metadata_db.c) from a real
 * recursive scan of MUSIC_ROOT_DIR -- folded into library_scan_once() just
 * below, same as rescan_books() above, so it runs at boot and on an
 * explicit Settings > Update Music Database rescan, never on the fast
 * cache-only boot path (library_load_from_cache_only()) or on every
 * Playlists screen visit -- that per-visit live scan was the real-device-
 * reported ~5s-to-open bug this fixes, see metadata_db_playlist_replace_
 * all()'s own comment. Ordinary in-app playlist create/delete
 * (playlist_files_create()/playlist_files_delete() call sites) update the
 * cache with a single-row insert/delete instead of calling this. */
static void rescan_playlists(void) {
    char ** paths = NULL;
    int count = 0;
    playlist_files_scan(MUSIC_ROOT_DIR, &paths, &count);
    metadata_db_playlist_replace_all(paths, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void library_scan_once(void) {
    library_free_scan_state();
    library_scan_progress_done = 0;
    library_scan_progress_total = 0;
    scan_all_songs_with_timeout(MUSIC_ROOT_DIR, &all_songs_paths, &all_songs_count);

    /* metadata_db_open() is idempotent (a no-op once already open, see its
     * own guard) -- called here, ahead of its other call site below, so
     * rescan_books() always has a real db handle to write into regardless
     * of whether any music was found. Unconditional, before the
     * all_songs_count==0 early return just below -- an empty (or
     * music-free) library shouldn't skip refreshing the book cache too,
     * they're independent collections. */
    metadata_db_open();
    rescan_books();
    rescan_playlists();

    if (all_songs_count == 0) return;

    library_scan_progress_total = all_songs_count;

    metadata_db_begin_update();

    all_song_tags = malloc(sizeof(song_tags_t) * (size_t) all_songs_count);
    scan_songs_range(0);

    metadata_db_end_update();

    build_groups_by(artist_key_of, compare_index_by_artist, &artist_groups, &artist_group_count);
    build_groups_by(album_key_of, compare_index_by_album, &album_groups, &album_group_count);
    build_groups_by(album_artist_key_of, compare_index_by_album_artist, &album_artist_groups, &album_artist_group_count);
    rebuild_all_songs_sort_order();
}

/* Boot-time equivalent of library_scan_once() above (still used verbatim
 * for the user-triggered Settings > Update Music Database rescan) that
 * matches the stock player's own boot behavior: load whatever's already
 * cached, don't walk the filesystem or re-read any file's tags. Real-device
 * incident: the full scan_once() path took ~149 seconds against a real
 * cache-cold 2066-song library (confirmed via persistent boot-checkpoint
 * logging -- library_scan_once() ran synchronously from gui_init(), before
 * this app's first frame could render or the main loop could start), long
 * enough that this app had never once survived a genuine cold boot in this
 * project's history -- no hardware watchdog would tolerate that delay, on
 * any user's library of meaningful size. metadata_db_open() itself stays
 * here (not removed): its own stock-db warm-start import (metadata_db.c)
 * is plain bounded SQLite row reads, not per-file filesystem I/O, and
 * finishes in well under a second even for a large library -- it's
 * specifically the filesystem walk + per-file tag-parsing pass that had to
 * move to the user-triggered-only path. */
static void library_load_from_cache_only(void) {
    library_free_scan_state();
    library_scan_progress_done = 0;
    library_scan_progress_total = 0;

    metadata_db_open();

    /* metadata_db_load_all() fills a cached_tags_t array (metadata_db.h);
     * all_song_tags is declared song_tags_t -- a separately-named struct
     * with an identical field layout (see song_tags_t's own comment), kept
     * distinct because gui.c's copy predates metadata_db.h and this is the
     * only place the two ever need to cross paths. Safe to reinterpret the
     * pointer rather than copy field-by-field. */
    cached_tags_t * tags = NULL;
    metadata_db_load_all(&all_songs_paths, &tags, &all_songs_count);
    all_song_tags = (song_tags_t *) tags;
    if (all_songs_count == 0) return;

    build_groups_by(artist_key_of, compare_index_by_artist, &artist_groups, &artist_group_count);
    build_groups_by(album_key_of, compare_index_by_album, &album_groups, &album_group_count);
    build_groups_by(album_artist_key_of, compare_index_by_album_artist, &album_artist_groups, &album_artist_group_count);
    rebuild_all_songs_sort_order();
}

/* Search binding IDs -- indices into search_bindings[], defined together
 * with the rest of the live-search infrastructure much further down (near
 * the A-Z index section, since it needs artist_groups/album_groups/etc.
 * already declared). Declared this early only because
 * all_songs_row_click_cb below (used by build_all_songs_screen(), itself
 * earlier in the file than the other three screens' own row-click
 * callbacks) needs to remap a filtered display index back to the real one
 * whenever a search is active. */
typedef enum {
    SEARCH_BINDING_ARTISTS,
    SEARCH_BINDING_ALBUMS,
    SEARCH_BINDING_ALBUM_ARTIST,
    SEARCH_BINDING_ALL_SONGS,
    SEARCH_BINDING_FILES,
    SEARCH_BINDING_SUBSONIC_ARTISTS,
    SEARCH_BINDING_SUBSONIC_ALBUMS,
    SEARCH_BINDING_COUNT
} search_binding_id_t;

static int search_remap_index(search_binding_id_t binding_id, int display_index);

static void all_songs_row_click_cb(int display_index) {
    display_index = search_remap_index(SEARCH_BINDING_ALL_SONGS, display_index);
    /* on_file_selected() takes ownership of (and eventually frees) the
     * playlist it's given, but all_songs_paths must survive for the next
     * tap too -- hand it a fresh copy rather than the persistent array
     * itself. Built in all_songs_sort_order (display order, the same
     * alphabetical order the A-Z index navigates) rather than raw scan
     * order, so next/prev during playback matches what's on screen. */
    char ** playlist_copy = malloc(sizeof(char *) * (size_t) all_songs_count);
    for (int i = 0; i < all_songs_count; i++) playlist_copy[i] = strdup(all_songs_paths[all_songs_sort_order[i]]);
    set_player_source_all_songs(display_index);
    on_file_selected(playlist_copy, all_songs_count, display_index);
}

static void all_songs_row_long_press_cb(int display_index) {
    display_index = search_remap_index(SEARCH_BINDING_ALL_SONGS, display_index);
    open_song_context_menu(all_songs_paths[all_songs_sort_order[display_index]]);
}

static lv_obj_t * build_all_songs_screen(void) {
    compact_list_item_t * items = NULL;
    if (all_songs_count > 0) {
        items = malloc(sizeof(compact_list_item_t) * (size_t) all_songs_count);
        for (int i = 0; i < all_songs_count; i++) {
            items[i] = (compact_list_item_t){ song_display_title_of(all_songs_sort_order[i]) };
        }
    }

    lv_obj_t * scr = build_compact_list_screen("All Songs", generic_back_cb, items, all_songs_count, all_songs_row_click_cb, all_songs_row_long_press_cb, &all_songs_list, LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    free(items);
    finalize_screen_navigation(scr);
    return scr;
}

/* Hands remote_control.c its own copy of the library for GET /api/library
 * and playlist mutation (see remote_control_sync_library()'s own doc
 * comment on why the index ordering must exactly match all_songs_paths/
 * all_song_tags) -- call every time those are rebuilt, same call sites as
 * build_all_songs_screen() itself. Builds three plain arrays rather than
 * passing all_song_tags directly since that struct also carries album/
 * album_artist/genre this doesn't need, and song_display_title_of()'s
 * filename-fallback logic isn't something remote_control.c should have to
 * duplicate. Paths are needed (not just for display) because playlist
 * creation/add-to-playlist reference a song by this same library index. */
static void sync_remote_control_library(void) {
    if (all_songs_count == 0) {
        remote_control_sync_library(NULL, NULL, NULL, NULL, NULL, 0);
        return;
    }
    const char ** titles = malloc(sizeof(char *) * (size_t) all_songs_count);
    const char ** artists = malloc(sizeof(char *) * (size_t) all_songs_count);
    const char ** album_artists = malloc(sizeof(char *) * (size_t) all_songs_count);
    const char ** albums = malloc(sizeof(char *) * (size_t) all_songs_count);
    const char ** paths = malloc(sizeof(char *) * (size_t) all_songs_count);
    for (int i = 0; i < all_songs_count; i++) {
        titles[i] = song_display_title_of(i);
        artists[i] = all_song_tags[i].artist;
        album_artists[i] = all_song_tags[i].album_artist;
        albums[i] = all_song_tags[i].album;
        paths[i] = all_songs_paths[i];
    }
    remote_control_sync_library(titles, artists, album_artists, albums, paths, all_songs_count);
    free(titles);
    free(artists);
    free(album_artists);
    free(albums);
    free(paths);
}

static void all_songs_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(all_songs_screen);
}

/* Shared drill-down target for both Artists and Albums: one persistent
 * screen whose row list is rebuilt each time (same "one object, rebuilt on
 * demand" approach file_browser.c uses for its own directory listing)
 * rather than pre-building a screen per artist/album -- a real library can
 * have hundreds of those, most never opened in a given session. */
static lv_obj_t * group_songs_screen;
static lv_obj_t * group_songs_title_label;
static lv_obj_t * group_songs_list;
static lv_obj_t * group_songs_edit_btn;
static const int * group_songs_indices; /* borrowed from whichever group_t is currently shown */
static int group_songs_count;

/* Now-playing indicator bar -- recreated fresh every populate_group_songs_
 * rows() call (that function's own lv_obj_clean(group_songs_list) destroys
 * whatever was here before, same as every row), so this pointer is only
 * ever valid between one populate call and the next, never stale across
 * one -- see refresh_group_songs_now_playing_indicator()'s own comment. */
static lv_obj_t * group_songs_now_playing_bar;

/* clear_player_source()/set_player_source_all_songs() etc. are defined
 * right after on_file_selected() -- this one needs group_songs_indices/
 * count/title_label above already in scope, which those didn't. */
/* Split out of set_player_source_group_songs() below so a caller that has
 * its own indices/title -- not the on-device Group Songs screen's own
 * group_songs_indices/title_label -- can set the same source kind without
 * touching that screen's shared, mutable display state. Used by remote
 * control's scoped play (play_remote_control_song()), which deliberately
 * never nav_pushes group_songs_screen and so must not reuse (and risk
 * corrupting, via rebuild_playlist_m3u_group()'s free()+realloc, out from
 * under whatever that screen currently has on-device) its live globals. */
static void set_player_source_group_songs_direct(const int * indices, int count, const char * title, int pos) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_GROUP_SONGS;
    player_source_group_title = strdup(title);
    player_source_group_indices = malloc(sizeof(int) * (size_t) count);
    memcpy(player_source_group_indices, indices, sizeof(int) * (size_t) count);
    player_source_group_count = count;
    player_source_group_pos = pos;
}

static void set_player_source_group_songs(int pos) {
    set_player_source_group_songs_direct(group_songs_indices, group_songs_count,
                                          lv_label_get_text(group_songs_title_label), pos);
}

/* Playlist design change: user .m3u playlists needed a way to remove a song
 * again after adding it. Non-NULL only when the group currently shown is a
 * user-created .m3u playlist (set by show_m3u_playlist() below, via
 * show_group_songs_editable()) -- stays NULL for Artists/Albums/Favorites/
 * Most Played, none of which back onto a file this app can rewrite (Favorites
 * is metadata_db-backed, Most Played is derived play-count ranking, and
 * unfavoriting/uncounting a song isn't what "remove from playlist" means for
 * either). Borrowed from playlists_m3u_paths[], same lifetime guarantee
 * show_favorites()/show_most_played()'s own indices arrays already rely on
 * (valid for as long as this screen is showing it). group_songs_edit_mode is
 * reset to false on every fresh entry into this screen so leaving and
 * re-entering never starts already in edit mode. */
static const char * group_songs_edit_m3u_path = NULL;
static bool group_songs_edit_mode = false;

/* Defined near show_m3u_playlist() further down, where playlist_m3u_group/
 * playlist_m3u_song_indices and file_browser_build_playlist_from_m3u() are
 * already in scope -- forward-declared here since populate_group_songs_rows()
 * (just below) needs to wire it up as the remove-icon's click handler. */
static void group_song_remove_row_cb(lv_event_t * e);

/* Defined near populate_playlists_screen() further down -- forward-declared
 * here since group_song_remove_row_cb() (below) and playlist_row_click_cb()
 * both need to refresh the Playlists screen's list after auto-deleting a
 * playlist that's become empty. */
static void populate_playlists_screen(void);

/* Real-device incident: LVGL still sends LV_EVENT_CLICKED on release even
 * when LV_EVENT_LONG_PRESSED already fired earlier in that same press --
 * same root cause/fix as quick_drawer_wifi_long_press_cb's own doc comment
 * (search that name for the full story) and compact_list_row_click_cb's
 * own matching fix (screen_builders.c) for All Songs -- without this, long-
 * pressing a Group Songs row (Artist/Album/Playlist/Favorites/Most Played)
 * to open the context menu also started that song playing on release. One
 * flag, not per-row -- single-touch device, only one row can plausibly be
 * mid-press at a time. */
static bool group_song_row_long_press_fired = false;

static void group_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_song_row_long_press_fired) {
        group_song_row_long_press_fired = false;
        return;
    }
    int pos = (int) (intptr_t) lv_event_get_user_data(e); /* position within the CURRENT group, not the whole library */

    char ** playlist_copy = malloc(sizeof(char *) * (size_t) group_songs_count);
    for (int i = 0; i < group_songs_count; i++) playlist_copy[i] = strdup(all_songs_paths[group_songs_indices[i]]);
    set_player_source_group_songs(pos);
    on_file_selected(playlist_copy, group_songs_count, pos);
}

static void group_song_row_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    group_song_row_long_press_fired = true;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    open_song_context_menu(all_songs_paths[group_songs_indices[pos]]);
}

/* Positions/shows or hides group_songs_now_playing_bar against the CURRENT
 * group_songs_indices/count -- callable standalone (no row rebuild, no
 * scroll reset) whenever now_playing_song_index changes while this screen
 * is open, and also called once at the end of populate_group_songs_rows()
 * itself so a freshly opened group (or an edit-mode toggle, which also goes
 * through a full repopulate) starts with the right state. Row height/gap
 * (LIST_ROW_HEIGHT+4, 4) are the same literals build_group_songs_screen()
 * already gives this list's own pad_top/pad_gap -- every row here is a
 * uniform LIST_ROW_HEIGHT regardless of edit mode, so row i's y is exactly
 * this formula even though this list is flex-laid-out (not manually
 * positioned like the compact-list infra's own pool). */
static void refresh_group_songs_now_playing_indicator(void) {
    if (!group_songs_now_playing_bar) return;

    int match = -1;
    if (now_playing_song_index >= 0) {
        for (int i = 0; i < group_songs_count; i++) {
            if (group_songs_indices[i] == now_playing_song_index) { match = i; break; }
        }
    }

    if (match < 0) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(group_songs_now_playing_bar, 0, 4 + match * (LIST_ROW_HEIGHT + 4));
}

/* Rebuilds group_songs_list's rows from whatever group_songs_indices/count
 * currently hold, in either of two shapes: a plain tappable-to-play label
 * (list_row_style, same as every other group -- Artists/Albums/Favorites/
 * Most Played always use this one) when not actively editing a playlist, or
 * a row with a trailing remove icon (no play-on-tap -- see
 * group_song_remove_row_cb() below) when group_songs_edit_mode is on for an
 * editable .m3u playlist. Split out from show_group_songs_editable() so the
 * remove callback and the Edit/Done toggle can both redraw in place without
 * re-deriving the group or nav_push()ing a second copy of this screen. */
static void populate_group_songs_rows(void) {
    lv_obj_clean(group_songs_list);

    bool editable = group_songs_edit_m3u_path != NULL;
    if (editable) {
        lv_obj_clear_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(group_songs_edit_btn, group_songs_edit_mode ? "Done" : "Edit");
    } else {
        lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    bool editing = editable && group_songs_edit_mode;

    for (int i = 0; i < group_songs_count; i++) {
        if (editing) {
            /* Container row (label + remove icon), same shape as e.g.
             * build_wifi_screen()'s saved/scanned network rows -- unlike the
             * plain-label rows below, this needs a second child so it can't
             * reuse list_row_style as-is. */
            lv_obj_t * row = lv_obj_create(group_songs_list);
            lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
            lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
            lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t * label = lv_label_create(row);
            lv_label_set_text(label, song_display_title_of(group_songs_indices[i]));
            lv_obj_add_style(label, &style_theme_text_primary, 0);
            lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

            lv_obj_t * remove_icon = lv_image_create(row);
            lv_image_set_src(remove_icon, asset_path("touch_list/del.png"));
            lv_obj_align(remove_icon, LV_ALIGN_RIGHT_MID, -20, 0);
            lv_obj_add_flag(remove_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(remove_icon, group_song_remove_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        } else {
            /* One lv_label via the shared list_row_style, not a container +
             * child label each with their own local style properties -- see
             * list_row_style's own doc comment (screen_builders.h). */
            lv_obj_t * row = lv_label_create(group_songs_list);
            lv_obj_add_style(row, &list_row_style, 0);
            lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_label_set_text(row, song_display_title_of(group_songs_indices[i]));

            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, group_song_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
            lv_obj_add_event_cb(row, group_song_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
        }
    }

    /* Recreated fresh here (lv_obj_clean() above just destroyed whatever
     * was here before) rather than kept as a truly persistent object --
     * every row in this list gets rebuilt on every populate call already,
     * so this just follows the same pattern. LV_OBJ_FLAG_IGNORE_LAYOUT
     * keeps this list's own flex column layout from trying to stack it in
     * as another row, same trick build_icon_grid_screen() uses for its
     * divider lines. Positioned/shown by refresh_group_songs_now_playing_
     * indicator() below, not here -- that also runs standalone (no rebuild)
     * whenever now_playing_song_index changes while this screen stays
     * open, e.g. a gapless auto-advance to the next track in the group. */
    group_songs_now_playing_bar = lv_obj_create(group_songs_list);
    lv_obj_remove_style_all(group_songs_now_playing_bar);
    lv_obj_set_size(group_songs_now_playing_bar, 5, LIST_ROW_HEIGHT);
    lv_obj_set_style_bg_color(group_songs_now_playing_bar, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(group_songs_now_playing_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(group_songs_now_playing_bar, 2, 0);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    refresh_group_songs_now_playing_indicator();
}

/* The player screen's "List" option -- reopens whichever screen the
 * current track was tapped from, scrolled back to it. Forward-declared
 * near the other more_menu_*_cb functions (build_more_menu_popup() wires
 * it up there); defined here instead since PLAYER_SOURCE_GROUP_SONGS
 * needs group_songs_screen/list/indices/count/title_label and
 * populate_group_songs_rows() all already in scope. */
static void more_menu_list_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();

    switch (player_source_kind) {
    case PLAYER_SOURCE_ALL_SONGS:
        nav_push(all_songs_screen);
        compact_list_scroll_to_index(all_songs_list, player_source_all_songs_index);
        break;
    case PLAYER_SOURCE_GROUP_SONGS:
        /* View-only when reached via List, even if the source group was
         * an editable .m3u playlist -- this is for orientation/navigation,
         * not editing, and group_songs_edit_m3u_path's own lifetime
         * guarantee (valid only "as long as this screen is showing it",
         * per its own comment) doesn't cover this snapshot anyway. */
        group_songs_edit_m3u_path = NULL;
        group_songs_edit_mode = false;
        group_songs_indices = player_source_group_indices;
        group_songs_count = player_source_group_count;
        lv_label_set_text(group_songs_title_label, player_source_group_title ? player_source_group_title : "");
        populate_group_songs_rows();
        nav_push(group_songs_screen);
        lv_obj_update_layout(group_songs_list);
        if (player_source_group_pos >= 0 && player_source_group_pos < (int) lv_obj_get_child_count(group_songs_list)) {
            lv_obj_scroll_to_view(lv_obj_get_child(group_songs_list, player_source_group_pos), LV_ANIM_OFF);
        }
        break;
    case PLAYER_SOURCE_FILE_BROWSER:
        nav_push(files_screen);
        file_browser_navigate_to(player_source_file_browser_dir, player_source_file_browser_row);
        break;
    case PLAYER_SOURCE_NONE:
    default:
        show_error_toast("No source list for this track");
        break;
    }
}

/* Every screen's back button is a fixed 64x64 at the screen's own left
 * edge (build_group_songs_screen() below, build_subsonic_list_screen()) --
 * this is where a title label should start, not centered over top of it. */
#define TITLE_LABEL_LEFT_INSET 76 /* 64px back button + 12px breathing room */
/* Default right margin for a title with no right-side icon of its own on
 * that particular screen -- most build_subsonic_list_screen() callers
 * (Playlists, Saved Servers, New Connection, Queue, DLNA, Remote Control,
 * the plugin list pool, ...) never get anywhere near this wide anyway
 * (always a short fixed string), so this is mostly headroom for the ones
 * that do have a long dynamic title but no button beside it (the local
 * library's Artist -> Albums drill-down, artist_albums_screen). */
#define TITLE_LABEL_DEFAULT_RIGHT_MARGIN 20

/* Narrows an already left-aligned, auto-scrolling title label (see
 * build_subsonic_list_screen()'s and build_group_songs_screen()'s own
 * construction) so its right edge stops before `right_icon`'s own left
 * edge, instead of running underneath it -- call once, right after
 * right_icon's own final on-screen position is set (real coordinates, not
 * a pending layout -- lv_label/lv_image both size/position synchronously
 * on lv_obj_align(), no intervening refresh pass needed). Safe to call
 * even while right_icon is currently hidden (e.g. a Download button only
 * shown for some views of a reused screen) -- a hidden object still has a
 * real position, and reserving room for it whether or not it's showing
 * right now is simpler and safer than tracking two different widths.
 *
 * Uses lv_obj_get_coords(), NOT lv_obj_get_x() -- real-device bug report:
 * lv_obj_get_x() reflects the coordinate as set relative to whichever
 * alignment reference the object was last lv_obj_align()'d against (e.g. a
 * TOP_RIGHT-aligned button's "x" isn't a left-relative position at all), so
 * comparing it against title's own TOP_LEFT-relative x produced nonsense.
 * lv_obj_get_coords() always returns real absolute screen coordinates
 * regardless of how the object was positioned, so title_area.x1/icon_area.x1
 * are directly comparable -- BUT only once a layout pass has actually run:
 * lv_obj_align() (lv_obj_pos.c) just sets style properties (align + offset)
 * for the layout engine to resolve LATER, it does not compute real
 * coordinates on the spot. Without lv_obj_update_layout() first,
 * lv_obj_get_coords() here was reading stale/uncommitted (0,0) coordinates
 * from before either object was ever positioned -- second real-device bug
 * report, same "only a couple characters visible" symptom as the
 * lv_obj_get_x() bug this replaced, different root cause. Forcing the
 * layout pass on the screen (both objects' common parent) resolves both at
 * once. */
static void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon) {
    lv_obj_update_layout(lv_obj_get_parent(title));

    lv_area_t title_area, icon_area;
    lv_obj_get_coords(title, &title_area);
    lv_obj_get_coords(right_icon, &icon_area);
    int32_t width = icon_area.x1 - title_area.x1 - 12;
    if (width < 40) width = 40; /* never collapse to nothing/negative on a pathological layout */
    lv_obj_set_width(title, width);
}

static void group_songs_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    group_songs_edit_mode = !group_songs_edit_mode;
    populate_group_songs_rows();
}

static void show_group_songs_editable(const group_t * group, const char * editable_m3u_path) {
    group_songs_edit_m3u_path = editable_m3u_path;
    group_songs_edit_mode = false;
    group_songs_indices = group->indices;
    group_songs_count = group->count;

    lv_label_set_text(group_songs_title_label, group->name);
    populate_group_songs_rows();

    nav_push(group_songs_screen);
}

static void show_group_songs(const group_t * group) {
    show_group_songs_editable(group, NULL);
}

static lv_obj_t * build_group_songs_screen(void) {
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

    group_songs_title_label = lv_label_create(scr);
    lv_label_set_text(group_songs_title_label, "");
    lv_obj_add_style(group_songs_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(group_songs_title_label, &app_font_28, 0); /* shows a metadata-derived artist/album/genre name -- see fallback_font.h */
    /* Real-device bug report: a long artist/album/genre/playlist name
     * (this label's whole reason for existing, per the comment above)
     * centered via LV_ALIGN_TOP_MID ran directly under the back button on
     * the left and the Edit button on the right. Left-aligned starting
     * just past the back button, narrowed below (once group_songs_edit_btn
     * itself exists) to stop before it, with auto-scroll for whatever
     * still overflows -- same fix as build_subsonic_list_screen()'s own
     * title. */
    int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    lv_obj_set_width(group_songs_title_label, scr_w - TITLE_LABEL_LEFT_INSET - TITLE_LABEL_DEFAULT_RIGHT_MARGIN);
    lv_label_set_long_mode(group_songs_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(group_songs_title_label, LV_ALIGN_TOP_LEFT, TITLE_LABEL_LEFT_INSET,
                 STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);

    /* Hidden by default -- populate_group_songs_rows() (via
     * show_group_songs_editable()) is what actually shows this, and only
     * for a group backed by an editable .m3u playlist. */
    group_songs_edit_btn = lv_label_create(scr);
    lv_label_set_text(group_songs_edit_btn, "Edit");
    lv_obj_set_style_text_color(group_songs_edit_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(group_songs_edit_btn, ui_size_20, 0);
    lv_obj_align(group_songs_edit_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(group_songs_edit_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    /* Reserved whether or not Edit is currently visible -- see
     * reserve_title_width_before()'s own comment on why that's safe/
     * simpler than tracking two different widths. */
    reserve_title_width_before(group_songs_title_label, group_songs_edit_btn);

    group_songs_list = lv_obj_create(scr);
    lv_obj_set_size(group_songs_list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(group_songs_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(group_songs_list, 0, 0);
    lv_obj_set_style_border_width(group_songs_list, 0, 0);
    lv_obj_set_scroll_dir(group_songs_list, LV_DIR_VER); /* see build_icon_grid_screen's comment */
    lv_obj_set_flex_flow(group_songs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(group_songs_list, 4, 0);
    lv_obj_set_style_pad_top(group_songs_list, 4, 0);

    finalize_screen_navigation(scr);
    return scr;
}

/* Reusable on-screen keyboard text entry -- the app had none of this
 * before network streaming needed a way to type a server URL/username/
 * password. One persistent screen (title + textarea + T9 keypad),
 * repurposed for whatever's being entered via show_text_entry() rather
 * than a screen per use, same "one object, rebuilt/retargeted on demand"
 * approach as group_songs_screen above. Deliberately skips
 * finalize_screen_navigation() (no swipe-to-go-back) -- a swipe gesture
 * while typing on the keypad would be an easy accidental way to lose
 * whatever was being entered. (text_entry_done_cb_t itself is forward-
 * declared earlier, alongside show_text_entry(), for the PEQ screen's
 * tap-to-edit handlers.)
 *
 * Real T9 multi-tap keypad, replacing an earlier lv_keyboard_create()-based
 * QWERTY layout -- feature request: reuse the stock firmware's own
 * keyboard/ theme assets (confirmed present under THEME_ROOT on a real
 * device, same asset_path() convention as every other themed icon in this
 * app) for a phone-keypad-style input instead of LVGL's generic keyboard
 * widget. 12-key layout (1-9, Mode, Shift, plus dedicated Del/Left/Right/
 * Enter/Space keys) in a 4-column x 5-row grid; keys 2-9 cycle through their
 * letter group (e.g. key 2 -> a -> b -> c) on repeated taps within
 * TEXT_ENTRY_MULTITAP_MS, matching classic phone-keypad multi-tap text
 * entry. Three keypad modes (ABC/NUM/SYM), cycled via the Mode key: ABC
 * cycles letters per key (0 and 1 have no letter group on this asset set,
 * so they always insert their literal digit); NUM makes every digit key
 * insert its literal digit directly, no cycling; SYM cycles each key's own
 * punctuation set (symbol0.png..symbol9.png). PEQ's numeric-only fields
 * (show_text_entry()'s `numeric` flag) skip all three and lock permanently
 * to plain-digit entry, hiding Mode/Shift (neither means anything for a
 * frequency/gain/Q value) and exposing a dedicated "./-" key instead (gain
 * needs negative numbers; nothing else here provides them once Mode is
 * hidden).
 *
 * Scope cuts, deliberate for this first pass: num_more.png/symbol_more.png
 * (a second punctuation page) aren't wired up -- symbol0-9 alone covers
 * ordinary punctuation (. , ! ? etc.), and every text_entry field here is a
 * short single-line value (URLs, names, credentials), not prose. The
 * select/ subfolder's pressed-state art isn't used either (no PRESSED/
 * RELEASED image swap, unlike screen_builders.c's icon-grid tiles) --
 * LV_EVENT_CLICKED alone is enough feedback for a keypad tap. bg.png/
 * text_bg.png/cursor.png (decorative chrome) and ok.png/ok_s.png (redundant
 * with enter.png's own baked-in "Enter" text) are unused. char_l.png/
 * char_u.png are never referenced -- confirmed byte-identical to each other
 * and to no clear distinct purpose from char.png (the Mode key's own ABC
 * glyph) by direct pixel inspection. */

static lv_obj_t * text_entry_screen;
static lv_obj_t * text_entry_title_label;
static lv_obj_t * text_entry_textarea;
static lv_obj_t * text_entry_keypad_group; /* wraps every T9 key -- reparented onto whichever screen has inline search open, see t9_keypad_attach()/t9_keypad_release() */
/* Enter's meaning while the keypad is on loan for inline search: dismiss
 * just the keypad (search stays active) instead of text_entry_commit()'s
 * normal nav_pop()-and-fire-callback modal flow -- see
 * text_entry_enter_click_cb(). */
static bool text_entry_inline_mode_active = false;
static lv_obj_t * text_entry_reveal_btn;
static text_entry_done_cb_t text_entry_on_done;
static void * text_entry_user_data;

/* ---- T9 keypad geometry: 5 columns x 4 rows of TEXT_ENTRY_KEY_SIZE keys,
 * centered under the 480px-wide screen and anchored to the bottom of the
 * 800px-tall screen (both hardcoded directly rather than shared from
 * SCREEN_WIDTH/SCREEN_HEIGHT constants -- main.c's copies aren't visible
 * here, same as everywhere else in this file that needs the screen's own
 * pixel size, e.g. COVER_ART_WIDTH above). Matches a real-device photo of
 * the stock keyboard exactly (not asset-name guessing -- an earlier 4x5
 * layout with a single cycling Mode key was wrong): column 0 holds three
 * always-visible mode-jump buttons (123/ABC/sym) stacked vertically rather
 * than one key that cycles between them; columns 1-3 hold the actual T9
 * 3x3 letter/digit/symbol pad; column 4 holds Del (row 0), the key-0/Shift
 * slot (row 1, see text_entry_key0_click_cb's own comment), and Enter
 * (rows 2-3, tall). Row 3's remaining cells are Left/Right and a wide
 * Space spanning columns 2-3. 5 columns at the native 94px key size only
 * just fits 480px wide (a 2px gap leaves a 1px margin each side) -- real
 * device photo confirms the stock keyboard packs this tight too. ---- */
#define TEXT_ENTRY_KEY_SIZE 94
#define TEXT_ENTRY_KEY_GAP 2
#define TEXT_ENTRY_GRID_COLS 5
#define TEXT_ENTRY_GRID_ROWS 4
#define TEXT_ENTRY_GRID_WIDTH (TEXT_ENTRY_GRID_COLS * TEXT_ENTRY_KEY_SIZE + (TEXT_ENTRY_GRID_COLS - 1) * TEXT_ENTRY_KEY_GAP)
#define TEXT_ENTRY_GRID_HEIGHT (TEXT_ENTRY_GRID_ROWS * TEXT_ENTRY_KEY_SIZE + (TEXT_ENTRY_GRID_ROWS - 1) * TEXT_ENTRY_KEY_GAP)
#define TEXT_ENTRY_BOTTOM_MARGIN 16
#define TEXT_ENTRY_GRID_X ((480 - TEXT_ENTRY_GRID_WIDTH) / 2)
#define TEXT_ENTRY_GRID_Y (800 - TEXT_ENTRY_GRID_HEIGHT - TEXT_ENTRY_BOTTOM_MARGIN)
#define TEXT_ENTRY_MULTITAP_MS 900

typedef enum {
    TEXT_ENTRY_KP_ABC = 0,
    TEXT_ENTRY_KP_NUM,
    TEXT_ENTRY_KP_SYM,
} text_entry_kp_mode_t;

/* Key index 10 is the numeric-lock-only "./-" key (T9_LETTER_GROUPS/
 * T9_SYMBOL_GROUPS below only cover 0-9, the real digit keys). NULL entries
 * mean "no letter group on this asset set" (keys 0 and 1) -- those always
 * insert their own literal digit in ABC mode, same as NUM mode does for
 * every key. */
static const char * const TEXT_ENTRY_LETTER_GROUPS[10] = {
    NULL, NULL, "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"
};
/* Read off symbolN.png's own rendered glyphs directly (contact-sheet
 * inspection, see this feature's own design notes) -- no declarative source
 * for these exists anywhere in the squashfs. */
static const char * const TEXT_ENTRY_SYMBOL_GROUPS[10] = {
    ".,", "!?", "()[]", "/\\|", "$%", "@#&", "+-*^", "_{}", ":;", "<=>"
};

static text_entry_kp_mode_t text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
/* Real-device bug report: Shift behaved as a persistent caps toggle (tap
 * once, every following letter stays capitalized until tapped again) --
 * every real T9/phone keypad instead treats these as two separate things:
 * Shift (the key-0 slot) capitalizes only the very next letter typed, then
 * reverts on its own, while tapping the ABC mode button AGAIN while it's
 * already the active mode is what toggles persistent caps ("Caps Lock").
 * text_entry_is_uppercase() below is the union of both -- everywhere that
 * used to read text_entry_shift directly for case now reads that instead. */
static bool text_entry_shift = false;     /* one-shot: consumed by the next letter (see text_entry_handle_digit_key) */
static bool text_entry_caps_lock = false; /* persistent: toggled by re-tapping the ABC mode button */
static bool text_entry_numeric_only = false;

static bool text_entry_is_uppercase(void) {
    return text_entry_caps_lock || text_entry_shift;
}

/* Case for a pending multi-tap cycle is pinned to whatever it was on that
 * cycle's FIRST tap (text_entry_handle_digit_key), not re-read live on every
 * repeat tap -- otherwise a one-shot Shift (consumed immediately after that
 * first tap, see below) would make the letter flip back to lowercase the
 * moment the user cycles to a second candidate for the SAME letter, even
 * though they're still resolving that one same keypress. */
static bool text_entry_pending_shift = false;

/* Multi-tap state: which key (0-10, or -1 = none) is mid-cycle and which
 * character of its cycle is currently showing in the textarea. A repeat tap
 * on the SAME key within TEXT_ENTRY_MULTITAP_MS deletes and re-inserts the
 * next character in its cycle; any other input (a different key, or the
 * timer simply elapsing) finalizes it, so the next tap on that key starts a
 * fresh cycle instead of continuing the old one. */
static int text_entry_pending_key = -1;
static int text_entry_pending_tap_index = 0;
static lv_timer_t * text_entry_multitap_timer;

/* The 10 T9 pad digit-slot keys, indexed by digit 0-9 -- kept so
 * text_entry_refresh_keys() can update their images/visibility in place
 * when the mode/shift/numeric-lock state changes, without rebuilding the
 * screen. Index 0 is special: it shares its screen position with Shift
 * (see text_entry_key0_click_cb's own comment). The numeric-lock "./-" key
 * and the three always-visible mode-jump buttons (123/ABC/sym) each need
 * their own visibility toggled independently of the digit-slot keys, so
 * they get their own named pointers rather than living in this array. */
static lv_obj_t * text_entry_key_img[10];
static lv_obj_t * text_entry_dotneg_key;
static lv_obj_t * text_entry_num_mode_key;
static lv_obj_t * text_entry_abc_mode_key;
static lv_obj_t * text_entry_sym_mode_key;

/* Bumped on every show_text_entry() call -- lets the READY handler below
 * tell whether the caller's done-callback chained straight into ANOTHER
 * show_text_entry() (Wi-Fi manual SSID entry is the one place that does:
 * SSID entered -> immediately prompts for the password) versus a callback
 * that just saved a value and returned. Real-device bug report: manual SSID
 * entry never actually asked for the password. Root cause was a race, not a
 * missing call -- the chained show_text_entry() DID run and DID push the
 * password screen, but this handler's own nav_pop() (issued for the SSID
 * screen, right before invoking the callback) is an ANIMATED transition
 * (screen_transition_slide(), ~NAV_ANIM_TIME_MS), and its completion
 * callback unconditionally lv_screen_load()s the screen nav_pop() was
 * originally headed to once that animation finishes -- landing well after
 * the password screen had already been pushed on top of it, and snapping
 * straight back to the Wi-Fi list before the user ever got a real chance at
 * it. nav_push()'s own dedup guard (same screen object already on top of
 * the stack -> just reload, don't grow the stack) means comparing nav_depth
 * before/after the callback can't detect this -- a chained call reuses this
 * same singleton text_entry_screen without changing nav_depth at all -- so
 * a dedicated generation counter is what actually distinguishes the two
 * cases. */
static uint32_t text_entry_generation = 0;

static void text_entry_reveal_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool now_masked = !lv_textarea_get_password_mode(text_entry_textarea);
    lv_textarea_set_password_mode(text_entry_textarea, now_masked);
    lv_image_set_src(text_entry_reveal_btn, asset_path(now_masked ? "keyboard/psk_show.png" : "keyboard/psk_hide.png"));
}

/* Extracted from what used to be lv_keyboard's own LV_EVENT_READY handler
 * (the T9 Enter key below just calls this directly) -- the chained-
 * show_text_entry()/nav_remove_stack_slot() logic is unrelated to the
 * keypad rewrite, so it's preserved verbatim rather than re-derived. */
static void text_entry_commit(void) {
    /* lv_textarea_get_text()'s buffer belongs to the textarea and would be
     * invalidated/reused the moment another screen starts editing it, so
     * copy it out before invoking the caller's callback (which may itself
     * push a screen that reuses this same textarea, e.g. entering the next
     * field). */
    char text_copy[256];
    snprintf(text_copy, sizeof(text_copy), "%s", lv_textarea_get_text(text_entry_textarea));
    text_entry_done_cb_t cb = text_entry_on_done;
    void * user_data = text_entry_user_data;
    uint32_t generation_before = text_entry_generation;
    int depth_before = nav_depth;
    /* Callback runs BEFORE nav_pop() specifically so the checks below can
     * see whether it navigated anywhere on its own -- see
     * text_entry_generation's own comment. Real-device bug report (Wi-Fi
     * manual SSID entry): the generation check alone wasn't enough -- it
     * only catches a callback that chains into ANOTHER show_text_entry()
     * call reusing this same singleton screen (nav_depth unchanged, dedup
     * guard in nav_push() eats the push). Wi-Fi's password-entered callback
     * instead calls start_wifi_connect(), which nav_push()es a DIFFERENT
     * screen (subsonic_downloading_screen, "Connecting to X...") straight
     * off this one -- nav_depth DOES grow there, since that push isn't a
     * dedup no-op. Confirmed live: the connecting screen flashed up then was
     * animated straight back to the password field by nav_pop() a moment
     * later, looking like nothing happened -- each repeat tap on the
     * keyboard's OK button (six of them, one real device test) genuinely
     * re-ran the whole callback, creating a fresh duplicate saved network
     * every time (wifi_control_connect() always add_network()s a new entry,
     * see its own comment).
     *
     * Simply skipping nav_pop() in that case (an earlier version of this
     * fix) stopped the yank-back, but left THIS screen's own stack slot
     * sitting there permanently, one level below wherever the callback's
     * own push actually landed -- confirmed live via the Subsonic download
     * path's identical shape (see poll_subsonic_download()'s own comment):
     * backing out of the destination screen afterward surfaced this
     * now-defunct form again instead of whatever was open before it, since
     * nothing had ever removed its slot. nav_remove_stack_slot() splices it
     * out after the fact once it's clear something else already took its
     * place. */
    if (cb) cb(text_copy, user_data);
    if (nav_depth > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else if (text_entry_generation == generation_before) {
        nav_pop();
    }
    /* else (nav_depth == depth_before && generation changed): the callback
     * chained into another show_text_entry() call reusing this exact screen
     * object -- nav_push()'s own dedup guard already reloaded it in place,
     * nothing further to do here. */
}

/* Clears any in-progress multi-tap cycle without touching the textarea --
 * called at the top of every OTHER key's handler (anything that isn't a
 * repeat tap of the same cycling key) so the next tap on that key starts a
 * fresh cycle instead of continuing whatever was pending before. Also what
 * the timeout itself does when no further tap arrives in time. */
static void text_entry_finalize_pending(void) {
    text_entry_pending_key = -1;
    lv_timer_pause(text_entry_multitap_timer);
}

static void text_entry_multitap_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    text_entry_finalize_pending();
}

/* Every character (or character group) a given digit-slot key currently
 * produces, as a NUL-terminated string -- length 1 means "just insert this
 * one character, no cycling" (NUM mode, or ABC mode's key 0/1, or a
 * genuinely single-symbol group); length > 1 is what the multi-tap cycle in
 * text_entry_key_click_cb() steps through. key_index 10 is the
 * numeric-lock-only "./-" key, meaningful only when text_entry_numeric_only
 * is set (its caller already knows not to ask otherwise). out must be at
 * least 8 bytes -- the longest real group (TEXT_ENTRY_SYMBOL_GROUPS' 4-char
 * entries) plus the NUL. uppercase is passed in explicitly (rather than read
 * live off text_entry_is_uppercase()) so text_entry_handle_digit_key() can
 * pin a multi-tap cycle's case to whatever it was on that cycle's first tap
 * -- see text_entry_pending_shift's own comment. */
static void text_entry_key_chars_ex(int key_index, char * out, size_t out_size, bool uppercase) {
    if (key_index == 10) {
        snprintf(out, out_size, ".-");
        return;
    }
    if (text_entry_numeric_only || text_entry_kp_mode == TEXT_ENTRY_KP_NUM) {
        out[0] = (char) ('0' + key_index);
        out[1] = '\0';
        return;
    }
    if (text_entry_kp_mode == TEXT_ENTRY_KP_SYM) {
        snprintf(out, out_size, "%s", TEXT_ENTRY_SYMBOL_GROUPS[key_index]);
        return;
    }
    /* ABC mode. Key 1 has no letter group on this asset set -- real-device
     * photo confirmed the stock keyboard repurposes it as a punctuation
     * shortcut here specifically (char_l.png/char_u.png's own baked-in
     * glyph), not literal "1" the way NUM mode's key 1 is. */
    if (key_index == 1) {
        snprintf(out, out_size, "._@/#");
        return;
    }
    const char * letters = TEXT_ENTRY_LETTER_GROUPS[key_index];
    if (!letters) {
        out[0] = (char) ('0' + key_index);
        out[1] = '\0';
        return;
    }
    size_t n = strlen(letters);
    size_t i;
    for (i = 0; i < n && i + 1 < out_size; i++) {
        out[i] = uppercase ? (char) toupper((unsigned char) letters[i]) : letters[i];
    }
    out[i] = '\0';
}

/* asset_path() only needs relative_path for the duration of its own call
 * (it strdup()s its own "S:..." result -- see its doc comment), so handing
 * back a reused static buffer here is safe: every call site below both
 * builds and consumes the path synchronously, on the GUI thread, one at a
 * time. */
static const char * text_entry_key_image(int key_index) {
    static char buf[64];
    if (text_entry_numeric_only || text_entry_kp_mode == TEXT_ENTRY_KP_NUM) {
        snprintf(buf, sizeof(buf), "keyboard/%d.png", key_index);
    } else if (text_entry_kp_mode == TEXT_ENTRY_KP_SYM) {
        snprintf(buf, sizeof(buf), "keyboard/symbol%d.png", key_index);
    } else if (key_index == 1) {
        /* char_l.png/char_u.png are byte-identical (confirmed via md5sum),
         * so this always resolves to the same image regardless of case --
         * picking by case anyway just for symmetry with every other
         * letter-group key below, which does need it. */
        snprintf(buf, sizeof(buf), "keyboard/char_%c.png", text_entry_is_uppercase() ? 'u' : 'l');
    } else {
        const char * letters = TEXT_ENTRY_LETTER_GROUPS[key_index];
        if (!letters) {
            snprintf(buf, sizeof(buf), "keyboard/%d.png", key_index);
        } else {
            snprintf(buf, sizeof(buf), "keyboard/%s_%c.png", letters, text_entry_is_uppercase() ? 'u' : 'l');
        }
    }
    return buf;
}

/* Re-syncs every key's image and (for the mode buttons/numeric-lock "./-"
 * key) visibility with the current mode/shift/numeric-lock state -- called
 * once from show_text_entry() and again on every mode/Shift/digit-0 tap. */
static void text_entry_refresh_keys(void) {
    for (int i = 1; i < 10; i++) {
        lv_image_set_src(text_entry_key_img[i], asset_path(text_entry_key_image(i)));
    }
    /* Index 0's slot shows Shift while cycling letters (case has no other
     * meaning), and the literal digit-0/symbol-0 key otherwise -- see
     * text_entry_key0_click_cb's own comment for the input-side half of
     * this same split. */
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        lv_image_set_src(text_entry_key_img[0], asset_path("keyboard/upper.png"));
    } else {
        lv_image_set_src(text_entry_key_img[0], asset_path(text_entry_key_image(0)));
    }

    if (text_entry_numeric_only) {
        lv_obj_add_flag(text_entry_num_mode_key, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(text_entry_abc_mode_key, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(text_entry_sym_mode_key, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(text_entry_dotneg_key, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(text_entry_dotneg_key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(text_entry_num_mode_key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(text_entry_abc_mode_key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(text_entry_sym_mode_key, LV_OBJ_FLAG_HIDDEN);
}

/* Shared by every cycling key (the 9 digit-slot keys, the key-0/digit-0
 * slot when it isn't acting as Shift, and the numeric-lock "./-" key) --
 * pulled out of the click handler so text_entry_key0_click_cb below can
 * reuse it without duplicating the multi-tap bookkeeping. A tap on a key
 * whose current character group has more than one character, repeated on
 * the SAME key before text_entry_multitap_timer elapses, cycles to the next
 * character in that group instead of inserting a new one (classic phone-
 * keypad multi-tap: key 2 pressed twice quickly types "b", not "aa"). Any
 * other case (a new key, a single-character group, or the timer having
 * already elapsed) just inserts the group's first character fresh.
 *
 * Case is decided once, on the FIRST tap of a fresh cycle (same_key_pending
 * false), and then pinned in text_entry_pending_shift for any further taps
 * that just cycle through candidates for that SAME letter -- see that
 * variable's own comment. That first tap is also where a one-shot Shift
 * (text_entry_shift, as opposed to the persistent text_entry_caps_lock) gets
 * consumed: it already did its job baking uppercase into chars[] above, so
 * it's cleared right after, letting the very next DIFFERENT key fall back to
 * whatever text_entry_caps_lock alone says. */
static void text_entry_handle_digit_key(int key_index) {
    bool same_key_pending = (text_entry_pending_key == key_index);
    bool uppercase = same_key_pending ? text_entry_pending_shift : text_entry_is_uppercase();

    char chars[8];
    text_entry_key_chars_ex(key_index, chars, sizeof(chars), uppercase);
    size_t n = strlen(chars);

    if (n > 1 && same_key_pending) {
        lv_textarea_delete_char(text_entry_textarea);
        text_entry_pending_tap_index = (text_entry_pending_tap_index + 1) % (int) n;
    } else {
        text_entry_pending_key = n > 1 ? key_index : -1;
        text_entry_pending_tap_index = 0;
        text_entry_pending_shift = uppercase;
        if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC && text_entry_shift) {
            text_entry_shift = false;
            text_entry_refresh_keys();
        }
    }
    lv_textarea_add_char(text_entry_textarea, (uint32_t) chars[text_entry_pending_tap_index]);

    if (n > 1) {
        lv_timer_reset(text_entry_multitap_timer);
        lv_timer_resume(text_entry_multitap_timer);
    }
}

static void text_entry_key_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_handle_digit_key((int) (intptr_t) lv_event_get_user_data(e));
}

/* Key index 0's screen slot (column 4, row 1) does double duty, matching
 * the real stock keyboard's own layout: while cycling letters, tapping it
 * arms a one-shot Shift for the next letter typed (text_entry_is_uppercase()'s
 * own comment) -- there's no digit "0" needed there since ABC mode has no
 * use for one. In NUM/SYM mode (or numeric_only, which behaves like NUM mode
 * regardless of text_entry_kp_mode's stale value -- see text_entry_key_chars_ex()'s
 * own numeric_only check) it's a completely ordinary cycling digit-slot key
 * for index 0, same as any other. */
static void text_entry_key0_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        text_entry_finalize_pending();
        text_entry_shift = !text_entry_shift;
        text_entry_refresh_keys();
        return;
    }
    text_entry_handle_digit_key(0);
}

/* The three mode buttons stacked in column 0 (rows 0-2) are always visible
 * together and each jump straight to their own mode -- unlike the earlier
 * single cycling Mode key this replaces, matching a real-device photo of
 * the stock keyboard's own layout exactly. */
static void text_entry_mode_num_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    text_entry_kp_mode = TEXT_ENTRY_KP_NUM;
    text_entry_shift = false; /* one-shot Shift is meaningless outside ABC -- don't let it carry back in */
    text_entry_refresh_keys();
}

/* Real-device bug report: tapping ABC again while it was ALREADY the active
 * mode did nothing -- the real stock keyboard uses exactly that (re-tapping
 * the already-active mode button) as its persistent Caps Lock toggle,
 * distinct from Shift's one-shot-next-letter behavior on the key-0 slot
 * above. Switching INTO ABC from NUM/SYM just activates it, same as before;
 * text_entry_caps_lock deliberately isn't reset there, so a caps-lock
 * preference set earlier survives a detour through NUM/SYM and back. */
static void text_entry_mode_abc_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        text_entry_caps_lock = !text_entry_caps_lock;
        text_entry_shift = false; /* toggling caps lock supersedes any pending one-shot arm */
    } else {
        text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    }
    text_entry_refresh_keys();
}

static void text_entry_mode_sym_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    text_entry_kp_mode = TEXT_ENTRY_KP_SYM;
    text_entry_shift = false; /* one-shot Shift is meaningless outside ABC -- don't let it carry back in */
    text_entry_refresh_keys();
}

static void text_entry_del_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_delete_char(text_entry_textarea);
}

static void text_entry_left_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_cursor_left(text_entry_textarea);
}

static void text_entry_right_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_cursor_right(text_entry_textarea);
}

static void text_entry_space_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_add_char(text_entry_textarea, ' ');
}

/* Defined in the search-binding section below (dismisses whichever
 * library screen currently has the keypad on loan for inline search --
 * see text_entry_inline_mode_active's own comment). Forward-declared here
 * since it's this file's only reference to search state from within the
 * text-entry section, which comes first. */
static void search_dismiss_active_keypad(void);
static void search_textarea_value_changed_cb(lv_event_t * e);

static void text_entry_enter_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    if (text_entry_inline_mode_active) {
        search_dismiss_active_keypad();
        return;
    }
    text_entry_commit();
}

/* Every key is a plain clickable image at its own top-left grid cell (col,
 * row) -- see the icon_tile_press_event_cb doc comment in screen_builders.c
 * for the precedent this follows (clickable lv_image, no wrapper container).
 * Taller/wider keys (Enter, Space) just have a native asset that extends
 * past one cell; their top-left anchor is computed the same way as every
 * other key. */
static lv_obj_t * text_entry_make_key(lv_obj_t * scr, int col, int row, lv_event_cb_t cb, void * user_data) {
    lv_obj_t * img = lv_image_create(scr);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT,
                 TEXT_ENTRY_GRID_X + col * (TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP),
                 TEXT_ENTRY_GRID_Y + row * (TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP));
    if (cb) lv_obj_add_event_cb(img, cb, LV_EVENT_CLICKED, user_data);
    return img;
}

/* T9 keypad: 5 columns x 4 rows (see TEXT_ENTRY_GRID_X/Y's own comment
 * above for the real-device-photo source of this layout). Row 0: Mode:123
 * / 1 / 2-ABC / 3-DEF / Del. Row 1: Mode:ABC / 4-GHI / 5-JKL / 6-MNO /
 * 0-or-Shift. Row 2: Mode:sym / 7-PQRS / 8-TUV / 9-WXYZ / Enter (spans
 * rows 2-3). Row 3: Left / Right / Space (spans cols 2-3) / Enter
 * continues. The numeric-lock "./-" key (numeric_only fields only)
 * occupies the same cell as Mode:123, since the three mode buttons are
 * hidden together whenever it's shown -- see text_entry_refresh_keys().
 *
 * Every key is built as a child of one `group` container (itself a plain
 * full-screen-sized, invisible object at (0,0)) rather than directly on
 * `parent`, so the whole keypad can be reparented in one
 * lv_obj_set_parent(group, ...) call -- see t9_keypad_attach()/
 * t9_keypad_release() -- instead of moving each key individually. Every
 * screen this ever attaches to is the same 480x800 full-screen size, so
 * TEXT_ENTRY_GRID_X/Y's absolute-pixel positioning (already fixed
 * relative to `group`'s own (0,0) origin) lands identically regardless of
 * which screen currently owns it. */
static lv_obj_t * build_t9_keypad_group(lv_obj_t * parent) {
    lv_obj_t * group = lv_obj_create(parent);
    lv_obj_set_size(group, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(group, 0, 0);
    lv_obj_set_style_bg_opa(group, 0, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    /* lv_obj_create()'s default theme padding shifts every key's TOP_LEFT-
     * aligned TEXT_ENTRY_GRID_X/Y position (and the black backing rect
     * below) inward from group's true left/top edge -- real-device
     * feedback: the keypad's first column didn't actually reach the
     * screen's left border despite TEXT_ENTRY_GRID_X computing to ~1px.
     * Same root cause already documented on the search bar's own padding. */
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_CLICKABLE);

    /* Opaque black backing sized to the keypad's own footprint (plus a
     * small margin), created before the keys so it sits behind them --
     * real-device feedback: the keys themselves have gaps between them
     * (TEXT_ENTRY_KEY_GAP), and `group` above is fully transparent, so
     * without this whatever's on the target screen behind the keypad
     * (list rows, etc.) showed through those gaps instead of solid black. */
    lv_obj_t * backing = lv_obj_create(group);
    lv_obj_set_size(backing, lv_pct(100), TEXT_ENTRY_GRID_HEIGHT + TEXT_ENTRY_BOTTOM_MARGIN + 8);
    lv_obj_set_pos(backing, 0, TEXT_ENTRY_GRID_Y - 8);
    lv_obj_set_style_bg_opa(backing, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(backing, lv_color_black(), 0);
    lv_obj_set_style_border_width(backing, 0, 0);
    lv_obj_set_style_radius(backing, 0, 0);
    lv_obj_remove_flag(backing, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(backing, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 1; i <= 9; i++) {
        int col = 1 + (i - 1) % 3;
        int row = (i - 1) / 3;
        text_entry_key_img[i] = text_entry_make_key(group, col, row, text_entry_key_click_cb, (void *) (intptr_t) i);
    }
    text_entry_key_img[0] = text_entry_make_key(group, 4, 1, text_entry_key0_click_cb, NULL);

    text_entry_dotneg_key = text_entry_make_key(group, 0, 0, text_entry_key_click_cb, (void *) (intptr_t) 10);
    lv_image_set_src(text_entry_dotneg_key, asset_path("keyboard/dot.png"));

    text_entry_num_mode_key = text_entry_make_key(group, 0, 0, text_entry_mode_num_click_cb, NULL);
    lv_image_set_src(text_entry_num_mode_key, asset_path("keyboard/num.png"));
    text_entry_abc_mode_key = text_entry_make_key(group, 0, 1, text_entry_mode_abc_click_cb, NULL);
    lv_image_set_src(text_entry_abc_mode_key, asset_path("keyboard/char.png"));
    text_entry_sym_mode_key = text_entry_make_key(group, 0, 2, text_entry_mode_sym_click_cb, NULL);
    lv_image_set_src(text_entry_sym_mode_key, asset_path("keyboard/symbol.png"));

    lv_obj_t * del_key = text_entry_make_key(group, 4, 0, text_entry_del_click_cb, NULL);
    lv_image_set_src(del_key, asset_path("keyboard/del.png"));
    lv_obj_t * left_key = text_entry_make_key(group, 0, 3, text_entry_left_click_cb, NULL);
    lv_image_set_src(left_key, asset_path("keyboard/left.png"));
    lv_obj_t * right_key = text_entry_make_key(group, 1, 3, text_entry_right_click_cb, NULL);
    lv_image_set_src(right_key, asset_path("keyboard/right.png"));
    lv_obj_t * enter_key = text_entry_make_key(group, 4, 2, text_entry_enter_click_cb, NULL);
    lv_image_set_src(enter_key, asset_path("keyboard/enter.png"));
    lv_obj_t * space_key = text_entry_make_key(group, 2, 3, text_entry_space_click_cb, NULL);
    lv_image_set_src(space_key, asset_path("keyboard/space2.png"));

    text_entry_multitap_timer = lv_timer_create(text_entry_multitap_timeout_cb, TEXT_ENTRY_MULTITAP_MS, NULL);
    lv_timer_pause(text_entry_multitap_timer);

    /* Lets a swipe started on the keypad (inline search's Enter key sits
     * right where a right-swipe could plausibly start) still bubble up to
     * the target screen's own back-gesture handler -- built once here, so
     * it persists across every future t9_keypad_attach() reparenting
     * (flags are object properties, not derived from the parent). */
    lv_obj_add_flag(group, LV_OBJ_FLAG_GESTURE_BUBBLE);
    enable_gesture_bubble_recursive(group);

    return group;
}

/* Reparents the shared keypad_group + text_entry_textarea onto
 * target_screen for inline search -- see text_entry_inline_mode_active's
 * own comment. Resets mode/shift/caps-lock/numeric state and clears the
 * textarea the same way show_text_entry() resets it for a fresh modal
 * field, since this is the same shared state machine. Search never needs
 * the password reveal button -- it stays on text_entry_screen, harmless
 * since that screen isn't the active one while this is attached elsewhere. */
static void t9_keypad_attach(lv_obj_t * target_screen, lv_obj_t * textarea_parent, int32_t textarea_x, int32_t textarea_y,
                              int32_t textarea_w) {
    lv_obj_set_parent(text_entry_keypad_group, target_screen);
    lv_obj_set_parent(text_entry_textarea, textarea_parent);
    /* lv_obj_align() (used to position this textarea for the modal flow,
     * see t9_keypad_release() below) sets a PERSISTENT align-mode style
     * property, not just a one-off position -- plain lv_obj_set_pos() here
     * left that mode at its build-time LV_ALIGN_TOP_MID, so textarea_x/y
     * were being applied as an offset from horizontal CENTER of whatever
     * the current parent is, not as an absolute top-left position.
     * Real-device symptom: the textarea rendered ~38px further right than
     * intended and visibly overlapped close_btn. lv_obj_align() with
     * TOP_LEFT here resets the mode so these coordinates are absolute. */
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_LEFT, textarea_x, textarea_y);
    lv_obj_set_width(text_entry_textarea, textarea_w);
    lv_textarea_set_password_mode(text_entry_textarea, false);
    lv_textarea_set_text(text_entry_textarea, "");

    text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    text_entry_shift = false;
    text_entry_caps_lock = false;
    text_entry_numeric_only = false;
    text_entry_finalize_pending();
    text_entry_refresh_keys();
    text_entry_generation++;
    text_entry_inline_mode_active = true;
}

/* Returns keypad_group + text_entry_textarea to text_entry_screen, their
 * resting parent when neither the modal flow nor inline search is using
 * them -- called both when inline search fully closes and defensively at
 * the start of show_text_entry() (see its own comment) so opening the
 * modal always reclaims them correctly regardless of which screen
 * currently holds them. */
static void t9_keypad_release(void) {
    lv_obj_set_parent(text_entry_keypad_group, text_entry_screen);
    lv_obj_set_parent(text_entry_textarea, text_entry_screen);
    lv_obj_set_size(text_entry_textarea, lv_pct(78), 50);
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + 40);
    text_entry_inline_mode_active = false;
}

static lv_obj_t * build_text_entry_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* lv_keyboard_create()'s built-in Close/X key used to be this screen's
     * only cancel path (see the old LV_EVENT_CANCEL branch this replaces --
     * generic_back_cb below is functionally identical, just nav_pop()).
     * Removing lv_keyboard for the T9 keypad below means that key is gone,
     * so this screen needs its own explicit back button now -- same
     * top-left 64x64/sub_back/btn_back.png pattern as build_files_screen()
     * and every other hand-built screen in this file. */
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

    text_entry_title_label = lv_label_create(scr);
    lv_label_set_text(text_entry_title_label, "");
    lv_obj_align(text_entry_title_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + 8);
    lv_obj_add_style(text_entry_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(text_entry_title_label, ui_size_16, 0);

    text_entry_textarea = lv_textarea_create(scr);
    lv_obj_set_size(text_entry_textarea, lv_pct(78), 50);
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + 40);
    lv_textarea_set_one_line(text_entry_textarea, true);
    /* Built once here, so it persists across every future t9_keypad_attach()
     * reparenting onto a library screen's own search bar -- lets a swipe
     * started on the textarea itself still bubble up to that screen's
     * back-gesture handler (see search_close_if_active_for_screen()). */
    lv_obj_add_flag(text_entry_textarea, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* Fires on every keystroke (lv_textarea's own native behavior --
     * nothing listened for this before inline search existed, since every
     * other text_entry_textarea use is one-shot-on-Enter via
     * text_entry_commit()). search_textarea_value_changed_cb() itself
     * no-ops unless text_entry_inline_mode_active is set, so this is inert
     * for all 9 existing modal callers. */
    lv_obj_add_event_cb(text_entry_textarea, search_textarea_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Real-device bug report: no way to unmask a password field while
     * typing it. Hidden entirely for non-password fields by
     * show_text_entry() below -- the textarea is narrowed to make room for
     * it unconditionally so switching between password/non-password fields
     * doesn't reflow the textarea's width and position. */
    text_entry_reveal_btn = lv_image_create(scr);
    lv_image_set_src(text_entry_reveal_btn, asset_path("keyboard/psk_show.png"));
    lv_obj_align_to(text_entry_reveal_btn, text_entry_textarea, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_add_flag(text_entry_reveal_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(text_entry_reveal_btn, text_entry_reveal_btn_cb, LV_EVENT_CLICKED, NULL);

    text_entry_keypad_group = build_t9_keypad_group(scr);

    return scr;
}

/* Pushes the text entry screen pre-filled with initial_text (may be NULL
 * for empty) under `title`; on_done fires with whatever was typed when the
 * T9 keypad's Enter key is pressed (not called at all if the screen's own
 * back button is used instead -- callers shouldn't assume it always fires).
 * is_password masks the input the same way a real password field would.
 * numeric locks the keypad to plain-digit entry (see text_entry_numeric_
 * only's own comment) instead of the normal ABC/NUM/SYM T9 cycle --
 * everything peq.c's freq/gain/Q fields need (digits, a decimal point, and
 * a minus sign for negative gain), nothing else. */
static void show_text_entry(const char * title, const char * initial_text, bool is_password, bool numeric,
                             text_entry_done_cb_t on_done, void * user_data) {
    /* Defensive reclaim -- if inline search happened to be holding the
     * shared keypad/textarea when some other flow (e.g. a Wi-Fi password
     * prompt) opens the modal, this puts them back on text_entry_screen
     * (and restores the textarea's modal position/size) before anything
     * below touches them. Harmless no-op when they're already there. */
    t9_keypad_release();

    lv_label_set_text(text_entry_title_label, title);
    lv_textarea_set_text(text_entry_textarea, initial_text ? initial_text : "");
    lv_textarea_set_password_mode(text_entry_textarea, is_password);
    /* Always reset to masked/hidden -- otherwise a field left revealed by
     * the previous show_text_entry() call (e.g. Wi-Fi's chained SSID ->
     * password, which reuses this same singleton screen without an
     * intervening rebuild) would carry that state into an unrelated field. */
    lv_image_set_src(text_entry_reveal_btn, asset_path("keyboard/psk_show.png"));
    if (is_password) {
        lv_obj_remove_flag(text_entry_reveal_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(text_entry_reveal_btn, LV_OBJ_FLAG_HIDDEN);
    }
    text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    text_entry_shift = false;
    text_entry_caps_lock = false;
    text_entry_numeric_only = numeric;
    text_entry_finalize_pending();
    text_entry_refresh_keys();
    text_entry_on_done = on_done;
    text_entry_user_data = user_data;
    text_entry_generation++;
    nav_push(text_entry_screen);
}

/* ---- Subsonic-compatible network streaming ----
 *
 * mp3/flac songs (the two decoders audio.c can open against a true network
 * stream -- see decoder_open()'s own comment in audio.c) play directly off
 * the server via subsonic_song_row_click_cb() below, no local file at all.
 * Every other format (aac/ogg/wma/m4a/ape/opus/wav/aiff/dsf -- whatever
 * this server's own transcoding settings hand back) still downloads the
 * whole track first via the mechanism right below, before handing it to the
 * existing local-file-only decoder for that format: retrofitting every
 * remaining decoder here (the vendored AIFF/DSD/AAC/ALAC/APE ones, all
 * built around a plain seekable FILE*) for streaming reads is a much bigger
 * project than this round's scope. The downloaded copy lands on the SD card
 * (SUBSONIC_STREAM_CACHE_DIR below), not /tmp -- see that macro's own
 * comment for why. */

static subsonic_server_t subsonic_server_from_settings(void) {
    subsonic_server_t server;
    snprintf(server.base_url, sizeof(server.base_url), "%s", current_settings.subsonic_url);
    snprintf(server.username, sizeof(server.username), "%s", current_settings.subsonic_username);
    snprintf(server.password, sizeof(server.password), "%s", current_settings.subsonic_password);
    server.verify_tls = current_settings.subsonic_verify_tls;
    return server;
}

/* Background download thread + a "done" flag polled from update_timer_cb --
 * the same shape as audio.c's own track_finished flag, for the same reason:
 * LVGL isn't thread-safe, so the thread can't touch any screen/widget
 * state directly. */
static pthread_t download_thread;
static bool download_active = false;
static volatile bool download_done_flag = false;
static volatile bool download_success_flag = false;
static char download_dest_path[512];

typedef struct {
    char url[1536];
    char dest_path[512];
    bool verify_tls;
} download_request_t;

static void * download_thread_func(void * arg) {
    download_request_t * req = (download_request_t *) arg;
    bool ok = http_get_to_file(req->url, req->verify_tls, req->dest_path, NULL, NULL);
    download_success_flag = ok;
    download_done_flag = true; /* written last -- update_timer_cb only checks this flag */
    free(req);
    return NULL;
}

static lv_obj_t * subsonic_downloading_screen;
static lv_obj_t * subsonic_downloading_label;
static lv_obj_t * subsonic_downloading_progress_bar;
static void start_subsonic_download(const char * url, bool verify_tls, const char * dest_path, const char * display_title) {
    snprintf(download_dest_path, sizeof(download_dest_path), "%s", dest_path);

    download_request_t * req = malloc(sizeof(*req));
    snprintf(req->url, sizeof(req->url), "%s", url);
    snprintf(req->dest_path, sizeof(req->dest_path), "%s", dest_path);
    req->verify_tls = verify_tls;

    download_done_flag = false;
    download_success_flag = false;
    download_active = true;

    lv_label_set_text_fmt(subsonic_downloading_label, "Downloading\n%s...", display_title);
    nav_push(subsonic_downloading_screen);

    pthread_create(&download_thread, NULL, download_thread_func, req);
}

static void poll_subsonic_download(void) {
    if (!download_active || !download_done_flag) return;

    download_active = false;
    pthread_join(download_thread, NULL);
    bool success = download_success_flag;

    /* Real-device bug report: a downloaded Subsonic track needed a second
     * tap on the song to actually start playing -- same race already fixed
     * for Wi-Fi manual SSID entry (see text_entry_kb_event_cb's own comment
     * for the full mechanism). on_file_selected() -> play_track_at_from()
     * nav_push()es the player screen the instant playback starts; nav_pop()
     * below (leaving the "Downloading..." screen) is an ANIMATED transition
     * queued via screen_transition_slide(), whose completion callback
     * unconditionally lv_screen_load()s back to the song list once that
     * animation finishes, a moment after the player screen was already
     * showing -- yanking the UI back even though audio was, underneath,
     * genuinely already playing.
     *
     * Real-device follow-up: simply skipping nav_pop() when on_file_
     * selected() already navigated (an earlier version of this fix) did
     * stop the yank-back, but left subsonic_downloading_screen's own slot
     * sitting in the stack forever, one level below the player screen --
     * confirmed live as backing out of the player landing back on the now-
     * defunct "Downloading..." screen instead of the song list underneath
     * it. nav_remove_stack_slot() (see its own comment) splices that slot
     * out once it's clear the player screen already took its place,
     * rather than trying to avoid creating it in the first place -- the
     * push already happened by the time this code can tell whether it
     * did. */
    int depth_before = nav_depth;
    if (success) {
        char ** playlist = malloc(sizeof(char *));
        playlist[0] = strdup(download_dest_path);
        clear_player_source(); /* a streamed-then-downloaded single track has no on-device list to go back to */
        on_file_selected(playlist, 1, 0);
    }
    if (nav_depth > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else {
        nav_pop(); /* leave the downloading screen */
    }
    /* else (download failed, or play_track_at_from() bailed out early via
     * external_dac_block_reason() without navigating): silently stays
     * wherever nav_pop() landed -- no error-toast UI exists yet to explain
     * a failed download (network drop, server error mid-stream, disk
     * full, ...), same gap already noted for subsonic_connect_row_cb
     * above. */
}

/* DLNA/UPnP-AV cast-and-play -- see dlna_control.h for the full mechanism
 * (dmrd relays SetAVTransportURI/Play/Stop over an undocumented socket;
 * this module downloads the cast URL and hands it to the normal local
 * playback pipeline, same download-then-play shape as Subsonic streaming
 * just above, and the same reason: no decoder here reads a true network
 * stream). Relies on the downloaded file's own embedded tags for Now
 * Playing display via the standard on_file_selected() path, same as
 * Subsonic, rather than the richer DIDL-Lite title/artist/album
 * dlna_control_consume_ready_track() also provides -- real casts observed
 * during this feature's investigation had matching embedded tags, and
 * threading an override path through apply_track_metadata_to_ui() for a
 * case that may not come up in practice wasn't worth the added risk this
 * round. No "please wait" screen the way subsonic downloads get one --
 * a cast is user-initiated from another device, not from a tap on this
 * screen, so there's no local navigation context to hold open while it
 * downloads. */
static void poll_dlna_control(void) {
    char path[512], title[256], artist[256], album[256];
    if (dlna_control_consume_ready_track(path, sizeof(path), title, sizeof(title),
                                          artist, sizeof(artist), album, sizeof(album))) {
        char ** playlist = malloc(sizeof(char *));
        playlist[0] = strdup(path);
        clear_player_source(); /* cast from another device -- no on-device list to go back to */
        on_file_selected(playlist, 1, 0);
    }
    if (dlna_control_consume_stop_requested()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    dlna_control_notify_status(audio_is_playing(), audio_is_paused());
}

/* Shared "please wait" interstitial for background ops that still use it
 * (subsonic downloads/connects, wifi connect, library rescan) -- an
 * in-flight download/connect/rescan isn't safely cancelable (partial file,
 * half-applied network config, ...), so this has no way back, by design.
 * Wi-Fi/Bluetooth scans and the Bluetooth power toggle used to share this
 * same screen too (with a cancel button, since abandoning a scan is safe),
 * but that was the source of a real, repeatedly-hit stuck-screen bug
 * (multiple uncoordinated users of one shared overlay/button, plus the
 * overlay getting buried under further navigation) -- see git history for
 * that mechanism if it's ever needed again. Those three now run fully in
 * the background with no screen of their own at all, matching the stock
 * player; this interstitial and its (now permanently hidden, nothing sets
 * it) cancel button are what's left for the remaining, genuinely
 * non-cancelable uses. */
static lv_obj_t * build_subsonic_downloading_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    subsonic_downloading_label = lv_label_create(scr);
    lv_label_set_text(subsonic_downloading_label, "");
    lv_obj_add_style(subsonic_downloading_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_align(subsonic_downloading_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(subsonic_downloading_label, LV_ALIGN_CENTER, 0, -20);

    /* Only the library-rescan flow (start_library_rescan()/poll_library_
     * rescan()) ever shows this -- the other reuses of this shared "please
     * wait" screen (subsonic download/connect, wifi connect) don't have a
     * meaningful total to report progress against, so it stays hidden
     * unless a rescan explicitly shows it, and gets hidden again as soon as
     * that rescan's success message is dismissed. */
    subsonic_downloading_progress_bar = lv_bar_create(scr);
    lv_obj_set_size(subsonic_downloading_progress_bar, 280, 14);
    lv_obj_align(subsonic_downloading_progress_bar, LV_ALIGN_CENTER, 0, 30);
    lv_bar_set_range(subsonic_downloading_progress_bar, 0, 100);
    lv_obj_add_style(subsonic_downloading_progress_bar, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_flag(subsonic_downloading_progress_bar, LV_OBJ_FLAG_HIDDEN);

    return scr;
}

/* ---- "Download to library" -- Subsonic screen redesign. Unlike the
 * single-song download-then-play flow just above (which caches into
 * SUBSONIC_STREAM_CACHE_DIR and is never meant to persist), this
 * permanently saves every song in an artist/album/playlist under
 * MUSIC_ROOT_DIR itself, laid out the same way a manually-ripped library
 * usually is: <Artist>/<Album>/<track> - <title>.<ext>. One background
 * thread does N sequential HTTP downloads (http_get_to_file() blocks per
 * file, same as the single-song path above), then a normal
 * start_library_rescan() so the new files actually show up in Artists/
 * Albums/All Songs without a separate manual "Update Music Database" step.
 * ---- */

/* FAT32/exFAT can't hold '/', and the rest of this set covers characters
 * various filesystems/tools choke on -- artist/album/song names from a real
 * server are free-form text from whoever tagged that library, unlike every
 * other path this app builds internally out of known-safe components. */
static void sanitize_path_component(const char * in, char * out, size_t out_size) {
    size_t pos = 0;
    for (const char * p = in; *p && pos + 1 < out_size; p++) {
        char c = *p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
        out[pos++] = c;
    }
    out[pos] = '\0';
}

typedef struct {
    subsonic_server_t server;
    /* Mode A (Album/Playlist download): songs/song_count are already fully
     * known -- an owned copy the thread frees when done. Mode B (Artist
     * download, from the Artist page's own Download button): songs is NULL
     * and albums_to_expand is set instead, since an artist's total song
     * count isn't known until the thread itself fetches each album's songs
     * first (subsonic_get_album_songs() per album, aggregated). */
    subsonic_song_t * songs;
    int song_count;
    subsonic_album_t * albums_to_expand;
    int album_to_expand_count;
    char playlist_name[128]; /* non-empty only for a Playlist download -- also creates a local .m3u alongside the downloaded files */
} subsonic_library_download_request_t;

static pthread_t subsonic_library_download_thread;
static bool subsonic_library_download_active = false;
static volatile bool subsonic_library_download_done_flag = false;
static volatile int subsonic_library_download_progress = 0; /* songs completed so far */
static volatile int subsonic_library_download_total = 0;    /* 0 while still expanding an artist's albums (Mode B) -- see poll_subsonic_library_download() */
static int subsonic_library_download_success_count = 0;

static void * subsonic_library_download_thread_func(void * arg) {
    subsonic_library_download_request_t * req = (subsonic_library_download_request_t *) arg;

    subsonic_song_t * songs = req->songs; /* Mode A: the caller's owned copy; Mode B: NULL, built below */
    int song_count = req->song_count;

    if (req->albums_to_expand) {
        int capacity = 0;
        int count = 0;
        for (int i = 0; i < req->album_to_expand_count; i++) {
            subsonic_song_t * album_songs = NULL;
            int album_song_count = 0;
            if (subsonic_get_album_songs(&req->server, req->albums_to_expand[i].id, &album_songs, &album_song_count)) {
                if (count + album_song_count > capacity) {
                    capacity = (count + album_song_count) * 2;
                    songs = realloc(songs, sizeof(subsonic_song_t) * (size_t) capacity);
                }
                memcpy(songs + count, album_songs, sizeof(subsonic_song_t) * (size_t) album_song_count);
                count += album_song_count;
            }
            free(album_songs);
        }
        song_count = count;
        subsonic_library_download_total = song_count; /* was 0 until this expansion finished -- unblocks poll_subsonic_library_download()'s progress display */
        free(req->albums_to_expand);
    }

    char playlist_m3u_path[512] = "";
    bool playlist_first = true;
    int success_count = 0;

    for (int i = 0; i < song_count; i++) {
        subsonic_song_t * song = &songs[i];

        char safe_artist[160], safe_album[160], safe_title[160];
        sanitize_path_component(song->artist[0] ? song->artist : "Unknown Artist", safe_artist, sizeof(safe_artist));
        sanitize_path_component(song->album[0] ? song->album : "Unknown Album", safe_album, sizeof(safe_album));
        sanitize_path_component(song->title[0] ? song->title : "Unknown Title", safe_title, sizeof(safe_title));

        char artist_dir[400], album_dir[400], dest_path[600];
        snprintf(artist_dir, sizeof(artist_dir), "%s/%s", MUSIC_ROOT_DIR, safe_artist);
        mkdir(artist_dir, 0755); /* no-op (EEXIST) if it's already there, same as every other mkdir() in this file */
        snprintf(album_dir, sizeof(album_dir), "%s/%s", artist_dir, safe_album);
        mkdir(album_dir, 0755);
        if (song->track > 0) {
            snprintf(dest_path, sizeof(dest_path), "%s/%02d - %s.%s", album_dir, song->track, safe_title, song->suffix);
        } else {
            snprintf(dest_path, sizeof(dest_path), "%s/%s.%s", album_dir, safe_title, song->suffix);
        }

        char url[1536];
        subsonic_build_stream_url(&req->server, song->id, url, sizeof(url));
        bool ok = http_get_to_file(url, req->server.verify_tls, dest_path, NULL, NULL);

        if (ok) {
            success_count++;
            if (req->playlist_name[0] != '\0') {
                if (playlist_first) {
                    playlist_files_create(PLAYLISTS_DIR, req->playlist_name, dest_path, playlist_m3u_path, sizeof(playlist_m3u_path));
                    playlist_first = false;
                } else if (playlist_m3u_path[0] != '\0') {
                    playlist_files_append(playlist_m3u_path, dest_path);
                }
            }
        }

        subsonic_library_download_progress = i + 1;
    }

    subsonic_library_download_success_count = success_count;
    free(songs);
    free(req);
    subsonic_library_download_done_flag = true; /* written last -- poll_subsonic_library_download() only checks this flag */
    return NULL;
}

/* songs/albums_to_expand are handed off to the thread, which frees them --
 * callers must pass owned, malloc'd copies, never a shared cache pointer
 * (subsonic_songs_cache/subsonic_albums_cache themselves can be replaced by
 * the next browse click while this download is still in flight). Exactly
 * one of songs or albums_to_expand should be non-NULL (Mode A vs Mode B --
 * see subsonic_library_download_request_t's own comment). playlist_name
 * NULL/empty means "don't create a .m3u." */
static void start_subsonic_library_download(subsonic_song_t * songs, int song_count,
                                              subsonic_album_t * albums_to_expand, int album_to_expand_count,
                                              const char * playlist_name, const char * progress_label) {
    subsonic_library_download_request_t * req = malloc(sizeof(*req));
    req->server = subsonic_server_from_settings();
    req->songs = songs;
    req->song_count = song_count;
    req->albums_to_expand = albums_to_expand;
    req->album_to_expand_count = album_to_expand_count;
    snprintf(req->playlist_name, sizeof(req->playlist_name), "%s", playlist_name ? playlist_name : "");

    subsonic_library_download_progress = 0;
    subsonic_library_download_total = albums_to_expand ? 0 : song_count; /* 0 = "still figuring out the total," see poll_subsonic_library_download() */
    subsonic_library_download_success_count = 0;
    subsonic_library_download_done_flag = false;
    subsonic_library_download_active = true;

    lv_label_set_text(subsonic_downloading_label, progress_label);
    lv_bar_set_value(subsonic_downloading_progress_bar, 0, LV_ANIM_OFF);
    if (albums_to_expand) {
        lv_obj_add_flag(subsonic_downloading_progress_bar, LV_OBJ_FLAG_HIDDEN); /* no meaningful total yet -- see the "0 while expanding" comment above */
    } else {
        lv_obj_remove_flag(subsonic_downloading_progress_bar, LV_OBJ_FLAG_HIDDEN);
    }
    nav_push(subsonic_downloading_screen);

    pthread_create(&subsonic_library_download_thread, NULL, subsonic_library_download_thread_func, req);
}

static void poll_subsonic_library_download(void) {
    if (!subsonic_library_download_active) return;

    if (!subsonic_library_download_done_flag) {
        int total = subsonic_library_download_total;
        if (total > 0) {
            lv_obj_remove_flag(subsonic_downloading_progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(subsonic_downloading_label, "Downloading to library...\n%d / %d",
                                   subsonic_library_download_progress, total);
            lv_bar_set_value(subsonic_downloading_progress_bar,
                              (subsonic_library_download_progress * 100) / total, LV_ANIM_OFF);
        }
        return;
    }

    subsonic_library_download_active = false;
    pthread_join(subsonic_library_download_thread, NULL);

    /* Leave this download's own use of the shared "please wait" screen
     * before either branch below -- start_library_rescan() pushes that same
     * screen again fresh for its own "Updating music database..." phase,
     * and would otherwise stack a second copy on top of this one still
     * sitting there. */
    nav_pop();

    if (subsonic_library_download_success_count > 0) {
        /* start_library_rescan() finishes with nav_reset_to_home(), same as
         * every other "the library just changed on disk" trigger in this
         * app (SD import, Wi-Fi import, manual rescan) -- no separate
         * success toast first, the label just switches straight from this
         * download's own progress text to the rescan's. */
        start_library_rescan();
    } else {
        show_error_toast("Download failed");
    }
}

/* Shared by the artists/albums/songs screens: a titled list of compact
 * rows, one per index-based click callback, built fresh from a persistent
 * (never-deleted) list container each time the user drills into a new
 * artist/album -- same "rebuild in place" approach as the local library's
 * group_songs_screen, since a real server library can have far too many
 * artists/albums/songs to pre-build a screen per one. */
static void populate_indexed_list(lv_obj_t * list, int count, const char * (*label_of)(int), lv_event_cb_t click_cb) {
    lv_obj_clean(list);
    for (int i = 0; i < count; i++) {
        /* One lv_label via the shared list_row_style, not a container +
         * child label each with their own local style properties -- see
         * list_row_style's own doc comment (screen_builders.h). */
        lv_obj_t * row = lv_label_create(list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_text(row, label_of(i));

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

/* Same row construction as populate_indexed_list() above, but from an
 * already-built label array rather than an index-based label_of()
 * callback -- used by search_apply_filter()/search_close() (see the
 * search_binding_t infra further down) to repopulate a non-virtualized
 * build_subsonic_list_screen() list under an active filter, where each
 * row's position no longer corresponds to 0..count-1 into the backing
 * cache array. */
static void populate_indexed_list_from_items(lv_obj_t * list, const compact_list_item_t * items, int count, lv_event_cb_t click_cb) {
    lv_obj_clean(list);
    for (int i = 0; i < count; i++) {
        lv_obj_t * row = lv_label_create(list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_text(row, items[i].label);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list) {
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

    lv_obj_t * title_label = lv_label_create(scr);
    lv_label_set_text(title_label, default_title);
    lv_obj_add_style(title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title_label, &app_font_28, 0); /* later re-set to a real (possibly non-Latin) server artist/album/song name -- see fallback_font.h */
    /* Real-device bug report: a long artist/album/playlist name (this
     * label's whole reason for existing, per the comment above) centered
     * via LV_ALIGN_TOP_MID ran directly under the back button on the left
     * and, on a screen that also has a search/download button on the
     * right (see reserve_title_width_before()'s own callers below), under
     * that too. Left-aligned starting just past the back button, with a
     * generous default width (narrowed later, per-screen, by
     * reserve_title_width_before() once that screen's own right-side
     * button actually exists) and auto-scroll for whatever still overflows
     * -- same fix as group_songs_screen's own title (build_group_songs_screen()). */
    int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    lv_obj_set_width(title_label, scr_w - TITLE_LABEL_LEFT_INSET - TITLE_LABEL_DEFAULT_RIGHT_MARGIN);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, TITLE_LABEL_LEFT_INSET, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);

    lv_obj_t * list = lv_obj_create(scr);
    lv_obj_set_size(list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER); /* see build_icon_grid_screen's comment in screen_builders.c */
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, 4, 0);
    lv_obj_set_style_pad_top(list, 4, 0);

    *out_title_label = title_label;
    *out_list = list;
    finalize_screen_navigation(scr);
    return scr;
}

/* Pill-styled toggle/chevron rows for the Wi-Fi/Bluetooth settings screens'
 * dynamic top section -- same real assets (touch_list/item_bg.png,
 * settings/on.png/off.png) as screen_builders.c's build_pill_list_screen(),
 * but built directly into an already-existing dynamically-repopulated list
 * container rather than that function's own fixed item array, since these
 * screens mix a handful of fixed settings rows with variable-length device/
 * network lists below them (build_pill_list_screen has no notion of
 * "and also whatever these dynamic sections need appended after"). */
static lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, 448, 124);
    lv_obj_add_style(row, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_image_src(row, asset_path("touch_list/item_bg.png"), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, ui_size_16, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 24, 0);
    return row;
}

static lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);

    lv_obj_t * toggle_img = lv_image_create(row);
    lv_image_set_src(toggle_img, asset_path(checked ? "settings/on.png" : "settings/off.png"));
    lv_obj_align(toggle_img, LV_ALIGN_RIGHT_MID, -20, 0);
    /* Real-device bug report: accent color wasn't applying to the Wi-Fi/
     * Bluetooth screens' own toggle rows -- add_style(&style_accent) isn't
     * usable here (these rows are torn down and rebuilt fresh on every
     * dynamic-list repopulation, same as the rest of this file's screens,
     * so a plain local style is just as correct and doesn't need
     * lv_obj_report_style_change() to reach it), but the on.png/off.png
     * sprite itself still needs an explicit recolor -- see
     * apply_accent_color()'s own comment on image_recolor vs bg_color.
     *
     * Real-device bug report #2: recoloring unconditionally (regardless of
     * `checked`) tinted BOTH on.png and off.png the same accent color,
     * making every toggle look identical in either state -- the whole
     * point of a two-sprite toggle is that they're already visually
     * distinct (on.png accent-ish/colored, off.png neutral gray), and this
     * was overwriting that distinction rather than adding to it. Only the
     * ON sprite should ever be recolored, same as every lv_switch-based
     * toggle in this file only applies style_accent with the
     * LV_STATE_CHECKED selector, never unconditionally. */
    if (checked) {
        lv_obj_set_style_image_recolor(toggle_img, accent_lv_color(), 0);
        /* LV_OPA_80, not LV_OPA_COVER -- see apply_accent_color()'s own
         * comment on style_accent's image_recolor_opa for why COVER
         * flattens on.png's handle and track into one indistinguishable
         * solid color. */
        lv_obj_set_style_image_recolor_opa(toggle_img, LV_OPA_80, 0);
    }

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

static lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * label_text, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);

    /* No matching real chevron asset found in theme2 at this screen scale
     * (see screen_builders.c's own PILL_ACCESSORY_CHEVRON) -- plain text is
     * the honest fallback rather than forcing a mismatched sprite. */
    lv_obj_t * chevron = lv_label_create(row);
    lv_label_set_text(chevron, ">");
    lv_obj_add_style(chevron, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(chevron, ui_size_16, 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -20, 0);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

static lv_obj_t * add_section_header(lv_obj_t * parent, const char * text) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, ui_size_16, 0);
    lv_obj_set_style_pad_top(label, 12, 0);
    lv_obj_set_style_pad_left(label, 24, 0);
    return label;
}

/* ---- plugin.show_settings_list() -- src/plugins/plugin_manager.c's
 * l_plugin_show_settings_list() bridge. A second, separate screen pool from
 * gui_plugin_show_list()'s own (PLUGIN_LIST_SCREEN_POOL_SIZE above): that
 * pool's rows are plain list_row_style labels, but a settings submenu needs
 * the pill-row visual language (add_pill_row_base()/add_pill_toggle_row()/
 * add_pill_chevron_row() just above -- the same building blocks the Wi-Fi/
 * Bluetooth screens' own dynamic top section already uses) plus a new
 * slider-row variant neither of those screens needed. See plugin_manager.h's
 * own comment on PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE/_MAX_ROWS/_MAX_
 * SLIDERS for the sizing rationale. ---- */

typedef struct {
    int type; /* PLUGIN_SETTINGS_ROW_TAP/_TOGGLE/_SLIDER, plugin_manager.h */
    char label[96];
    bool toggle_value;
    int slider_min, slider_max, slider_value;
    char icon_path[256]; /* "" = none */
    int32_t row_height;  /* 0 = default, ignored for a slider row */
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
    lv_obj_set_size(card, 448, 130);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(card); /* child 0 */
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    /* text_size is never NULL here -- populate_plugin_settings_list_screen()
     * already defaults it to "small" (matching this row's own previous
     * hardcoded ui_size_16) before calling in. */
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, 12);
    lv_obj_set_style_text_font(label, pill_row_resolve_text_size(text_size), 0);
    pill_row_apply_icon(card, label, icon_path, PILL_ROW_ICON_PX_DEFAULT, LV_ALIGN_TOP_LEFT, 20, 12);

    lv_obj_t * value_label = lv_label_create(card); /* child 1 -- see plugin_settings_slider_event_cb()'s lookup */
    lv_obj_add_style(value_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(value_label, ui_size_16, 0);
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
    lv_obj_add_style(slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(slider, &style_accent, LV_PART_KNOB);
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
static void apply_plugin_pill_row_resize(lv_obj_t * row_obj, int32_t row_height) {
    if (row_height <= 0) return;
    int32_t height = row_height;
    if (height < PILL_ROW_HEIGHT_MIN) height = PILL_ROW_HEIGHT_MIN;
    if (height > PILL_ROW_HEIGHT_MAX) height = PILL_ROW_HEIGHT_MAX;
    lv_obj_set_height(row_obj, height);
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
         * hardcoded ui_size_16, so a row that doesn't set text_size renders
         * exactly as it did before this field existed. */
        const char * text_size = st->text_size[0] ? st->text_size : "small";

        if (st->type == PLUGIN_SETTINGS_ROW_TOGGLE) {
            lv_obj_t * row_obj = add_pill_toggle_row(list, st->label, st->toggle_value, NULL);
            lv_obj_add_event_cb(row_obj, plugin_settings_toggle_row_click_cb, LV_EVENT_CLICKED, packed);
            lv_obj_t * label = lv_obj_get_child(row_obj, 0); /* add_pill_row_base()'s own child-0-is-the-label layout */
            lv_obj_set_style_text_font(label, pill_row_resolve_text_size(text_size), 0);
            pill_row_apply_icon(row_obj, label, icon, PILL_ROW_ICON_PX_DEFAULT, LV_ALIGN_LEFT_MID, 24, 0);
            apply_plugin_pill_row_resize(row_obj, st->row_height);
        } else if (st->type == PLUGIN_SETTINGS_ROW_SLIDER) {
            lv_obj_t * card = add_pill_slider_row(list, st->label, st->slider_min, st->slider_max, st->slider_value,
                                                   plugin_settings_slider_event_cb, packed, icon, text_size);
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
            apply_plugin_pill_row_resize(row_obj, st->row_height);
        }
    }
}

int gui_plugin_show_settings_list(const char * title, const int * row_types, const char * const * labels,
                                   const bool * toggle_initial, const int * slider_min, const int * slider_max,
                                   const int * slider_value, const char * const * icon_paths, const int32_t * heights,
                                   const char * const * text_sizes, int count) {
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
        snprintf(st->text_size, sizeof(st->text_size), "%s", text_sizes[i] ? text_sizes[i] : "");
    }
    plugin_settings_list_row_state_count[slot] = n;

    populate_plugin_settings_list_screen(slot);
    nav_push(plugin_settings_list_screens[slot]);
    return slot;
}

static subsonic_artist_t * subsonic_artists_cache = NULL;
static int subsonic_artists_count = 0;
static subsonic_album_t * subsonic_albums_cache = NULL;
static int subsonic_albums_count = 0;
static subsonic_song_t * subsonic_songs_cache = NULL;
static int subsonic_songs_count = 0;
static subsonic_playlist_t * subsonic_playlists_cache = NULL;
static int subsonic_playlists_count = 0;

static lv_obj_t * subsonic_menu_screen;
static lv_obj_t * subsonic_menu_title_label;
static lv_obj_t * subsonic_menu_list;
static lv_obj_t * subsonic_artists_screen;
static lv_obj_t * subsonic_artists_title_label;
static lv_obj_t * subsonic_artists_list;
static lv_obj_t * subsonic_albums_screen;
static lv_obj_t * subsonic_albums_title_label;
static lv_obj_t * subsonic_albums_list;
static lv_obj_t * subsonic_albums_download_btn;
static lv_obj_t * subsonic_songs_screen;
static lv_obj_t * subsonic_songs_title_label;
static lv_obj_t * subsonic_songs_list;
static lv_obj_t * subsonic_songs_download_btn;
static lv_obj_t * subsonic_playlists_screen;
static lv_obj_t * subsonic_playlists_title_label;
static lv_obj_t * subsonic_playlists_list;
static lv_obj_t * subsonic_saved_servers_screen;
static lv_obj_t * subsonic_saved_servers_list;
static lv_obj_t * subsonic_new_connection_screen;

/* Subsonic screen redesign: subsonic_albums_screen is reused both for "one
 * artist's albums" (reached from Artists) and "every album on the server"
 * (reached from the new Albums menu row, via subsonic_get_all_albums()) --
 * same list shape, same tap-to-see-songs destination either way. The
 * Download button (top of "an Artist page", per the feature request) only
 * makes sense for the former -- downloading "all albums in the whole
 * library" is a much bigger, likely-unintended operation nobody asked for
 * -- so this tracks which case is currently showing: non-empty when it's
 * one artist's albums (holds that artist's name, both to gate the button
 * and to label the download's progress screen), empty for the flat
 * top-level list. Set by whichever click handler populates the screen. */
static char subsonic_albums_context_artist[128] = "";

/* subsonic_songs_screen is reused for "one album's songs" (Download creates
 * no playlist file) and "one playlist's songs" (Download also creates a
 * local .m3u alongside the downloaded files) -- this tracks which, and the
 * playlist's own name when it's the latter. Set by whichever click handler
 * populates the screen. */
static bool subsonic_songs_context_is_playlist = false;
static char subsonic_songs_context_playlist_name[128] = "";

static const char * subsonic_artist_label_of(int i) { return subsonic_artists_cache[i].name; }
static const char * subsonic_album_label_of(int i) { return subsonic_albums_cache[i].name; }
static const char * subsonic_song_label_of(int i) { return subsonic_songs_cache[i].title; }
static const char * subsonic_playlist_label_of(int i) { return subsonic_playlists_cache[i].name; }

/* Real-device bug report: connecting to a Subsonic (Navidrome) server and
 * selecting a song crashed the player instead of playing it. Root cause:
 * this device has only 64MB of RAM total, and /tmp here is tmpfs (RAM-
 * backed, capped at ~28MB by the rootfs) -- not disk, unlike a normal Linux
 * box. A single downloaded FLAC landed at ~26MB, confirmed via dmesg's own
 * OOM killer trace ("Out of memory: Kill process ... open_hiby_playe") --
 * the file alone nearly filled the entire tmpfs cap, and pushed system-wide
 * free memory low enough for the kernel to kill this process outright.
 * MUSIC_ROOT_DIR (the SD card, vfat, tens of GB free on a real device) is
 * genuinely disk-backed and already exactly where every other file this app
 * touches lives -- moving the download destination there instead of /tmp
 * fixes this for any track size, not just ones under some smaller cap. */
#define SUBSONIC_STREAM_CACHE_DIR MUSIC_ROOT_DIR "/.subsonic_cache"

/* Builds one queue entry (stream URL + parallel metadata) for song into
 * new_playlist[slot]/new_meta[slot] -- shared by subsonic_song_row_click_cb()
 * below across every mp3/flac song it queues, not just the tapped one. */
static void subsonic_fill_stream_queue_entry(const subsonic_server_t * server, const subsonic_song_t * song,
                                              char ** new_playlist, subsonic_stream_song_meta_t * new_meta, int slot) {
    char url[1536];
    subsonic_build_stream_url(server, song->id, url, sizeof(url));
    /* The "#.<suffix>" appended here is a local-only hint consumed by
     * audio.c's decoder_open() (stream_format_hint()); http_conn_parse_url()
     * strips it before it ever reaches the actual HTTP request, so it has
     * no effect on the server-facing URL. */
    size_t len = strlen(url);
    snprintf(url + len, sizeof(url) - len, "#.%s", song->suffix);

    new_playlist[slot] = strdup(url);

    subsonic_stream_song_meta_t * m = &new_meta[slot];
    snprintf(m->url, sizeof(m->url), "%s", url);
    snprintf(m->title, sizeof(m->title), "%s", song->title);
    snprintf(m->artist, sizeof(m->artist), "%s", song->artist);
    snprintf(m->album, sizeof(m->album), "%s", song->album);
    if (song->cover_art[0]) {
        subsonic_build_cover_art_url(server, song->cover_art, m->cover_url, sizeof(m->cover_url));
    } else {
        m->cover_url[0] = '\0';
    }
    m->verify_tls = server->verify_tls;
}

/* The actual logic behind tapping a Subsonic song, index into subsonic_
 * songs_cache -- split out from subsonic_song_row_click_cb() (immediately
 * below, the real LVGL callback) purely so it has a plain (int) signature
 * callable directly, without needing to fabricate an lv_event_t. */
static void subsonic_play_song_and_queue_rest(int index) {
    subsonic_song_t * song = &subsonic_songs_cache[index];

    subsonic_server_t server = subsonic_server_from_settings();

    /* mp3/flac plays directly off the stream URL -- no download, no wait --
     * and queues the rest of this album/playlist (subsonic_songs_cache is
     * already the full song list either way, see subsonic_songs_context_is_
     * playlist's own comment above) that's ALSO mp3/flac, in original order,
     * starting at the tapped song, so Prev/Next/auto-advance/Repeat/Shuffle
     * all work across the whole thing exactly like a local-library playlist.
     * A song in some other format is simply left out of this queue -- there's
     * no good way to background-download it without either stalling the
     * queue right there or building a much bigger hybrid stream+download
     * pipeline, and skipping it keeps every other song in the album/playlist
     * reachable instead of the queue silently dead-ending on it. See this
     * section's own top comment for why a non-streamable format tapped
     * directly still downloads first, as a single track, same as before. */
    if (strcasecmp(song->suffix, "mp3") == 0 || strcasecmp(song->suffix, "flac") == 0) {
        char ** new_playlist = malloc(sizeof(char *) * (size_t) subsonic_songs_count);
        subsonic_stream_song_meta_t * new_meta = malloc(sizeof(subsonic_stream_song_meta_t) * (size_t) subsonic_songs_count);
        int count = 0;
        int start_index = -1;

        for (int i = 0; i < subsonic_songs_count; i++) {
            subsonic_song_t * s = &subsonic_songs_cache[i];
            if (strcasecmp(s->suffix, "mp3") != 0 && strcasecmp(s->suffix, "flac") != 0) continue;
            subsonic_fill_stream_queue_entry(&server, s, new_playlist, new_meta, count);
            if (i == index) start_index = count;
            count++;
        }

        if (start_index < 0) {
            /* Shouldn't happen -- the tapped song itself was mp3/flac, so it
             * must have been included above -- but fail safely rather than
             * play the wrong track if this invariant is ever violated. */
            for (int i = 0; i < count; i++) free(new_playlist[i]);
            free(new_playlist);
            free(new_meta);
            return;
        }

        free(subsonic_stream_meta);
        subsonic_stream_meta = new_meta;
        subsonic_stream_meta_count = count;

        clear_player_source(); /* a streamed queue has no on-device list to go back to */
        on_file_selected(new_playlist, count, start_index);
        return;
    }

    char url[1536];
    subsonic_build_stream_url(&server, song->id, url, sizeof(url));
    mkdir(SUBSONIC_STREAM_CACHE_DIR, 0755); /* no-op (EEXIST) if it's already there */
    char dest[256];
    snprintf(dest, sizeof(dest), SUBSONIC_STREAM_CACHE_DIR "/stream.%s", song->suffix);

    start_subsonic_download(url, server.verify_tls, dest, song->title);
}

static void subsonic_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    subsonic_play_song_and_queue_rest(index);
}

static void subsonic_album_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    index = search_remap_index(SEARCH_BINDING_SUBSONIC_ALBUMS, index);

    subsonic_server_t server = subsonic_server_from_settings();
    free(subsonic_songs_cache);
    subsonic_songs_cache = NULL;
    subsonic_songs_count = 0;
    if (!subsonic_get_album_songs(&server, subsonic_albums_cache[index].id, &subsonic_songs_cache, &subsonic_songs_count)) return;

    subsonic_songs_context_is_playlist = false; /* an album's Download button never creates an .m3u -- see subsonic_download_songs_btn_cb() */
    lv_label_set_text(subsonic_songs_title_label, subsonic_albums_cache[index].name);
    lv_obj_clear_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_HIDDEN);
    populate_indexed_list(subsonic_songs_list, subsonic_songs_count, subsonic_song_label_of, subsonic_song_row_click_cb);
    nav_push(subsonic_songs_screen);
}

static void subsonic_artist_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    index = search_remap_index(SEARCH_BINDING_SUBSONIC_ARTISTS, index);

    subsonic_server_t server = subsonic_server_from_settings();
    free(subsonic_albums_cache);
    subsonic_albums_cache = NULL;
    subsonic_albums_count = 0;
    if (!subsonic_get_artist_albums(&server, subsonic_artists_cache[index].id, &subsonic_albums_cache, &subsonic_albums_count)) return;

    /* Non-empty -- this is "an Artist page" (one artist's albums), so its
     * Download button (downloads every song across every album shown here)
     * is meaningful; see subsonic_albums_context_artist's own comment. */
    snprintf(subsonic_albums_context_artist, sizeof(subsonic_albums_context_artist), "%s", subsonic_artists_cache[index].name);
    lv_label_set_text(subsonic_albums_title_label, subsonic_artists_cache[index].name);
    lv_obj_clear_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
    populate_indexed_list(subsonic_albums_list, subsonic_albums_count, subsonic_album_label_of, subsonic_album_row_click_cb);
    nav_push(subsonic_albums_screen);
}

/* ---- Subsonic screen redesign: Playlists (new top-level menu row) and the
 * flat "every album in the library" Albums row -- both reuse existing
 * screens/caches (subsonic_songs_screen and subsonic_albums_screen
 * respectively) rather than needing screens of their own, since a
 * playlist's songs and an album's songs are shown identically, and the flat
 * album list is shown identically to one artist's album list. ---- */

static void subsonic_playlist_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);

    subsonic_server_t server = subsonic_server_from_settings();
    free(subsonic_songs_cache);
    subsonic_songs_cache = NULL;
    subsonic_songs_count = 0;
    if (!subsonic_get_playlist_songs(&server, subsonic_playlists_cache[index].id, &subsonic_songs_cache, &subsonic_songs_count)) return;

    subsonic_songs_context_is_playlist = true; /* this page's Download button also creates a local .m3u -- see subsonic_download_songs_btn_cb() */
    snprintf(subsonic_songs_context_playlist_name, sizeof(subsonic_songs_context_playlist_name), "%s",
             subsonic_playlists_cache[index].name);
    lv_label_set_text(subsonic_songs_title_label, subsonic_playlists_cache[index].name);
    lv_obj_clear_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_HIDDEN);
    populate_indexed_list(subsonic_songs_list, subsonic_songs_count, subsonic_song_label_of, subsonic_song_row_click_cb);
    nav_push(subsonic_songs_screen);
}

static void subsonic_menu_artists_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Already fetched/populated at connect time (poll_subsonic_connect()) --
     * no re-fetch needed here, matching every other "browse from cache"
     * screen entry in this app. */
    nav_push(subsonic_artists_screen);
}

static void subsonic_menu_playlists_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    subsonic_server_t server = subsonic_server_from_settings();
    free(subsonic_playlists_cache);
    subsonic_playlists_cache = NULL;
    subsonic_playlists_count = 0;
    if (!subsonic_get_playlists(&server, &subsonic_playlists_cache, &subsonic_playlists_count)) return;

    populate_indexed_list(subsonic_playlists_list, subsonic_playlists_count, subsonic_playlist_label_of,
                           subsonic_playlist_row_click_cb);
    nav_push(subsonic_playlists_screen);
}

static void subsonic_menu_albums_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    subsonic_server_t server = subsonic_server_from_settings();
    free(subsonic_albums_cache);
    subsonic_albums_cache = NULL;
    subsonic_albums_count = 0;
    if (!subsonic_get_all_albums(&server, &subsonic_albums_cache, &subsonic_albums_count)) return;

    /* Empty -- this is the flat, whole-library album list, not "an Artist
     * page," so no Download-everything button (see subsonic_albums_context_
     * artist's own comment for why that distinction matters). */
    subsonic_albums_context_artist[0] = '\0';
    lv_label_set_text(subsonic_albums_title_label, "Albums");
    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
    populate_indexed_list(subsonic_albums_list, subsonic_albums_count, subsonic_album_label_of, subsonic_album_row_click_cb);
    nav_push(subsonic_albums_screen);
}

/* Top-of-screen Download button shared by subsonic_songs_screen (an
 * album's or a playlist's songs -- see subsonic_songs_context_is_playlist)
 * -- downloads exactly what's currently shown, into the local library. */
static void subsonic_download_songs_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (subsonic_songs_count == 0) return;

    subsonic_song_t * songs_copy = malloc(sizeof(subsonic_song_t) * (size_t) subsonic_songs_count);
    memcpy(songs_copy, subsonic_songs_cache, sizeof(subsonic_song_t) * (size_t) subsonic_songs_count);

    const char * playlist_name = subsonic_songs_context_is_playlist ? subsonic_songs_context_playlist_name : NULL;
    char label[192];
    snprintf(label, sizeof(label), "Downloading\n%s...", lv_label_get_text(subsonic_songs_title_label));
    start_subsonic_library_download(songs_copy, subsonic_songs_count, NULL, 0, playlist_name, label);
}

/* Top-of-screen Download button on subsonic_albums_screen, active only when
 * it's showing "an Artist page" (see subsonic_albums_context_artist) --
 * downloads every song across every album currently listed. Hands the
 * thread the album list itself (Mode B, see subsonic_library_download_
 * request_t's own comment) rather than pre-fetching every album's songs
 * here on the UI thread first, since that could be a lot of sequential
 * network round trips for an artist with many albums. */
static void subsonic_download_artist_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (subsonic_albums_context_artist[0] == '\0' || subsonic_albums_count == 0) return;

    subsonic_album_t * albums_copy = malloc(sizeof(subsonic_album_t) * (size_t) subsonic_albums_count);
    memcpy(albums_copy, subsonic_albums_cache, sizeof(subsonic_album_t) * (size_t) subsonic_albums_count);

    char label[192];
    snprintf(label, sizeof(label), "Downloading\n%s...", subsonic_albums_context_artist);
    start_subsonic_library_download(NULL, 0, albums_copy, subsonic_albums_count, NULL, label);
}

/* Ping + getArtists run on the same background-thread-plus-polled-flag
 * shape as the per-song download (see download_thread_func/
 * poll_subsonic_download above) -- these were originally called straight
 * from the click handler on the main LVGL thread on the theory that a
 * ping+one API call is a sub-second round trip, but real-hardware testing
 * showed just how badly *any* main-thread stall reads on this device (a
 * frozen screen is indistinguishable from a real hang), and a TCP connect()
 * to an unreachable/slow server has no such sub-second guarantee -- it can
 * block for the OS's full connect timeout. Silently stays wherever it
 * lands on any failure (wrong credentials, unreachable server, ...) --
 * there's still no toast/error-message UI in this app to explain why,
 * which remains a real, separately-tracked gap. Shared by both Saved
 * Servers (tapping a row) and New Connection (the form's own Connect &
 * Browse button) below, rather than each having its own copy. */
static pthread_t subsonic_connect_thread;
static bool subsonic_connect_active = false;
static volatile bool subsonic_connect_done_flag = false;
static volatile bool subsonic_connect_success_flag = false;
/* Snapshot of whichever server this in-flight attempt is for -- the
 * request struct itself is freed inside the thread function before
 * poll_subsonic_connect() ever runs, so this is what poll_subsonic_
 * connect() reads back to know what to persist on success. */
static subsonic_server_t subsonic_connect_pending_server;

typedef struct {
    subsonic_server_t server;
} subsonic_connect_request_t;

static void * subsonic_connect_thread_func(void * arg) {
    subsonic_connect_request_t * req = (subsonic_connect_request_t *) arg;

    bool ok = subsonic_ping(&req->server);
    if (ok) {
        free(subsonic_artists_cache);
        subsonic_artists_cache = NULL;
        subsonic_artists_count = 0;
        ok = subsonic_get_artists(&req->server, &subsonic_artists_cache, &subsonic_artists_count);
    }

    subsonic_connect_success_flag = ok;
    subsonic_connect_done_flag = true; /* written last -- poll_subsonic_connect only checks this flag */
    free(req);
    return NULL;
}

static void poll_subsonic_connect(void) {
    if (!subsonic_connect_active || !subsonic_connect_done_flag) return;

    subsonic_connect_active = false;
    pthread_join(subsonic_connect_thread, NULL);
    bool success = subsonic_connect_success_flag;

    /* Same nav_pop()-races-a-navigating-callback issue already fixed for
     * Wi-Fi manual SSID entry and the Subsonic download-then-play path --
     * see text_entry_kb_event_cb's own comment for the full mechanism, and
     * poll_subsonic_download()'s own comment for why skipping nav_pop()
     * alone isn't enough either (it leaves this screen's own stack slot
     * stuck underneath the artists screen forever) -- nav_remove_stack_
     * slot() splices it out instead, once it's clear the artists screen
     * already took its place. */
    int depth_before = nav_depth;
    if (success) {
        /* Becomes the active connection everything else in this app's
         * Subsonic browsing/download flow reads via subsonic_server_from_
         * settings() -- Saved Servers and New Connection both funnel
         * through this one function, so this is the one place that needs
         * to update it, regardless of which screen the user connected
         * from. Also persisted to the Saved Servers list itself (an
         * upsert, so reconnecting to an already-saved server with updated
         * credentials just refreshes it rather than duplicating it). */
        snprintf(current_settings.subsonic_url, sizeof(current_settings.subsonic_url), "%s",
                 subsonic_connect_pending_server.base_url);
        snprintf(current_settings.subsonic_username, sizeof(current_settings.subsonic_username), "%s",
                 subsonic_connect_pending_server.username);
        snprintf(current_settings.subsonic_password, sizeof(current_settings.subsonic_password), "%s",
                 subsonic_connect_pending_server.password);
        current_settings.subsonic_verify_tls = subsonic_connect_pending_server.verify_tls;
        settings_save(&current_settings);

        metadata_db_subsonic_server_save(subsonic_connect_pending_server.base_url, subsonic_connect_pending_server.username,
                                          subsonic_connect_pending_server.password, subsonic_connect_pending_server.verify_tls);

        /* Subsonic screen redesign: lands on a menu (Artists/Playlists/
         * Albums) rather than jumping straight into the artist list --
         * getArtists is still fetched right here as part of connecting
         * (below), so tapping Artists from the menu is instant with no
         * extra round trip; Playlists/Albums fetch lazily on their own tap,
         * same as every other drill-down in this screen already does. */
        populate_indexed_list(subsonic_artists_list, subsonic_artists_count, subsonic_artist_label_of, subsonic_artist_row_click_cb);
        nav_push(subsonic_menu_screen);
    }
    if (nav_depth > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else {
        nav_pop(); /* leave the connecting screen */
    }
    /* else (failure): silently stays wherever nav_pop() landed -- same
     * documented gap as poll_subsonic_download above. */
}

static void start_subsonic_connect(const subsonic_server_t * server) {
    subsonic_connect_pending_server = *server;

    subsonic_connect_request_t * req = malloc(sizeof(*req));
    req->server = *server;

    subsonic_connect_done_flag = false;
    subsonic_connect_success_flag = false;
    subsonic_connect_active = true;

    lv_label_set_text(subsonic_downloading_label, "Connecting to server...");
    nav_push(subsonic_downloading_screen);

    pthread_create(&subsonic_connect_thread, NULL, subsonic_connect_thread_func, req);
}

/* ---- Saved Servers -- every server metadata_db_subsonic_server_save() has
 * ever remembered a successful connection for (see poll_subsonic_connect()
 * above), listed by URL; tapping one reconnects with its stored
 * credentials via the same start_subsonic_connect() the New Connection
 * form below also uses. ---- */

static subsonic_server_row_t * subsonic_saved_servers = NULL;
static int subsonic_saved_server_count = 0;

static void subsonic_saved_server_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    subsonic_server_row_t * row = &subsonic_saved_servers[index];

    subsonic_server_t server;
    snprintf(server.base_url, sizeof(server.base_url), "%s", row->url);
    snprintf(server.username, sizeof(server.username), "%s", row->username);
    snprintf(server.password, sizeof(server.password), "%s", row->password);
    server.verify_tls = row->verify_tls;
    start_subsonic_connect(&server);
}

static const char * subsonic_saved_server_label_of(int i) { return subsonic_saved_servers[i].url; }

static void populate_subsonic_saved_servers_screen(void) {
    free(subsonic_saved_servers);
    subsonic_saved_servers = NULL;
    subsonic_saved_server_count = 0;
    metadata_db_load_subsonic_servers(&subsonic_saved_servers, &subsonic_saved_server_count);

    lv_obj_clean(subsonic_saved_servers_list);
    if (subsonic_saved_server_count == 0) {
        lv_obj_t * label = lv_label_create(subsonic_saved_servers_list);
        lv_label_set_text(label, "No saved servers yet");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }
    populate_indexed_list(subsonic_saved_servers_list, subsonic_saved_server_count, subsonic_saved_server_label_of,
                          subsonic_saved_server_row_cb);
}

static lv_obj_t * build_subsonic_saved_servers_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Saved Servers", &title_label, &subsonic_saved_servers_list);
}

static void subsonic_saved_servers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_subsonic_saved_servers_screen();
    nav_push(subsonic_saved_servers_screen);
}

/* ---- New Connection -- a blank form with its own transient state, kept
 * deliberately separate from current_settings.subsonic_* (which only ever
 * gets updated on a SUCCESSFUL connect, in poll_subsonic_connect() above)
 * so an abandoned or failed new-connection attempt can never clobber
 * whatever server was actually active before it.
 *
 * Real-device bug report: no indicator on screen showed a field had
 * already been filled in -- the original build_pill_list_screen()-based
 * rows (same as the old single-connection setup screen) show only a
 * static label baked in once at gui_init() time, never anything reflecting
 * current state. Rebuilt as a dynamic list instead, same shape as the
 * Wi-Fi/DNS settings screens (add_pill_chevron_row/add_pill_toggle_row on
 * a plain scrollable container, repopulated on demand via lv_obj_clean() +
 * rebuild) -- every row here shows the field's current value (or a masked
 * "Set"/"Not set" for the password, so it's never actually shown on
 * screen), and gets refreshed after every edit. ---- */
static subsonic_server_t subsonic_new_conn_form = { .verify_tls = true };
static lv_obj_t * subsonic_new_connection_list;

static void populate_subsonic_new_connection_screen(void); /* defined below, after the row callbacks it references */

static void subsonic_new_conn_url_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.base_url, sizeof(subsonic_new_conn_form.base_url), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_url_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Server URL (e.g. https://myserver:4040)", subsonic_new_conn_form.base_url, false, false,
                    subsonic_new_conn_url_entry_done, NULL);
}

static void subsonic_new_conn_username_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.username, sizeof(subsonic_new_conn_form.username), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_username_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Username", subsonic_new_conn_form.username, false, false, subsonic_new_conn_username_entry_done, NULL);
}

static void subsonic_new_conn_password_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.password, sizeof(subsonic_new_conn_form.password), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_password_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Password", subsonic_new_conn_form.password, true, false, subsonic_new_conn_password_entry_done, NULL);
}

static void subsonic_new_conn_verify_tls_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    subsonic_new_conn_form.verify_tls = !subsonic_new_conn_form.verify_tls;
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_connect_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_subsonic_connect(&subsonic_new_conn_form);
}

static void populate_subsonic_new_connection_screen(void) {
    lv_obj_clean(subsonic_new_connection_list);

    char url_text[300];
    snprintf(url_text, sizeof(url_text), "Server URL: %s",
             subsonic_new_conn_form.base_url[0] ? subsonic_new_conn_form.base_url : "Not set");
    add_pill_chevron_row(subsonic_new_connection_list, url_text, subsonic_new_conn_url_row_cb);

    add_pill_toggle_row(subsonic_new_connection_list, "Verify server certificate", subsonic_new_conn_form.verify_tls,
                        subsonic_new_conn_verify_tls_toggle_cb);

    char username_text[160];
    snprintf(username_text, sizeof(username_text), "Username: %s",
             subsonic_new_conn_form.username[0] ? subsonic_new_conn_form.username : "Not set");
    add_pill_chevron_row(subsonic_new_connection_list, username_text, subsonic_new_conn_username_row_cb);

    add_pill_chevron_row(subsonic_new_connection_list, subsonic_new_conn_form.password[0] ? "Password: Set" : "Password: Not set",
                         subsonic_new_conn_password_row_cb);

    add_pill_chevron_row(subsonic_new_connection_list, "Connect & Browse", subsonic_new_conn_connect_row_cb);
}

static lv_obj_t * build_subsonic_new_connection_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("New Connection", &title_label, &subsonic_new_connection_list);
}

static void subsonic_new_connection_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Only the text fields reset -- verify_tls deliberately carries over
     * between visits (no widget-desync risk here anymore, unlike the old
     * build_pill_list_screen()-based version -- this screen's rows are
     * fully rebuilt by populate_subsonic_new_connection_screen() below on
     * every visit, so there's nothing stale left to desync from). */
    subsonic_new_conn_form.base_url[0] = '\0';
    subsonic_new_conn_form.username[0] = '\0';
    subsonic_new_conn_form.password[0] = '\0';
    populate_subsonic_new_connection_screen();
    nav_push(subsonic_new_connection_screen);
}

/* ---- Subsonic entry point -- just the two options; subsonic_tile_cb below
 * (Stream Media's Subsonic tile) pushes this instead of what used to be a
 * single combined setup+connect screen for one connection only. ---- */
static lv_obj_t * build_subsonic_entry_screen(void) {
    static pill_list_item_t items[2];
    items[0] = (pill_list_item_t){ "Saved Servers", PILL_ACCESSORY_CHEVRON, false, subsonic_saved_servers_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "New Connection", PILL_ACCESSORY_CHEVRON, false, subsonic_new_connection_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen("Subsonic", generic_back_cb, items, 2, &style_accent);
    finalize_screen_navigation(scr);
    return scr;
}

/* Drill-down target for Artists and Album Artists: tapping either shows the
 * artist's own albums (regrouping just their songs by album, via
 * build_groups_by_indices) rather than dumping straight into a flat song
 * list -- tapping an album from there lands on show_group_songs() same as
 * every other terminal list. One persistent screen shared by both callers,
 * same rebuild-in-place approach as group_songs_screen. */
static lv_obj_t * artist_albums_title_label;
static lv_obj_t * artist_albums_list;
static group_t * artist_albums_groups;
static int artist_albums_group_count;

/* Now-playing indicator bar -- same "recreated fresh every populate call"
 * lifecycle as group_songs_now_playing_bar (see its own comment):
 * populate_indexed_list()'s lv_obj_clean(artist_albums_list) destroys
 * whatever was here before, same as every row. */
static lv_obj_t * artist_albums_now_playing_bar;

static const char * artist_album_label_of(int i) { return artist_albums_groups[i].name; }

static void artist_album_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_group_songs(&artist_albums_groups[index]);
}

/* Matches by song-index membership (not album NAME, unlike Albums/Album
 * Artist's own name-based match) -- this screen already only ever holds
 * albums belonging to the ONE artist just tapped into (artist_albums_
 * groups is built from artist_group->indices), so an index check is both
 * more precise (no risk of a same-named album by a different artist
 * lighting this up) and cheaper than resolving artist/album strings again.
 * Same standalone-refresh reasoning as refresh_group_songs_now_playing_
 * indicator() -- callable without rebuilding rows, e.g. on a live playback
 * change while this screen stays open. */
static void refresh_artist_albums_now_playing_indicator(void) {
    if (!artist_albums_now_playing_bar) return;

    int match = -1;
    if (now_playing_song_index >= 0) {
        for (int i = 0; i < artist_albums_group_count && match < 0; i++) {
            for (int j = 0; j < artist_albums_groups[i].count; j++) {
                if (artist_albums_groups[i].indices[j] == now_playing_song_index) { match = i; break; }
            }
        }
    }

    if (match < 0) {
        lv_obj_add_flag(artist_albums_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(artist_albums_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(artist_albums_now_playing_bar, 0, 4 + match * (LIST_ROW_HEIGHT + 4));
}

static void show_artist_albums(const group_t * artist_group) {
    free_group_array(artist_albums_groups, artist_albums_group_count);
    build_groups_by_indices(artist_group->indices, artist_group->count, album_key_of, compare_index_by_album,
                             &artist_albums_groups, &artist_albums_group_count);

    lv_label_set_text(artist_albums_title_label, artist_group->name);
    populate_indexed_list(artist_albums_list, artist_albums_group_count, artist_album_label_of, artist_album_row_click_cb);

    /* Recreated fresh here, same reasoning as group_songs_now_playing_bar's
     * own comment -- populate_indexed_list() just destroyed whatever was
     * here before. LV_OBJ_FLAG_IGNORE_LAYOUT keeps this list's own flex
     * column layout from stacking it in as another row. */
    artist_albums_now_playing_bar = lv_obj_create(artist_albums_list);
    lv_obj_remove_style_all(artist_albums_now_playing_bar);
    lv_obj_set_size(artist_albums_now_playing_bar, 5, LIST_ROW_HEIGHT);
    lv_obj_set_style_bg_color(artist_albums_now_playing_bar, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(artist_albums_now_playing_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(artist_albums_now_playing_bar, 2, 0);
    lv_obj_add_flag(artist_albums_now_playing_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(artist_albums_now_playing_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(artist_albums_now_playing_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(artist_albums_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    refresh_artist_albums_now_playing_indicator();

    nav_push(artist_albums_screen);
}

static void artist_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_ARTISTS, index);
    show_artist_albums(&artist_groups[index]);
}

static void album_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_ALBUMS, index);
    show_group_songs(&album_groups[index]);
}

static lv_obj_t * build_artists_screen(void) {
    compact_list_item_t * items = NULL;
    if (artist_group_count > 0) {
        items = malloc(sizeof(compact_list_item_t) * (size_t) artist_group_count);
        for (int i = 0; i < artist_group_count; i++) {
            items[i] = (compact_list_item_t){ artist_groups[i].name };
        }
    }
    lv_obj_t * scr = build_compact_list_screen("Artists", generic_back_cb, items, artist_group_count, artist_row_click_cb, NULL, &artists_list, LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    free(items);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_albums_screen(void) {
    compact_list_item_t * items = NULL;
    if (album_group_count > 0) {
        items = malloc(sizeof(compact_list_item_t) * (size_t) album_group_count);
        for (int i = 0; i < album_group_count; i++) {
            items[i] = (compact_list_item_t){ album_groups[i].name };
        }
    }
    lv_obj_t * scr = build_compact_list_screen("Albums", generic_back_cb, items, album_group_count, album_row_click_cb, NULL, &albums_list, LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    free(items);
    finalize_screen_navigation(scr);
    return scr;
}

static void album_artist_row_click_cb(int index) {
    index = search_remap_index(SEARCH_BINDING_ALBUM_ARTIST, index);
    show_artist_albums(&album_artist_groups[index]);
}

static lv_obj_t * build_album_artist_screen(void) {
    compact_list_item_t * items = NULL;
    if (album_artist_group_count > 0) {
        items = malloc(sizeof(compact_list_item_t) * (size_t) album_artist_group_count);
        for (int i = 0; i < album_artist_group_count; i++) {
            items[i] = (compact_list_item_t){ album_artist_groups[i].name };
        }
    }
    lv_obj_t * scr = build_compact_list_screen("Album Artist", generic_back_cb, items, album_artist_group_count, album_artist_row_click_cb, NULL, &album_artist_list, LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    free(items);
    finalize_screen_navigation(scr);
    return scr;
}

/* ---- A-Z browse index (Artists/Album Artist/All Songs) --
 *
 * A draggable vertical letter strip (touch_list/a_z.png, stock HiBy asset)
 * overlaid on the right edge of each of these three screens, jumping the
 * underlying build_compact_list_screen() list straight to the first
 * entry starting with the touched letter -- the standard iOS Music/
 * Contacts pattern. touch_list/a_z_result_bg.png is the big popup letter
 * bubble shown while dragging, same convention.
 *
 * Drag tracking is polled from its own dedicated timer (poll_az_index_drag,
 * registered in gui_init() next to poll_quick_drawer_drag) rather than any
 * LVGL touch event, for the exact same reason poll_quick_drawer_drag()
 * does -- see its doc comment: LV_EVENT_PRESSING never fires at all on
 * this LVGL version, only PRESSED/RELEASED/CLICKED/LONG_PRESSED do,
 * regardless of which object was hit. A separate timer rather than folding
 * this into poll_quick_drawer_drag keeps that already-large function
 * scoped to its own three existing gestures. */

typedef const char * (*az_index_name_of_t)(int display_index);

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * list;
    lv_obj_t * strip;       /* the a_z.png touch strip */
    lv_obj_t * popup;       /* the a_z_result_bg.png bubble, hidden except while dragging */
    lv_obj_t * popup_label; /* big current-letter text inside popup */
    az_index_name_of_t name_of;
    const int * count_ptr;  /* address of artist_group_count/album_artist_group_count/all_songs_count -- read fresh on every drag, so a library rescan's new counts are picked up automatically */
} az_index_binding_t;

#define AZ_INDEX_BINDING_COUNT 4
static az_index_binding_t az_index_bindings[AZ_INDEX_BINDING_COUNT];
static int az_index_registered_count = 0;

static const char * artist_group_name_of(int i) { return artist_groups[i].name; }
static const char * album_group_name_of(int i) { return album_groups[i].name; }
static const char * album_artist_group_name_of(int i) { return album_artist_groups[i].name; }
static const char * all_songs_sorted_name_of(int i) { return song_display_title_of(all_songs_sort_order[i]); }

/* Called once per screen right after that screen (and its list) is built,
 * at both gui_init()'s boot-time build and the post-library-rescan rebuild
 * -- az_index_registered_count is reset to 0 before the latter, since the
 * old screen/list/strip/popup pointers this table holds become dangling
 * the moment lv_obj_delete() runs on the old screens there. */
static void register_az_index(lv_obj_t * screen, lv_obj_t * list, az_index_name_of_t name_of, const int * count_ptr) {
    /* Not touch_list/a_z.png -- confirmed via pixel sampling that stock
     * asset is a fully opaque solid-black rectangle with the letters
     * painted in (alpha=255 everywhere, RGB (0,0,0) in the "empty" areas),
     * not a real transparent PNG -- invisible only against HiBy's own
     * pure-black screens, never designed to overlap anything lighter. It
     * visibly blacks out the right edge of these lists' (28,28,30) row
     * backgrounds, which a widget-level bg_opa/border style can't fix
     * since that opaque black is baked into the image's own pixels, not
     * the object's separate background layer. A plain label (genuinely
     * transparent -- only its glyph pixels paint anything) sidesteps this
     * entirely and needs no chroma-key support this LVGL build doesn't have. */
    lv_obj_t * strip = lv_label_create(screen);
    lv_label_set_text(strip, "A\nB\nC\nD\nE\nF\nG\nH\nI\nJ\nK\nL\nM\nN\nO\nP\nQ\nR\nS\nT\nU\nV\nW\nX\nY\nZ\n#");
    lv_obj_set_style_text_font(strip, &lv_font_montserrat_16, 0);
    lv_obj_add_style(strip, &style_theme_text_primary, 0);
    lv_obj_set_style_text_align(strip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* Stretched with extra line spacing to span close to the full list
     * height (27 lines * lv_font_montserrat_16's own 22px line height is
     * only ~594px, well short of the ~688px-tall list area) rather than
     * sitting bunched up near the top. */
    lv_obj_set_style_text_line_space(strip, 3, 0);
    /* Top edge (the "A") lines up with the list's own top edge; the
     * stretched height above lands the bottom ("#") close to the screen's
     * bottom corner, matching the list's own bottom edge -- the list
     * itself starts at exactly STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT and
     * runs flush to the screen bottom (see build_compact_list_screen()). */
    lv_obj_align(strip, LV_ALIGN_TOP_RIGHT, -4, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT);

    lv_obj_t * popup = lv_image_create(screen);
    lv_image_set_src(popup, asset_path("touch_list/a_z_result_bg.png"));
    lv_obj_center(popup);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * popup_label = lv_label_create(popup);
    lv_obj_set_style_text_font(popup_label, &lv_font_montserrat_32, 0);
    lv_obj_add_style(popup_label, &style_theme_text_primary, 0);
    lv_obj_center(popup_label);

    az_index_bindings[az_index_registered_count++] =
        (az_index_binding_t){ screen, list, strip, popup, popup_label, name_of, count_ptr };
}

static az_index_binding_t * find_az_binding_for_screen(lv_obj_t * screen) {
    for (int i = 0; i < az_index_registered_count; i++) {
        if (az_index_bindings[i].screen == screen) return &az_index_bindings[i];
    }
    return NULL;
}

/* One entry per letter A-Z plus '#' (index 26) for anything not starting
 * with a letter -- the display index of the first matching entry, or the
 * nearest one after it if that exact letter has no entries (standard
 * "jump forward to nearest match" A-Z index behavior). Rebuilt fresh on
 * every touch-down rather than cached/invalidated on rescan -- a single
 * linear pass over at most a few thousand names is negligible next to a
 * 60fps timer's own budget, and this avoids needing any rescan-invalidation
 * hook at all. */
static void build_az_jump_table(az_index_binding_t * b, int * table) {
    for (int i = 0; i < 27; i++) table[i] = -1;

    int count = *b->count_ptr;
    for (int i = 0; i < count; i++) {
        const char * name = b->name_of(i);
        if (!name || !name[0]) continue;
        char c = (char) toupper((unsigned char) name[0]);
        int idx = (c >= 'A' && c <= 'Z') ? (c - 'A') : 26;
        if (table[idx] == -1) table[idx] = i;
    }

    for (int i = 25; i >= 0; i--) {
        if (table[i] == -1) table[i] = table[i + 1];
    }
}

static bool az_index_dragging = false;
static az_index_binding_t * az_index_active_binding = NULL;
static int az_index_jump_table[27];

static void poll_az_index_drag(lv_timer_t * timer) {
    (void) timer;
    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (!az_index_dragging) {
        if (!pressed) return;
        az_index_binding_t * b = find_az_binding_for_screen(lv_screen_active());
        if (!b) return;

        lv_area_t area;
        lv_obj_get_coords(b->strip, &area);
        if (p.x < area.x1 || p.x > area.x2 || p.y < area.y1 || p.y > area.y2) return;

        az_index_dragging = true;
        az_index_active_binding = b;
        build_az_jump_table(b, az_index_jump_table);
        lv_obj_remove_flag(b->popup, LV_OBJ_FLAG_HIDDEN);
    } else if (!pressed) {
        lv_obj_add_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN);
        az_index_dragging = false;
        az_index_active_binding = NULL;
        return;
    }

    az_index_binding_t * b = az_index_active_binding;
    lv_area_t area;
    lv_obj_get_coords(b->strip, &area);
    int32_t rel_y = p.y - area.y1;
    int32_t h = area.y2 - area.y1;
    if (rel_y < 0) rel_y = 0;
    if (rel_y >= h) rel_y = h - 1;
    int letter_idx = (int) ((int64_t) rel_y * 27 / (h > 0 ? h : 1));
    if (letter_idx > 26) letter_idx = 26;

    int target = az_index_jump_table[letter_idx];
    if (target >= 0) compact_list_scroll_to_index(b->list, target);

    char letter = (letter_idx < 26) ? (char) ('A' + letter_idx) : '#';
    lv_label_set_text_fmt(b->popup_label, "%c", letter);
}

/* ---- Live search (Artists/Albums/Album Artist/All Songs/Files) --
 *
 * A search icon in the title row opens an inline search bar + the
 * existing T9 keypad at the bottom of the SAME screen (reparented via
 * t9_keypad_attach(), see its own doc comment) -- unlike this app's
 * existing modal show_text_entry() flow, the list stays visible and
 * narrows live between them as each character is typed, rather than
 * being hidden behind a full-screen keyboard. The search bar (bg_search.png)
 * is drawn as a full-title-row-width container created AFTER (so on top
 * of) the screen's own back button and title -- covering both rather than
 * needing direct references to hide them, and doubling as a tap-absorbing
 * surface so the covered back button can't be hit through it. The only
 * way out of search while it's open is the close ("x") button; the
 * screen's own back button becomes reachable again once that's tapped. */

#define SEARCH_BAR_Y STATUS_BAR_CLEARANCE
#define SEARCH_BAR_HEIGHT 80

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * list;
    lv_obj_t * search_btn;
    lv_obj_t * search_bar;
    az_index_name_of_t name_of; /* same "label for display index i" shape the A-Z index already uses */
    const int * count_ptr;
    bool active;
    int * filtered_indices; /* display index -> real index; NULL when not filtering (full list shown) */
    int filtered_count;     /* only meaningful while filtered_indices != NULL */
    bool is_overlay_list;   /* true only for Files -- `list` is a search-results overlay layered on top of
                              * file_browser.c's own folder-browsing UI (opaque, its own screen-colored background),
                              * hidden except while search is active, rather than the screen's one-and-only list
                              * the other four bindings resize in place. */
    lv_event_cb_t flex_click_cb; /* non-NULL only for the Subsonic Artists/Albums bindings -- `list` is a plain
                              * build_subsonic_list_screen() flex-column list (populate_indexed_list()'s one-real-
                              * object-per-row shape, not the virtualized compact_list_widget the other bindings
                              * assume), so filtering repopulates it via populate_indexed_list_from_items() instead
                              * of compact_list_set_items(). NULL leaves the existing compact-list path unchanged --
                              * set by hand right after register_search() returns, since register_search()'s own
                              * struct-literal assignment doesn't take it as a parameter. */
} search_binding_t;

static search_binding_t search_bindings[SEARCH_BINDING_COUNT];

static search_binding_t * find_search_binding_for_screen(lv_obj_t * screen) {
    for (int i = 0; i < SEARCH_BINDING_COUNT; i++) {
        if (search_bindings[i].screen == screen) return &search_bindings[i];
    }
    return NULL;
}

static int search_remap_index(search_binding_id_t binding_id, int display_index) {
    search_binding_t * b = &search_bindings[binding_id];
    if (b->filtered_indices) return b->filtered_indices[display_index];
    return display_index;
}

/* Plain case-insensitive substring match -- not strcasestr(), which needs
 * _GNU_SOURCE defined before <string.h> that this file doesn't set (and
 * musl guards the prototype on it -- confirmed via musl's own string.h).
 * Empty needle matches everything (the "search box is empty" case). */
static bool search_matches(const char * haystack, const char * needle) {
    if (!haystack) return false;
    if (!needle || !needle[0]) return true;
    size_t needle_len = strlen(needle);
    for (const char * h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] != '\0' && tolower((unsigned char) h[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
}

/* Rebuilds binding->list's contents to only entries matching `query`, via
 * compact_list_set_items() rather than a screen rebuild. An empty query
 * shows NOTHING (not the full library) -- real-device feedback: search
 * should be an overlay that only shows something once you've actually
 * typed something, not the whole list up front. filtered_indices maps
 * each surviving display row back to its real artist_groups/album_groups/
 * album_artist_groups/all_songs_sort_order index, for the row-click
 * callbacks' search_remap_index() calls. */
static void search_apply_filter(search_binding_t * b, const char * query) {
    int count = (query && query[0]) ? *b->count_ptr : 0;
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) (count > 0 ? count : 1));
    int * indices = malloc(sizeof(int) * (size_t) (count > 0 ? count : 1));
    int matched = 0;

    for (int i = 0; i < count; i++) {
        const char * name = b->name_of(i);
        if (!search_matches(name, query)) continue;
        items[matched] = (compact_list_item_t){ name };
        indices[matched] = i;
        matched++;
    }

    if (b->flex_click_cb) {
        populate_indexed_list_from_items(b->list, items, matched, b->flex_click_cb);
    } else {
        compact_list_set_items(b->list, items, matched);
    }
    free(items);

    free(b->filtered_indices);
    if (query && query[0]) {
        b->filtered_indices = indices;
        b->filtered_count = matched;
    } else {
        /* Unfiltered -- display index already equals the real index, no
         * remap table needed (also covers count == 0 cleanly). */
        free(indices);
        b->filtered_indices = NULL;
        b->filtered_count = 0;
    }
}

static void search_textarea_value_changed_cb(lv_event_t * e) {
    (void) e;
    if (!text_entry_inline_mode_active) return;
    search_binding_t * b = find_search_binding_for_screen(lv_screen_active());
    if (!b) return;
    search_apply_filter(b, lv_textarea_get_text(text_entry_textarea));
}

/* Restores a binding's list to its normal full-height, bottom-anchored
 * layout -- the exact geometry build_compact_list_screen() itself uses. */
static void search_restore_list_geometry(lv_obj_t * list) {
    lv_obj_set_size(list, lv_pct(100),
                     lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE - TITLE_ROW_HEIGHT);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* Enter's meaning while a search binding owns the keypad: hide just the
 * keypad (search bar, typed query, and the current filter all stay) and
 * give the list back the vertical space the keypad occupied -- matches a
 * normal search box where dismissing the on-screen keyboard doesn't clear
 * what you searched for. text_entry_textarea is deliberately NOT
 * reparented back here (unlike the full t9_keypad_release()) so the typed
 * query stays visible in the search bar. */
static void search_dismiss_active_keypad(void) {
    search_binding_t * b = find_search_binding_for_screen(lv_screen_active());
    if (!b) return;
    lv_obj_set_parent(text_entry_keypad_group, text_entry_screen);
    text_entry_inline_mode_active = false;
    lv_obj_set_size(b->list, lv_pct(100), TEXT_ENTRY_GRID_Y - (SEARCH_BAR_Y + SEARCH_BAR_HEIGHT));
    lv_obj_align(b->list, LV_ALIGN_TOP_MID, 0, SEARCH_BAR_Y + SEARCH_BAR_HEIGHT);
}

static void search_open(search_binding_t * b) {
    lv_obj_add_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);

    if (b->is_overlay_list) lv_obj_remove_flag(b->list, LV_OBJ_FLAG_HIDDEN);

    az_index_binding_t * az = find_az_binding_for_screen(b->screen);
    if (az) lv_obj_add_flag(az->strip, LV_OBJ_FLAG_HIDDEN);

    /* Room for the search bar above AND the keypad below -- the keypad
     * attaches at its own fixed TEXT_ENTRY_GRID_X/Y position regardless of
     * which screen it's on, so that's the exact number to stop above. */
    lv_obj_set_size(b->list, lv_pct(100), TEXT_ENTRY_GRID_Y - (SEARCH_BAR_Y + SEARCH_BAR_HEIGHT));
    lv_obj_align(b->list, LV_ALIGN_TOP_MID, 0, SEARCH_BAR_Y + SEARCH_BAR_HEIGHT);

    /* Spans from near the bar's left edge to just before close_btn (at
     * 440-51-8=381). */
    t9_keypad_attach(b->screen, b->search_bar, 8, 14, 365);

    b->active = true;
    search_apply_filter(b, ""); /* start unfiltered -- also (re)establishes filtered_indices == NULL */
}

static void search_close(search_binding_t * b) {
    t9_keypad_release();

    lv_obj_add_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);

    az_index_binding_t * az = find_az_binding_for_screen(b->screen);
    if (az) lv_obj_remove_flag(az->strip, LV_OBJ_FLAG_HIDDEN);

    if (b->is_overlay_list) {
        lv_obj_add_flag(b->list, LV_OBJ_FLAG_HIDDEN); /* reveals file_browser.c's own folder-browsing UI underneath again */
    } else {
        search_restore_list_geometry(b->list);
    }

    free(b->filtered_indices);
    b->filtered_indices = NULL;
    compact_list_item_t * items = NULL;
    int count = *b->count_ptr;
    if (count > 0) {
        items = malloc(sizeof(compact_list_item_t) * (size_t) count);
        for (int i = 0; i < count; i++) items[i] = (compact_list_item_t){ b->name_of(i) };
    }
    if (b->flex_click_cb) {
        populate_indexed_list_from_items(b->list, items, count, b->flex_click_cb);
    } else {
        compact_list_set_items(b->list, items, count);
    }
    free(items);

    b->active = false;
}

/* screen_gesture_event_cb()'s back-swipe hook -- see its own forward
 * declaration for why. */
static bool search_close_if_active_for_screen(lv_obj_t * screen) {
    search_binding_t * b = find_search_binding_for_screen(screen);
    if (!b || !b->active) return false;
    search_close(b);
    return true;
}

static void search_btn_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    search_binding_id_t id = (search_binding_id_t) (intptr_t) lv_event_get_user_data(e);
    search_open(&search_bindings[id]);
}

static void search_close_btn_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    search_binding_id_t id = (search_binding_id_t) (intptr_t) lv_event_get_user_data(e);
    search_close(&search_bindings[id]);
}

/* Called once per screen right after that screen (and its list) is built,
 * same two call sites (boot-time gui_init() + post-rescan rebuild) the
 * A-Z index already registers at. Builds the initial search icon (top-
 * right of the title row, same position/pattern playlists_edit_btn
 * already uses) and the search bar (hidden until search_btn is tapped). */
static void register_search(search_binding_id_t id, lv_obj_t * screen, lv_obj_t * list, az_index_name_of_t name_of,
                             const int * count_ptr, bool is_overlay_list) {
    search_binding_t * b = &search_bindings[id];
    free(b->filtered_indices); /* re-registering (post-rescan rebuild) over a binding left mid-filter would otherwise leak this */

    lv_obj_t * search_btn = lv_image_create(screen);
    lv_image_set_src(search_btn, asset_path("sub_back/btn_search.png"));
    /* Vertically centered within the same STATUS_BAR_CLEARANCE..+TITLE_ROW_HEIGHT
     * band the back button's own 64x64 box occupies (build_back_button()),
     * not the shorter title-label-specific centering formula this used to
     * borrow -- real-device feedback: the two didn't line up. */
    lv_obj_align(search_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 51) / 2);
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(search_btn, search_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_GESTURE_BUBBLE); /* added after finalize_screen_navigation()'s one-time pass, needs this set explicitly -- see screen_gesture_event_cb()'s own comment */

    /* Sized to bg_search.png's own native 440x80 (not lv_pct(100)) and
     * left-aligned at x=0 -- LVGL's bg_image draws at native size CENTERED
     * within the object's own box (see LIST_ROW_WIDTH's own comment in
     * screen_builders.h), so a same-size box is what makes the image
     * render flush against bar's left edge instead of with margins on
     * both sides. Still fully covers the 64px-wide back button underneath
     * for tap-absorption purposes (440 >> 64). No mag_icon/clear_btn --
     * real-device feedback: search mode should show just the bar and the
     * close ("x") button, nothing else. */
    lv_obj_t * bar = lv_obj_create(screen);
    lv_obj_set_size(bar, 440, SEARCH_BAR_HEIGHT);
    lv_obj_set_pos(bar, 0, SEARCH_BAR_Y);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_image_src(bar, asset_path("sub_back/bg_search.png"), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    /* lv_obj_create() pulls in the default theme's "card" style, which sets
     * pad_all to a nonzero default -- every lv_obj_set_pos() below for this
     * bar's children is relative to its CONTENT area (inside that padding),
     * so left uncleared, close_btn (and the reparented textarea) land
     * further right/down than intended. Same root cause already documented
     * on volume_popup's own construction. */
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE); /* absorbs taps so the covered back button/title beneath can't be hit through it */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * close_btn = lv_image_create(bar);
    lv_image_set_src(close_btn, asset_path("sub_back/close.png"));
    lv_obj_set_pos(close_btn, 440 - 51 - 8, 14);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, search_close_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);

    /* Same reasoning as search_btn above -- bar and its children were all
     * added after finalize_screen_navigation()'s one-time recursive pass,
     * so a swipe starting on any of them (the bar covers the whole title
     * row) needs this set explicitly to bubble up to
     * screen_gesture_event_cb() at all. */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    enable_gesture_bubble_recursive(bar);

    *b = (search_binding_t){ screen, list, search_btn, bar, name_of, count_ptr, false, NULL, 0, is_overlay_list };
}

/* Files' search (SEARCH_BINDING_FILES) is the one binding with no per-
 * screen row-click callback of its own to add a search_remap_index() line
 * to (see the other four's own callbacks) -- it plays the tapped result
 * directly, building its own playlist fresh in exactly the order currently
 * displayed (the filtered subset if one's active, matching
 * all_songs_row_click_cb's own "build in display order, pass display_index
 * straight through" shape), so display_index never needs remapping here. */
static void files_search_row_click_cb(int display_index) {
    search_binding_t * b = &search_bindings[SEARCH_BINDING_FILES];
    int count = b->filtered_indices ? b->filtered_count : all_songs_count;

    char ** playlist_copy = malloc(sizeof(char *) * (size_t) count);
    for (int i = 0; i < count; i++) {
        int sorted_index = b->filtered_indices ? b->filtered_indices[i] : i;
        playlist_copy[i] = strdup(all_songs_paths[all_songs_sort_order[sorted_index]]);
    }
    /* Same underlying all_songs_sort_order data as All Songs itself (just
     * possibly filtered down), so the "List" option can just reopen the
     * (always unfiltered) All Songs screen -- remap the tapped result back
     * to its real position there. */
    int all_songs_display_index = b->filtered_indices ? b->filtered_indices[display_index] : display_index;
    set_player_source_all_songs(all_songs_display_index);
    on_file_selected(playlist_copy, count, display_index);
}

/* ---- Playlists (Music submenu, replacing the old Genres tile -- real-
 * device feedback: genre tags are inconsistently populated across
 * libraries, unlike play history/manual curation) --
 *
 * Favorites and Most Played (top 20 by play count, see metadata_db's
 * song_play_count table) are synthesized as group_t's and shown through
 * show_group_songs() -- the exact same drill-down screen Artists/Albums/
 * Album Artist already use -- rather than a screen of their own. Unlike
 * those groups (fixed at library-scan time), favorite/play-count
 * membership can change between visits, so these two are recomputed fresh
 * every time their row is tapped instead of once at scan time; the backing
 * indices arrays are freed+realloc'd on each tap rather than per screen
 * object, same lifetime as show_group_songs()'s own group_songs_indices
 * (borrowed, must outlive the group_songs_screen it's currently showing).
 *
 * User-created .m3u playlists (Player screen's "Add to Playlist", or
 * dropped onto the SD card by hand) are listed below those two via
 * playlist_files_scan() -- already used for the Add to Playlist picker,
 * just not previously surfaced anywhere for actually playing one. ---- */

#define MOST_PLAYED_LIMIT 20

static int * favorites_indices = NULL;
static group_t favorites_group;
static int * most_played_indices = NULL;
static group_t most_played_group;

/* all_songs_paths has no path->index map -- this only runs on the rare
 * "user tapped Favorites/Most Played" event, not a hot path, so a linear
 * scan is fine (same tradeoff metadata_db's own favorite/play-count tables
 * make by keying on path rather than this array's index). */
static int find_song_index_by_path(const char * path) {
    for (int i = 0; i < all_songs_count; i++) {
        if (strcmp(all_songs_paths[i], path) == 0) return i;
    }
    return -1;
}

static const group_t * find_group_by_name(const group_t * groups, int group_count, const char * name) {
    for (int i = 0; i < group_count; i++) {
        if (strcasecmp(groups[i].name, name) == 0) return &groups[i];
    }
    return NULL;
}

/* One specific album within one artist/album-artist's own songs -- same
 * scoping build_groups_by_indices() does for the on-device Artist ->
 * Albums drill-down (see artist_album_row_click_cb()), just resolving the
 * one album a remote play actually needs instead of materializing every
 * other album of that artist's too. Order-preserving: parent->indices is
 * already path-sorted within a shared artist (build_groups_by()'s own
 * tie-break), so this comes out in the same track order the on-device
 * Album Songs screen would show. Caller owns *out_indices (free() it). */
static void filter_group_by_album(const group_t * parent, const char * album, int ** out_indices, int * out_count) {
    int * indices = malloc(sizeof(int) * (size_t) parent->count);
    int count = 0;
    for (int i = 0; i < parent->count; i++) {
        if (strcasecmp(all_song_tags[parent->indices[i]].album, album) == 0) indices[count++] = parent->indices[i];
    }
    *out_indices = indices;
    *out_count = count;
}

/* Real-device bug report: playing a song from the remote-control web UI
 * always queued the entire library (alphabetical order, whatever play_mode
 * happened to be) instead of just the Album/Playlist the song was actually
 * tapped from -- see remote_control.c's own request_play_playlist_name
 * comment for the full story. playlist_name/artist_filter/album_artist_
 * filter/album_filter are remote_control_consume_play_index()'s own
 * context, empty string meaning "not provided". song_index is a raw
 * all_songs_paths/all_song_tags index (already translated from remote_
 * control.c's library index space by the caller). Falls back to the
 * existing whole-library behavior whenever no usable scope resolves --
 * both the plain "no filters at all" case (someone playing from All
 * Songs/search) and a stale reference (e.g. a playlist renamed/deleted
 * since the phone last loaded its song list). */
static void play_remote_control_song(int song_index, const char * playlist_name, const char * artist_filter,
                                      const char * album_artist_filter, const char * album_filter) {
    if (song_index < 0 || song_index >= all_songs_count) return;

    int * scoped_indices = NULL;
    int scoped_count = 0;
    char scoped_title[128] = "";

    if (playlist_name[0] != '\0') {
        char m3u_path[512];
        snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, playlist_name);
        char ** paths = NULL;
        int count = 0;
        if (file_browser_build_playlist_from_m3u(m3u_path, &paths, &count) && count > 0) {
            scoped_indices = malloc(sizeof(int) * (size_t) count);
            for (int i = 0; i < count; i++) {
                int idx = find_song_index_by_path(paths[i]);
                if (idx >= 0) scoped_indices[scoped_count++] = idx;
            }
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            snprintf(scoped_title, sizeof(scoped_title), "%s", playlist_name);
        }
    } else if (album_filter[0] != '\0' && (artist_filter[0] != '\0' || album_artist_filter[0] != '\0')) {
        const group_t * parent = artist_filter[0] != '\0'
                                      ? find_group_by_name(artist_groups, artist_group_count, artist_filter)
                                      : find_group_by_name(album_artist_groups, album_artist_group_count, album_artist_filter);
        if (parent) {
            filter_group_by_album(parent, album_filter, &scoped_indices, &scoped_count);
            snprintf(scoped_title, sizeof(scoped_title), "%s", album_filter);
        }
    }

    if (scoped_indices && scoped_count > 0) {
        int pos = -1;
        for (int i = 0; i < scoped_count; i++) {
            if (scoped_indices[i] == song_index) {
                pos = i;
                break;
            }
        }
        if (pos >= 0) {
            char ** playlist_copy = malloc(sizeof(char *) * (size_t) scoped_count);
            for (int i = 0; i < scoped_count; i++) playlist_copy[i] = strdup(all_songs_paths[scoped_indices[i]]);
            set_player_source_group_songs_direct(scoped_indices, scoped_count, scoped_title, pos);
            on_file_selected(playlist_copy, scoped_count, pos);
            free(scoped_indices);
            return;
        }
    }
    free(scoped_indices);

    int display_index = -1;
    for (int i = 0; i < all_songs_count; i++) {
        if (all_songs_sort_order[i] == song_index) {
            display_index = i;
            break;
        }
    }
    if (display_index >= 0) all_songs_row_click_cb(display_index);
}

/* Called from apply_track_metadata_to_ui() right after now_playing_song_
 * index is updated -- the single dispatch point that pushes it out to
 * every now-playing-aware list. Artists/Albums/Album Artist match by name
 * (a whole group's songs share one row, so the indicator lights up
 * whichever artist/album/album-artist the CURRENT TRACK belongs to, not
 * just an exact-index match); All Songs matches by exact song identity,
 * reverse-mapped through all_songs_sort_order since row i displays
 * all_songs_sort_order[i], not song index i directly (see all_songs_row_
 * click_cb()'s own comment). Skips (rather than asserts on) any list
 * that's NULL -- Artists/Albums/Album Artist/All Songs are all built once
 * at startup so in practice this only ever matters before gui_init()
 * finishes building them, but there's no reason to depend on call-order
 * here when a simple guard covers it. Not a hot path (once per real
 * track-start event, matching find_song_index_by_path()'s own linear-scan
 * tradeoff above), so four more linear scans are fine. */
static void refresh_now_playing_indicators(void) {
    int artist_row = -1, album_row = -1, album_artist_row = -1, all_songs_row = -1;

    if (now_playing_song_index >= 0) {
        const char * artist = all_song_tags[now_playing_song_index].artist;
        const char * album = all_song_tags[now_playing_song_index].album;
        const char * album_artist = all_song_tags[now_playing_song_index].album_artist;

        for (int i = 0; i < artist_group_count; i++) {
            if (strcmp(artist_groups[i].name, artist) == 0) { artist_row = i; break; }
        }
        for (int i = 0; i < album_group_count; i++) {
            if (strcmp(album_groups[i].name, album) == 0) { album_row = i; break; }
        }
        for (int i = 0; i < album_artist_group_count; i++) {
            if (strcmp(album_artist_groups[i].name, album_artist) == 0) { album_artist_row = i; break; }
        }
        for (int i = 0; i < all_songs_count; i++) {
            if (all_songs_sort_order[i] == now_playing_song_index) { all_songs_row = i; break; }
        }
    }

    if (artists_list) compact_list_set_now_playing(artists_list, artist_row);
    if (albums_list) compact_list_set_now_playing(albums_list, album_row);
    if (album_artist_list) compact_list_set_now_playing(album_artist_list, album_artist_row);
    if (all_songs_list) compact_list_set_now_playing(all_songs_list, all_songs_row);

    refresh_group_songs_now_playing_indicator(); /* group_songs isn't compact_list-based -- see its own comment */
    refresh_artist_albums_now_playing_indicator(); /* Artists/Album Artist's shared albums drill-down -- also not compact_list-based */
}

static void show_favorites(void) {
    char ** paths;
    int count;
    metadata_db_load_favorite_songs(&paths, &count);

    free(favorites_indices);
    favorites_indices = count > 0 ? malloc(sizeof(int) * (size_t) count) : NULL;
    int found = 0;
    for (int i = 0; i < count; i++) {
        int idx = find_song_index_by_path(paths[i]);
        if (idx >= 0) favorites_indices[found++] = idx; /* skip a favorite not in the currently-loaded library */
        free(paths[i]);
    }
    free(paths);

    snprintf(favorites_group.name, sizeof(favorites_group.name), "Favorites");
    favorites_group.indices = favorites_indices;
    favorites_group.count = found;
    show_group_songs(&favorites_group);
}

static void show_most_played(void) {
    char ** paths;
    int count;
    metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);

    free(most_played_indices);
    most_played_indices = count > 0 ? malloc(sizeof(int) * (size_t) count) : NULL;
    int found = 0;
    for (int i = 0; i < count; i++) {
        int idx = find_song_index_by_path(paths[i]);
        if (idx >= 0) most_played_indices[found++] = idx;
        free(paths[i]);
    }
    free(paths);

    snprintf(most_played_group.name, sizeof(most_played_group.name), "Most Played");
    most_played_group.indices = most_played_indices;
    most_played_group.count = found;
    show_group_songs(&most_played_group);
}

static lv_obj_t * playlists_list;
static char ** playlists_m3u_paths = NULL;
static int playlists_m3u_count = 0;

static int * playlist_m3u_song_indices = NULL;
static group_t playlist_m3u_group;

/* Maps paths/count (an m3u's contents, straight from file_browser_build_
 * playlist_from_m3u()) onto playlist_m3u_group.indices/count -- the same
 * path-to-library-index resolution show_favorites()/show_most_played() do,
 * since show_group_songs()/show_group_songs_editable() operate on library
 * indices, not raw paths. An m3u entry that isn't part of the currently-
 * scanned library (stale entry, or added since the last rescan) is silently
 * skipped, same tolerance those two already have. name is optional (NULL
 * leaves playlist_m3u_group.name as whatever it already was) -- the remove
 * callback below reuses this after a removal without needing to re-pass the
 * still-unchanged display name. Shared by show_m3u_playlist() (initial
 * entry) and group_song_remove_row_cb() (refresh in place after a removal). */
static void rebuild_playlist_m3u_group(const char * name, char ** paths, int count) {
    free(playlist_m3u_song_indices);
    playlist_m3u_song_indices = count > 0 ? malloc(sizeof(int) * (size_t) count) : NULL;
    int found = 0;
    for (int i = 0; i < count; i++) {
        int idx = find_song_index_by_path(paths[i]);
        if (idx >= 0) playlist_m3u_song_indices[found++] = idx;
    }

    if (name) snprintf(playlist_m3u_group.name, sizeof(playlist_m3u_group.name), "%s", name);
    playlist_m3u_group.indices = playlist_m3u_song_indices;
    playlist_m3u_group.count = found;
}

/* Shows an .m3u's contents the same way Favorites/Most Played do -- a
 * tappable song list (show_group_songs_editable()), not straight into
 * playback -- rather than the old behavior of jumping directly to track 0
 * the moment the playlist row itself was tapped, matching every other entry
 * point into a group of songs elsewhere in this screen (Artists/Albums/
 * Album Artist all list first too). m3u_path is passed through as the
 * editable-playlist marker so the Edit/remove UI (group_songs_edit_m3u_path,
 * see its own comment) only ever shows up here, not for Favorites/Most
 * Played/Artists/Albums. */
static void show_m3u_playlist(const char * name, const char * m3u_path, char ** paths, int count) {
    rebuild_playlist_m3u_group(name, paths, count);
    show_group_songs_editable(&playlist_m3u_group, m3u_path);
}

/* Remove-icon handler for an editable playlist's rows (see
 * populate_group_songs_rows() -- forward-declared near
 * group_songs_edit_m3u_path). Rewrites the underlying .m3u file, then
 * re-reads it back and redraws in place (no nav_push -- this stays on the
 * same group_songs_screen instance, just with the file's new contents)
 * rather than trusting the in-memory indices array to still match the file
 * after the rewrite. */
static void group_song_remove_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    if (!group_songs_edit_m3u_path || pos < 0 || pos >= group_songs_count) return;

    const char * m3u_path = group_songs_edit_m3u_path;
    playlist_files_remove(m3u_path, all_songs_paths[group_songs_indices[pos]]);

    char ** songs = NULL;
    int count = 0;
    file_browser_build_playlist_from_m3u(m3u_path, &songs, &count); /* false (empty playlist) leaves songs/count at 0, nothing allocated -- handled below */

    if (count == 0) {
        /* Empty playlists are auto-deleted rather than left around as a
         * stuck row nothing can open again -- this is the normal, in-app
         * way a playlist becomes empty (removing its last song). m3u_path
         * (== group_songs_edit_m3u_path) is a pointer into
         * playlists_m3u_paths, so delete before populate_playlists_screen()
         * below frees/rebuilds that array out from under it. */
        playlist_files_delete(m3u_path);
        metadata_db_playlist_delete_one(m3u_path);
        populate_playlists_screen(); /* not auto-refreshed by nav_pop() -- see build_playlists_screen()'s own comment */
        nav_pop();
        show_error_toast("Playlist deleted (last song removed)");
        return;
    }

    rebuild_playlist_m3u_group(NULL, songs, count);
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);

    group_songs_indices = playlist_m3u_group.indices;
    group_songs_count = playlist_m3u_group.count;
    populate_group_songs_rows();
    show_error_toast("Removed from playlist");
}

static void playlist_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);

    if (index == 0) {
        show_favorites();
        return;
    }
    if (index == 1) {
        show_most_played();
        return;
    }
    if (index == 2) {
        open_queue_screen();
        return;
    }

    int m3u_index = index - 3;
    if (m3u_index < 0 || m3u_index >= playlists_m3u_count) return;

    char ** songs;
    int count;
    if (!file_browser_build_playlist_from_m3u(playlists_m3u_paths[m3u_index], &songs, &count)) {
        /* False here means either "genuinely can't open" (leave it alone --
         * could be a transient SD-card issue, not the playlist's fault) or
         * "opened fine, zero playable entries" (a stale empty playlist,
         * e.g. one that predates the auto-delete-on-last-removal fix in
         * group_song_remove_row_cb() -- safe to clean up now that it's been
         * tapped). fopen() success alone distinguishes the two without
         * risking deleting a file we merely failed to read. */
        FILE * probe = fopen(playlists_m3u_paths[m3u_index], "r");
        if (probe) {
            fclose(probe);
            playlist_files_delete(playlists_m3u_paths[m3u_index]);
            metadata_db_playlist_delete_one(playlists_m3u_paths[m3u_index]);
            populate_playlists_screen();
            show_error_toast("Empty playlist deleted");
        } else {
            show_error_toast("Playlist is empty or unreadable");
        }
        return;
    }
    show_m3u_playlist(basename_of(playlists_m3u_paths[m3u_index]), playlists_m3u_paths[m3u_index], songs, count);
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);
}

/* Playlists' own row shape -- plain rounded rect (LIST_ROW_* look, same as
 * Artists/Albums/All Songs/Files) at LIST_ROW_WIDTH_WIDE rather than
 * add_pill_row_base()'s shared touch_list/item_bg.png pill: that PNG draws
 * at its native size regardless of the row's own width (see LIST_ROW_WIDTH's
 * own comment), so widening a pill row would've just left dead space around
 * an unchanged-size graphic instead of an actually-bigger tap target.
 * add_pill_row_base() itself stays untouched -- it's shared with a dozen
 * unrelated settings screens (Bluetooth DAC, Codec, Font Size, USB Mode,
 * ...) not part of this request. */
static lv_obj_t * add_playlist_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, LIST_ROW_HEIGHT);
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    return row;
}

/* Edit-mode delete icon for a user-created .m3u row -- never wired onto the
 * Favorites/Most Played rows (see populate_playlists_screen() below), since
 * neither is backed by a real file playlist_files_delete() could remove. */
static void playlist_delete_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int m3u_index = (int) (intptr_t) lv_event_get_user_data(e);
    if (m3u_index < 0 || m3u_index >= playlists_m3u_count) return;

    playlist_files_delete(playlists_m3u_paths[m3u_index]);
    metadata_db_playlist_delete_one(playlists_m3u_paths[m3u_index]);
    populate_playlists_screen();
    show_error_toast("Playlist deleted");
}

static void playlists_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    playlists_edit_mode = !playlists_edit_mode;
    populate_playlists_screen();
}

static void populate_playlists_screen(void) {
    lv_obj_clean(playlists_list);

    for (int i = 0; i < playlists_m3u_count; i++) free(playlists_m3u_paths[i]);
    free(playlists_m3u_paths);
    playlists_m3u_paths = NULL;
    playlists_m3u_count = 0;
    /* Persistent cache read (metadata_db.c), not a live playlist_files_scan()
     * -- real-device feedback: this screen took up to 5s to open because it
     * re-walked the whole SD card every visit. See rescan_playlists()'s own
     * comment for where the cache actually gets refreshed. */
    metadata_db_load_all_playlists(&playlists_m3u_paths, &playlists_m3u_count);

    lv_label_set_text(playlists_edit_btn, playlists_edit_mode ? "Done" : "Edit");

    /* Favorites/Most Played are never deletable (neither is a real .m3u
     * file -- see playlist_row_click_cb()'s index==0/1 special cases), so
     * edit mode just makes them inert instead of showing a delete icon
     * that would have nothing to act on. Same "row does nothing, only the
     * per-row action works" convention as populate_group_songs_rows()'s
     * own edit mode. */
    lv_obj_t * favorites_row = add_playlist_row_base(playlists_list, "Favorites");
    if (!playlists_edit_mode) {
        lv_obj_add_flag(favorites_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(favorites_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 0);
    }

    lv_obj_t * most_played_row = add_playlist_row_base(playlists_list, "Most Played");
    if (!playlists_edit_mode) {
        lv_obj_add_flag(most_played_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(most_played_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 1);
    }

    /* Never deletable either -- same treatment as Favorites/Most Played
     * above, see playlist_row_click_cb()'s index==2 case. */
    lv_obj_t * queue_row = add_playlist_row_base(playlists_list, "Queue");
    if (!playlists_edit_mode) {
        lv_obj_add_flag(queue_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(queue_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 2);
    }

    for (int i = 0; i < playlists_m3u_count; i++) {
        lv_obj_t * row = add_playlist_row_base(playlists_list, basename_of(playlists_m3u_paths[i]));
        if (playlists_edit_mode) {
            lv_obj_t * delete_icon = lv_image_create(row);
            lv_image_set_src(delete_icon, asset_path("touch_list/del.png"));
            lv_obj_align(delete_icon, LV_ALIGN_RIGHT_MID, -20, 0);
            lv_obj_add_flag(delete_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(delete_icon, playlist_delete_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (3 + i));
        }
    }
}

static lv_obj_t * build_playlists_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * scr = build_subsonic_list_screen("Playlists", &title_label, &playlists_list);

    /* Rows here are LIST_ROW_WIDTH_WIDE (see add_playlist_row_base()), wider
     * than build_subsonic_list_screen()'s own default 448px pill rows --
     * explicit cross-axis centering scoped to just this screen instance so
     * the wider rows are guaranteed centered rather than relying on
     * whatever the shared builder's own (untouched, ~20-screens-shared)
     * default flex alignment happens to be. */
    lv_obj_set_flex_align(playlists_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    /* Attached directly onto this specific screen instance rather than
     * threaded through build_subsonic_list_screen()'s own parameters --
     * that builder is shared by ~20 unrelated screens, same established
     * pattern as build_wifi_screen()'s Rescan button and the Subsonic
     * Download buttons. */
    playlists_edit_btn = lv_label_create(scr);
    lv_label_set_text(playlists_edit_btn, "Edit");
    lv_obj_set_style_text_color(playlists_edit_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(playlists_edit_btn, ui_size_20, 0);
    lv_obj_align(playlists_edit_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(playlists_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(playlists_edit_btn, playlists_edit_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

/* Settings > Update Music Database: reruns library_scan_once() (a full
 * rescan + tag re-read of MUSIC_ROOT_DIR, same as gui_init's startup call)
 * on a background thread -- a real library's tag reads are too slow to do
 * on the UI thread, same reasoning as every other pthread_create() in this
 * file -- then rebuilds the four prebuilt library screens (All Songs,
 * Artists, Albums, Album Artist), since unlike group_songs_screen/
 * artist_albums_screen (which rebuild their rows on every visit) these are
 * only ever built once at startup and would otherwise keep showing rows
 * bound to indices into the now-freed pre-rescan arrays. Playlists isn't
 * one of these four -- see its own build_playlists_screen()/populate_
 * playlists_screen() comment for why it doesn't need rebuilding here. */
static pthread_t library_rescan_thread;
static bool library_rescan_active = false;
static volatile bool library_rescan_done_flag = false;

static void * library_rescan_thread_func(void * arg) {
    (void) arg;
    library_scan_once();
    library_rescan_done_flag = true; /* written last -- update_timer_cb only checks this flag */
    return NULL;
}

static void start_library_rescan(void) {
    library_rescan_done_flag = false;
    library_rescan_active = true;
    lv_label_set_text(subsonic_downloading_label, "Updating\nmusic database...");
    lv_bar_set_value(subsonic_downloading_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(subsonic_downloading_progress_bar, LV_OBJ_FLAG_HIDDEN);
    nav_push(subsonic_downloading_screen);
    pthread_create(&library_rescan_thread, NULL, library_rescan_thread_func, NULL);
}

/* Rebuilds the four prebuilt library screens (All Songs, Artists, Albums,
 * Album Artist) against whatever's now in all_songs_paths/artist_groups/
 * album_groups/album_artist_groups -- unlike group_songs_screen/
 * artist_albums_screen (which rebuild their rows on every visit) these are
 * only ever built once at startup and would otherwise keep showing rows
 * bound to indices into freed arrays after ANY reload of the underlying
 * data, not just a full rescan -- shared by poll_library_rescan()'s
 * background-scan-completion path below and reload_library_on_sd_reinsert()'s
 * fast-cache-load path further down, both of which replace that data via a
 * different route (library_scan_once() vs library_load_from_cache_only())
 * but need the exact same screen-side cleanup afterward. playlists_screen
 * deliberately NOT rebuilt here -- see poll_library_rescan()'s own former
 * comment on why (still applies verbatim): its content is recomputed fresh
 * on every visit already, never stale to begin with. */
static void refresh_library_screens_after_reload(void) {
    lv_obj_delete(all_songs_screen);
    lv_obj_delete(artists_screen);
    lv_obj_delete(albums_screen);
    lv_obj_delete(album_artist_screen);
    all_songs_screen = build_all_songs_screen();
    sync_remote_control_library();
    artists_screen = build_artists_screen();
    albums_screen = build_albums_screen();
    album_artist_screen = build_album_artist_screen();

    /* The four old screens' A-Z index bindings (strip/popup/list pointers)
     * just went dangling along with the lv_obj_delete()s above -- re-register
     * against the freshly rebuilt screens/lists before anything can poll them. */
    az_index_registered_count = 0;
    register_az_index(artists_screen, artists_list, artist_group_name_of, &artist_group_count);
    register_az_index(albums_screen, albums_list, album_group_name_of, &album_group_count);
    register_az_index(album_artist_screen, album_artist_list, album_artist_group_name_of, &album_artist_group_count);
    register_az_index(all_songs_screen, all_songs_list, all_songs_sorted_name_of, &all_songs_count);

    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, artist_group_name_of, &artist_group_count, false);
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, album_group_name_of, &album_group_count, false);
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, album_artist_group_name_of,
                     &album_artist_group_count, false);
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, all_songs_sorted_name_of, &all_songs_count, false);
}

/* SD-card-reinsertion fast path -- see poll_sd_card_hotplug()'s own call
 * site. Real-device feature request: reinserting a card the app has
 * already scanned before was recreating/re-reading its whole tag cache
 * from scratch every single time (a full start_library_rescan(), same
 * multi-minute cost as a genuinely new library, since that's the only path
 * poll_sd_card_hotplug() ever used on the mount edge) instead of just
 * loading the cache that same card's own root already carries (metadata_db.c's
 * METADATA_DB_PATH lives ON the SD card itself, not somewhere device-global
 * -- see its own comment -- so a previously-scanned card reinserted here
 * has its own already-populated database sitting right there).
 * library_load_from_cache_only() (a bounded SQLite read, not a filesystem
 * walk -- see its own comment) already does exactly this; the only thing
 * missing was calling it here instead of unconditionally reaching for
 * start_library_rescan(). Falls through to that full scan only if the
 * cache load comes back with nothing -- a genuinely new/never-scanned
 * card, or one whose database failed to open. A plain toast rather than
 * the "Updating music database..." screen start_library_rescan() shows --
 * this is a reload the user didn't explicitly ask for, over before they'd
 * even notice a progress screen, so pushing/popping that screen would just
 * yank them away from whatever they're actually doing right now for no
 * reason. */
static void reload_library_on_sd_reinsert(void) {
    library_load_from_cache_only();
    if (all_songs_count > 0) {
        refresh_library_screens_after_reload();
        show_info_toast("Library updated");
    } else {
        start_library_rescan();
    }
}

/* How long the "Library updated" success message stays up once a rescan
 * finishes, before falling back to Home -- purely so the user gets a
 * moment to actually read it, not a wait for anything real. */
#define LIBRARY_RESCAN_SUCCESS_MS 1500
static bool library_rescan_success_pending = false;
static uint32_t library_rescan_success_since_tick = 0;

static void poll_library_rescan(void) {
    /* Keep the screen awake for the whole "Updating music database..."/
     * "Library updated" window -- label and progress-bar text updates
     * don't touch LVGL's own indev-driven inactivity clock (no touch, no
     * hw button), so without this the auto screen-timeout would fire mid-
     * scan on a short timeout setting same as it would on any other idle
     * screen. Real-device feedback: with the screen-timeout fix that made
     * touch no longer wake an auto-slept screen (only the power button
     * does now), that read as the whole device freezing -- a dark,
     * touch-unresponsive screen mid-rescan looks identical to a genuine
     * hang from the outside. */
    if (library_rescan_active || library_rescan_success_pending) {
        lv_display_trigger_activity(NULL);
    }

    if (library_rescan_success_pending) {
        if (lv_tick_elaps(library_rescan_success_since_tick) >= LIBRARY_RESCAN_SUCCESS_MS) {
            library_rescan_success_pending = false;
            nav_reset_to_home(); /* leaves the busy screen and discards any stale deeper screen */
        }
        return;
    }

    if (!library_rescan_active) return;

    if (!library_rescan_done_flag) {
        /* Still scanning -- total stays 0 until the initial file walk
         * finishes (see library_scan_once()), so there's nothing
         * meaningful to show yet in that window; the label just keeps
         * reading "Updating music database..." until then. */
        if (library_scan_progress_total > 0) {
            lv_label_set_text_fmt(subsonic_downloading_label, "Updating\nmusic database...\n%d / %d",
                                   library_scan_progress_done, library_scan_progress_total);
            lv_bar_set_value(subsonic_downloading_progress_bar,
                              (int32_t) ((int64_t) library_scan_progress_done * 100 / library_scan_progress_total),
                              LV_ANIM_OFF);
        }
        return;
    }

    library_rescan_active = false;
    pthread_join(library_rescan_thread, NULL);

    refresh_library_screens_after_reload();

    lv_obj_add_flag(subsonic_downloading_progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(subsonic_downloading_label, "Library updated\n%d songs", all_songs_count);
    library_rescan_success_pending = true;
    library_rescan_success_since_tick = lv_tick_get();
}

/* SD mount-failure detection + Format SD Card -- see poll_sd_card_hotplug()
 * below for the detection/debounce logic and sd_format_card_worker() further
 * down for the actual format sequence. These three are declared here
 * (rather than alongside their real definitions further down) so that both
 * halves of the feature -- the gated detection logic below, and the
 * ungated popups/background thread that come after it in this file -- can
 * see them regardless of which one is defined first. */
static void show_sd_mount_failed_popup(void); /* defined below, alongside its popup */
static bool sd_mount_fail_notified = false;
static volatile bool sd_format_active = false;

#ifndef HOST_BUILD
/* Real-device bug report: reinserting the SD card while the device is
 * already on doesn't populate its files in the player. Root cause -- see
 * main.c's own mount_sd_card_if_needed() comment for the full real-device
 * investigation: this firmware has NO hotplug mechanism at all for the
 * internal SD card slot (confirmed by reading every relevant piece of
 * config -- /etc/mdev.conf only has a rule for external USB mass-storage
 * sd[a-z] devices, not the internal mmcblk* card; no fstab entry; no
 * init.d script). Something else on the system does eventually retry
 * mounting it reactively, but confirmed (via dmesg) to take on the order
 * of many minutes, and even once mounted, this app itself never notices --
 * only the user-triggered Settings > Update Music Database rescan reads
 * the SD card, and nothing was polling for "did it just become mounted"
 * to trigger that automatically.
 *
 * This polls whether MUSIC_ROOT_DIR is currently a real mount point (its
 * st_dev differs from its parent /data/mnt's -- the standard POSIX "is
 * this a mountpoint" check, since an unmounted MUSIC_ROOT_DIR is just an
 * empty directory living directly on /data/mnt's own filesystem). While
 * not mounted, it retries mount_sd_card_if_needed() itself every few
 * seconds instead of waiting on whatever slow reactive mechanism the OS
 * has -- harmless to call when nothing's inserted (see that function's own
 * comment: it just fails silently). On the unmounted -> mounted edge, it
 * kicks off the same rescan Settings > Update Music Database triggers, so
 * a card inserted after boot actually shows up without the user needing
 * to know to go find that button.
 *
 * Real-device feature request (2026-08-08): removal wasn't handled at all
 * symmetrically -- pulling the card left All Songs/Artists/Albums/
 * Playlists, and the Files screen, still showing entries for files that no
 * longer exist (tapping one would just fail to play), and there was no way
 * back to a correct state short of a manual Settings > Update Music
 * Database rescan. The mounted -> unmounted edge below now fires the same
 * rescan (correctly collapsing to an empty library against an empty
 * mountpoint) plus a Files-screen reset, mirroring the insertion side.
 *
 * That edge relies on sd_card_root_is_mounted() actually flipping to false
 * on a physical eject, which isn't guaranteed on its own: this firmware has
 * no hotplug mechanism at all for this slot (see above), so nothing ever
 * calls umount() when the card is pulled -- the VFS mount entry for
 * MUSIC_ROOT_DIR can just sit there claiming to still be mounted (same
 * st_dev) with reads underneath it now failing at the I/O layer instead.
 * sd_card_device_node_present() checks for the card's block device node
 * separately -- mdev creates/removes /dev/mmcblk0p1 directly off the
 * kernel's own uevents for the mmc host controller's card-detect line, a
 * baseline mdev behavior independent of the custom per-device *rules*
 * mdev.conf lacks for mmcblk* (those only add extra actions on top, they're
 * not what makes the node itself appear/disappear) -- so the node going
 * away is a real, kernel-driven signal of physical removal even though
 * nothing above the block layer reacts to it. When it disappears while
 * still nominally mounted, this self-issues the same `umount` this
 * firmware's own mass_storage_removing.sh uses for external USB media, so
 * the ordinary mounted -> unmounted edge below fires correctly on the very
 * next poll instead of never firing at all. Unverified against a real
 * eject on this exact device/kernel at the time this was written (no live
 * unit on hand) -- flagged for a real removal-cycle test rather than
 * asserted as confirmed working. */
#define SD_CARD_MOUNT_POLL_SECONDS 5

/* Consecutive failed poll cycles (SD_CARD_MOUNT_POLL_SECONDS apart) before
 * the mount-failure popup shows -- 6 * 5s = 30s of genuinely stuck retries,
 * matching this file's own LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS in spirit
 * (long enough that the kernel's own brief card-enumeration window right
 * after physical insertion can't false-positive, short enough the user
 * isn't left staring at an empty library wondering what's wrong). */
#define SD_MOUNT_FAIL_STREAK_THRESHOLD 6

static bool sd_card_root_is_mounted(void) {
    struct stat parent_st, root_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat(MUSIC_ROOT_DIR, &root_st) != 0) return false;
    return parent_st.st_dev != root_st.st_dev;
}

static bool sd_card_device_node_present(void) {
    struct stat st;
    /* Either node counts as "still present" -- mount_sd_card_if_needed()
     * (main.c) now falls back to the whole-disk node for a partition-less
     * card (see its own comment), so a card mounted that way only ever has
     * a /dev/mmcblk0 node, never a p1 one. Checking p1 alone here would
     * make this wrongly conclude such a card had been physically removed
     * on the very next poll after it successfully mounted, force-unmounting
     * a card that's still sitting right there. */
    return stat("/dev/mmcblk0p1", &st) == 0 || stat("/dev/mmcblk0", &st) == 0;
}

/* Whole-disk node, as opposed to sd_card_device_node_present()'s first-
 * partition node above -- present whenever a card is physically inserted
 * regardless of whether it has a partition table at all, so this is what
 * distinguishes "no card" from "card inserted but can't be mounted" for the
 * mount-failure/Format SD Card flow below (mount_sd_card_if_needed() itself
 * -- see main.c -- already tries every filesystem type this platform
 * actually supports, so a card that's physically present but still never
 * mounts genuinely needs a repartition/reformat, not just a different -t). */
static bool sd_card_base_device_present(void) {
    struct stat st;
    return stat("/dev/mmcblk0", &st) == 0;
}

static void poll_sd_card_hotplug(void) {
    /* Starts true: mount_sd_card_if_needed() already ran once at boot
     * (main.c, before gui_init()) and the initial library load already
     * happened against whatever was mounted by then -- assuming "already
     * mounted" here avoids this poll re-triggering a redundant rescan on
     * its very first tick when the card was present all along. If it
     * wasn't actually mounted yet at that point, the very next branch
     * below correctly falls into the "not mounted" case anyway and starts
     * retrying/rescanning from there. */
    static bool was_mounted = true;
    static time_t last_check = 0;
    static int mount_fail_streak = 0;
    /* Real-device testing: a plain single-poll "was mounted, now isn't"
     * edge fires the removal-collapse rescan below too eagerly -- confirmed
     * live that mount_sd_card_if_needed() (called synchronously, right
     * above that rescan's own pthread_create()) can complete a re-mount
     * faster than this function's own SD_CARD_MOUNT_POLL_SECONDS poll
     * interval, so a brief unmount (this app's own umount -l retry above,
     * or -- unverified but plausible -- a very fast physical reseat) can
     * have already remounted again by the time the removal rescan's
     * background thread actually gets scheduled, making it scan and fully
     * re-tag the *already-back* real card instead of correctly finding
     * nothing, exactly the slow full-rescan reload_library_on_sd_reinsert()
     * below exists to avoid. Requiring the unmounted state to be seen on
     * SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD consecutive polls (reset to 0 the
     * moment "mounted" is seen again, see the mounted branch below) before
     * acting filters that out -- a genuine removal stays gone far longer
     * than this short confirmation window, so real removal handling is
     * delayed by only a few seconds, not skipped. */
    static int unmount_confirm_streak = 0;
#define SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD 2

    time_t now = time(NULL);
    if (last_check != 0 && now - last_check < SD_CARD_MOUNT_POLL_SECONDS) return;
    last_check = now;

    bool mounted = sd_card_root_is_mounted();
    if (mounted && !sd_card_device_node_present()) {
        /* -l (lazy): the node's already gone, so there's no real device
         * left to flush to, and a plain umount would fail with EBUSY if
         * this app (or anything else) still has an fd open on a file under
         * MUSIC_ROOT_DIR from right before the card was pulled -- the same
         * reasoning mass_storage_removing.sh already applies to external
         * USB media on this firmware (see its own "umount -l" call). */
        char * argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
        subprocess_run(argv, NULL, 0);
        mounted = false;
    }

    if (!mounted) {
        mount_sd_card_if_needed();
        if (was_mounted) {
            unmount_confirm_streak++;
            if (unmount_confirm_streak >= SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD && !library_rescan_active) {
                /* Real-device bug report: metadata_db_open() only ever
                 * checks `if (db) return;` -- left open across a removal,
                 * it would keep reusing this now-dead connection (pointing
                 * at a file on a card that's physically gone) forever
                 * after, even across a later reinsertion of the same or a
                 * different card, silently defeating
                 * reload_library_on_sd_reinsert()'s own cache-load fast
                 * path below (a query against a dead handle isn't a real
                 * cache hit). Closing it here -- guarded by the same
                 * !library_rescan_active this branch already requires, so
                 * there's no background rescan thread concurrently using
                 * the connection -- makes the next metadata_db_open()
                 * (from whichever path runs on the next insertion)
                 * genuinely reopen fresh against whatever's actually
                 * mounted by then. */
                metadata_db_close();
                start_library_rescan();
                file_browser_reset_to_root();
                was_mounted = false;
                unmount_confirm_streak = 0;
            }
            /* Else: not yet confirmed (or a rescan from something else,
             * e.g. Settings > Update Music Database, is already running)
             * -- was_mounted deliberately stays true, so a flicker that
             * remounts before reaching the threshold below is
             * indistinguishable from nothing having happened at all: the
             * mounted branch's own streak reset plus its own
             * `!was_mounted` reinsertion check (still false) mean neither
             * edge ever fires. */
        }

        /* Card is physically there (the whole-disk node exists) but still
         * won't mount after repeated retries. mount_sd_card_if_needed()
         * (main.c) already tries every supported filesystem type against
         * both the first-partition node and, as a fallback, the whole-disk
         * node itself (covering a "superfloppy" card with no partition
         * table/MBR at all) -- so reaching here means either the partition
         * exists but holds something none of those attempts could mount
         * (corruption, or a filesystem this platform genuinely doesn't
         * support), or the raw disk itself does too. Both need the same fix
         * -- reformat -- so this doesn't try to tell them apart. Debounced
         * (SD_MOUNT_FAIL_STREAK_THRESHOLD
         * consecutive failed polls, SD_CARD_MOUNT_POLL_SECONDS apart) so
         * the kernel's own brief card-enumeration window right after
         * physical insertion -- where the node legitimately isn't there
         * *yet* -- doesn't fire a false alarm; not re-shown on every poll
         * once shown once (sd_mount_fail_notified), so the user isn't
         * nagged again until the card actually changes (removed, or a
         * format attempt runs -- both reset it below/in poll_sd_format()). */
        if (sd_card_base_device_present()) {
            mount_fail_streak++;
            if (mount_fail_streak >= SD_MOUNT_FAIL_STREAK_THRESHOLD && !sd_mount_fail_notified && !sd_format_active) {
                sd_mount_fail_notified = true;
                show_sd_mount_failed_popup();
            }
        } else {
            mount_fail_streak = 0;
            sd_mount_fail_notified = false;
        }
        return;
    }

    mount_fail_streak = 0;
    sd_mount_fail_notified = false;
    unmount_confirm_streak = 0; /* seeing "mounted" again cancels any not-yet-confirmed removal */
    if (!was_mounted && !library_rescan_active) {
        reload_library_on_sd_reinsert();
        file_browser_reset_to_root();
    }
    was_mounted = true;
}
#else
static void poll_sd_card_hotplug(void) {
}
#endif

/* Actual "Format SD Card" sequence -- runs on a background thread (see
 * start_sd_format()/poll_sd_format() below), since fdisk/mkdosfs can take
 * real time on a slow card and this must never block the UI thread.
 *
 * UNVERIFIED ON REAL HARDWARE. Every command here was chosen from static
 * analysis of the real firmware's own busybox binary (`strings` confirmed
 * mkdosfs/fdisk/dd/umount/partprobe are all compiled-in applets, including
 * fdisk's own interactive prompt text, proving a standard util-linux-style
 * command menu), not from an actual run -- this is a MIPS binary and can't
 * be executed on the dev host, and no live device was available while this
 * was written. Test with a spare/non-critical card before trusting it with
 * a card that matters.
 *
 * Sequence: best-effort unmount, zero the first sector (so fdisk always
 * sees a blank disk regardless of whatever partition table, or lack of
 * one, the card came in with -- avoids needing to conditionally script
 * fdisk's own delete-partition flow for "already has a stale/foreign
 * table"), script fdisk to create one primary partition spanning the whole
 * disk typed W95 FAT32 (LBA), partprobe to force the kernel to notice the
 * new partition, wait for its device node to appear, mkdosfs it, then hand
 * off to the normal mount path and confirm it actually mounted. */
#ifndef HOST_BUILD
static bool sd_format_card_worker(void) {
    char * umount_argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
    subprocess_run(umount_argv, NULL, 0);

    if (!sd_card_base_device_present()) return false; /* card pulled before/during format */

    char * dd_argv[] = { (char *) "dd", (char *) "if=/dev/zero", (char *) "of=/dev/mmcblk0",
                          (char *) "bs=512", (char *) "count=1", NULL };
    if (!subprocess_run_timeout(dd_argv, NULL, 0, 15000)) return false;

    pid_t fdisk_pid;
    int fdisk_write_fd;
    char * fdisk_argv[] = { (char *) "fdisk", (char *) "/dev/mmcblk0", NULL };
    if (!subprocess_popen_stdin(fdisk_argv, &fdisk_pid, &fdisk_write_fd)) return false;
    /* n(ew) -> p(rimary) -> partition 1 -> default first sector [Enter] ->
     * default last sector [Enter] -> t(ype) -> c (W95 FAT32, LBA) -> w(rite)+exit. */
    const char * fdisk_cmds = "n\np\n1\n\n\nt\nc\nw\n";
    ssize_t ignored = write(fdisk_write_fd, fdisk_cmds, strlen(fdisk_cmds));
    (void) ignored;
    close(fdisk_write_fd);
    subprocess_terminate(fdisk_pid); /* reaps it -- `w` alone isn't guaranteed to have taken effect yet */

    char * partprobe_argv[] = { (char *) "partprobe", (char *) "/dev/mmcblk0", NULL };
    subprocess_run_timeout(partprobe_argv, NULL, 0, 10000);

    bool got_partition = false;
    for (int i = 0; i < 20 && !got_partition; i++) {
        if (sd_card_device_node_present()) { got_partition = true; break; }
        usleep(250000);
    }
    if (!got_partition) return false;

    char * mkdosfs_argv[] = { (char *) "mkdosfs", (char *) "-F", (char *) "32", (char *) "-n",
                               (char *) "MUSIC", (char *) "/dev/mmcblk0p1", NULL };
    if (!subprocess_run_timeout(mkdosfs_argv, NULL, 0, 120000)) return false;

    mount_sd_card_if_needed();
    return sd_card_root_is_mounted();
}
#else
static bool sd_format_card_worker(void) {
    return false;
}
#endif

static pthread_t sd_format_thread;
static volatile bool sd_format_done_flag = false;
static volatile bool sd_format_succeeded = false;

static void * sd_format_thread_func(void * arg) {
    (void) arg;
    sd_format_succeeded = sd_format_card_worker();
    sd_format_done_flag = true; /* written last -- poll_sd_format() only checks this flag */
    return NULL;
}

static void start_sd_format(void) {
    sd_format_done_flag = false;
    sd_format_active = true;
    lv_label_set_text(subsonic_downloading_label, "Formatting\nSD Card...");
    lv_obj_add_flag(subsonic_downloading_progress_bar, LV_OBJ_FLAG_HIDDEN); /* no meaningful sub-progress to show */
    nav_push(subsonic_downloading_screen);
    pthread_create(&sd_format_thread, NULL, sd_format_thread_func, NULL);
}

static void poll_sd_format(void) {
    if (!sd_format_active || !sd_format_done_flag) return;

    sd_format_active = false;
    pthread_join(sd_format_thread, NULL);
    nav_pop(); /* leave the "Formatting SD Card..." screen */

    if (sd_format_succeeded) {
        show_error_toast("SD card formatted");
        sd_mount_fail_notified = false; /* give the freshly-formatted card a clean slate */
        if (!library_rescan_active) {
            start_library_rescan();
            file_browser_reset_to_root();
        }
    } else {
        show_error_toast("SD card format failed");
    }
}

static lv_obj_t * sd_mount_failed_popup;
static lv_obj_t * sd_mount_failed_popup_backdrop;
static lv_obj_t * sd_format_confirm_popup;
static lv_obj_t * sd_format_confirm_popup_backdrop;

static void hide_sd_mount_failed_popup(void) {
    lv_obj_add_flag(sd_mount_failed_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sd_mount_failed_popup, LV_OBJ_FLAG_HIDDEN);
}

static void hide_sd_format_confirm_popup(void) {
    lv_obj_add_flag(sd_format_confirm_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sd_format_confirm_popup, LV_OBJ_FLAG_HIDDEN);
}

static void sd_mount_failed_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_mount_failed_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_format_confirm_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_format_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_mount_failed_format_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
    lv_obj_remove_flag(sd_format_confirm_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sd_format_confirm_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sd_format_confirm_popup_backdrop);
    lv_obj_move_foreground(sd_format_confirm_popup);
}

static void sd_format_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
    start_sd_format();
}

/* Only ever shown from the debounced detection in poll_sd_card_hotplug()
 * above -- deliberately has no permanent home in Settings, so it can't be
 * used to format a perfectly working card by mistake; it only exists at
 * all once a real, sustained mount failure has actually been observed. */
static void show_sd_mount_failed_popup(void) {
    lv_obj_remove_flag(sd_mount_failed_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sd_mount_failed_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sd_mount_failed_popup_backdrop);
    lv_obj_move_foreground(sd_mount_failed_popup);
}

static void build_sd_mount_failed_popup(void) {
    lv_obj_t * top = lv_layer_top();

    sd_mount_failed_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(sd_mount_failed_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(sd_mount_failed_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sd_mount_failed_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(sd_mount_failed_popup_backdrop, 0, 0);
    lv_obj_remove_flag(sd_mount_failed_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sd_mount_failed_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sd_mount_failed_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(sd_mount_failed_popup_backdrop, sd_mount_failed_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    sd_mount_failed_popup = lv_obj_create(top);
    lv_obj_set_size(sd_mount_failed_popup, 400, 300);
    lv_obj_align(sd_mount_failed_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(sd_mount_failed_popup, 16, 0);
    lv_obj_add_style(sd_mount_failed_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(sd_mount_failed_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sd_mount_failed_popup, 0, 0);
    lv_obj_remove_flag(sd_mount_failed_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sd_mount_failed_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(sd_mount_failed_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "SD card couldn't be read");

    lv_obj_t * body = lv_label_create(sd_mount_failed_popup);
    lv_obj_set_width(body, lv_pct(90));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(body, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(body, ui_size_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 58);
    lv_label_set_text(body,
        "It may have no partition table or a file system this player can't use. "
        "Formatting will erase it and set it up for this player.");

    lv_obj_t * format_row = lv_obj_create(sd_mount_failed_popup);
    lv_obj_set_size(format_row, lv_pct(90), 56);
    lv_obj_align(format_row, LV_ALIGN_TOP_MID, 0, 156);
    lv_obj_set_style_radius(format_row, 12, 0);
    lv_obj_set_style_bg_opa(format_row, 0, 0);
    lv_obj_set_style_border_width(format_row, 0, 0);
    lv_obj_remove_flag(format_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(format_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(format_row, sd_mount_failed_format_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * format_label = lv_label_create(format_row);
    lv_label_set_text(format_label, "Format SD Card");
    lv_obj_set_style_text_color(format_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(format_label, ui_size_20, 0);
    lv_obj_center(format_label);

    lv_obj_t * dismiss_row = lv_obj_create(sd_mount_failed_popup);
    lv_obj_set_size(dismiss_row, lv_pct(90), 56);
    lv_obj_align(dismiss_row, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_style_radius(dismiss_row, 12, 0);
    lv_obj_set_style_bg_opa(dismiss_row, 0, 0);
    lv_obj_set_style_border_width(dismiss_row, 0, 0);
    lv_obj_remove_flag(dismiss_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dismiss_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dismiss_row, sd_mount_failed_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * dismiss_label = lv_label_create(dismiss_row);
    lv_label_set_text(dismiss_label, "Dismiss");
    lv_obj_set_style_text_color(dismiss_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(dismiss_label, ui_size_20, 0);
    lv_obj_center(dismiss_label);
}

static void build_sd_format_confirm_popup(void) {
    lv_obj_t * top = lv_layer_top();

    sd_format_confirm_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(sd_format_confirm_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(sd_format_confirm_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sd_format_confirm_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(sd_format_confirm_popup_backdrop, 0, 0);
    lv_obj_remove_flag(sd_format_confirm_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sd_format_confirm_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sd_format_confirm_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(sd_format_confirm_popup_backdrop, sd_format_confirm_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    sd_format_confirm_popup = lv_obj_create(top);
    lv_obj_set_size(sd_format_confirm_popup, 400, 260);
    lv_obj_align(sd_format_confirm_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(sd_format_confirm_popup, 16, 0);
    lv_obj_add_style(sd_format_confirm_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(sd_format_confirm_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sd_format_confirm_popup, 0, 0);
    lv_obj_remove_flag(sd_format_confirm_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sd_format_confirm_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(sd_format_confirm_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "Erase and format SD card?");

    lv_obj_t * body = lv_label_create(sd_format_confirm_popup);
    lv_obj_set_width(body, lv_pct(90));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(body, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(body, ui_size_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 58);
    lv_label_set_text(body, "This permanently deletes everything on the card. This cannot be undone.");

    lv_obj_t * confirm_row = lv_obj_create(sd_format_confirm_popup);
    lv_obj_set_size(confirm_row, lv_pct(90), 56);
    lv_obj_align(confirm_row, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_radius(confirm_row, 12, 0);
    lv_obj_set_style_bg_opa(confirm_row, 0, 0);
    lv_obj_set_style_border_width(confirm_row, 0, 0);
    lv_obj_remove_flag(confirm_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(confirm_row, sd_format_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * confirm_label = lv_label_create(confirm_row);
    lv_label_set_text(confirm_label, "Format");
    lv_obj_set_style_text_color(confirm_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(confirm_label, ui_size_20, 0);
    lv_obj_center(confirm_label);

    lv_obj_t * cancel_row = lv_obj_create(sd_format_confirm_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_TOP_MID, 0, 182);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, sd_format_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, ui_size_20, 0);
    lv_obj_center(cancel_label);
}

/* Power-off countdown -- shown when hw_buttons_consume_power_long_press()
 * fires (see update_timer_cb()'s own consumer block). Same lv_layer_top()
 * overlay shape as sd_mount_failed_popup/sd_format_confirm_popup above,
 * built once and shown/hidden by flag rather than nav_push()'d, so it can
 * appear over whatever screen is currently active. Unlike those two, the
 * countdown itself isn't tied to the popup being visible for any particular
 * duration on its own -- poll_power_off_countdown() (called every tick from
 * update_timer_cb) drives it, and idle_shutdown_now() at zero doesn't
 * return on a real device, so there's no explicit "now power off" call site
 * beyond that. */
#define POWER_OFF_COUNTDOWN_SECONDS 3

static lv_obj_t * power_off_countdown_popup;
static lv_obj_t * power_off_countdown_popup_backdrop;
static lv_obj_t * power_off_countdown_label;
static bool power_off_countdown_active = false;
static uint32_t power_off_countdown_start_tick;

static void hide_power_off_countdown_popup(void) {
    lv_obj_add_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(power_off_countdown_popup, LV_OBJ_FLAG_HIDDEN);
}

static void cancel_power_off_countdown(void) {
    power_off_countdown_active = false;
    hide_power_off_countdown_popup();
}

static void power_off_countdown_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cancel_power_off_countdown();
}

static void power_off_countdown_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cancel_power_off_countdown();
}

static void start_power_off_countdown(void) {
    power_off_countdown_active = true;
    power_off_countdown_start_tick = lv_tick_get();
    lv_label_set_text_fmt(power_off_countdown_label, "%d", POWER_OFF_COUNTDOWN_SECONDS);
    lv_obj_remove_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(power_off_countdown_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(power_off_countdown_popup_backdrop);
    lv_obj_move_foreground(power_off_countdown_popup);
}

/* Called every tick from update_timer_cb while power_off_countdown_active --
 * updates the on-screen seconds-remaining label, and calls idle_shutdown_now()
 * once the countdown reaches zero (only reachable by letting it run out;
 * tapping Cancel or the backdrop aborts it first, via
 * cancel_power_off_countdown() above). */
static void poll_power_off_countdown(void) {
    if (!power_off_countdown_active) return;

    uint32_t elapsed_ms = lv_tick_elaps(power_off_countdown_start_tick);
    uint32_t total_ms = (uint32_t) POWER_OFF_COUNTDOWN_SECONDS * 1000;
    if (elapsed_ms >= total_ms) {
        power_off_countdown_active = false;
        idle_shutdown_now(); /* does not return on a real device */
        return;
    }

    int seconds_left = (int) ((total_ms - elapsed_ms + 999) / 1000);
    lv_label_set_text_fmt(power_off_countdown_label, "%d", seconds_left);
}

static void build_power_off_countdown_popup(void) {
    lv_obj_t * top = lv_layer_top();

    power_off_countdown_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(power_off_countdown_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(power_off_countdown_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(power_off_countdown_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(power_off_countdown_popup_backdrop, 0, 0);
    lv_obj_remove_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(power_off_countdown_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(power_off_countdown_popup_backdrop, power_off_countdown_backdrop_cb, LV_EVENT_CLICKED, NULL);

    power_off_countdown_popup = lv_obj_create(top);
    lv_obj_set_size(power_off_countdown_popup, 320, 280);
    lv_obj_align(power_off_countdown_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(power_off_countdown_popup, 16, 0);
    lv_obj_add_style(power_off_countdown_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(power_off_countdown_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(power_off_countdown_popup, 0, 0);
    lv_obj_remove_flag(power_off_countdown_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_off_countdown_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(power_off_countdown_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "Powering Off");

    power_off_countdown_label = lv_label_create(power_off_countdown_popup);
    lv_obj_add_style(power_off_countdown_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(power_off_countdown_label, ui_size_28, 0);
    lv_obj_align(power_off_countdown_label, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_text_fmt(power_off_countdown_label, "%d", POWER_OFF_COUNTDOWN_SECONDS);

    lv_obj_t * cancel_row = lv_obj_create(power_off_countdown_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, power_off_countdown_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, ui_size_20, 0);
    lv_obj_center(cancel_label);
}

static void update_music_database_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_library_rescan();
}

static void artists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(artists_screen);
}

static void albums_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(albums_screen);
}

static void album_artist_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(album_artist_screen);
}

static void playlists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    playlists_edit_mode = false; /* fresh entry from the menu always starts out of edit mode, same convention as show_group_songs_editable() */
    populate_playlists_screen();
    nav_push(playlists_screen);
}

static lv_obj_t * build_music_screen(void) {
    static icon_grid_item_t items[6];
    items[0] = (icon_grid_item_t){ "category/explorer.png", "category/explorer_s.png", "Files", music_files_tile_cb, NULL };
    items[1] = (icon_grid_item_t){ "category/artist.png", "category/artist_s.png", "Artists", artists_tile_cb, NULL };
    items[2] = (icon_grid_item_t){ "category/album.png", "category/album_s.png", "Albums", albums_tile_cb, NULL };
    items[3] = (icon_grid_item_t){ "category/album_artist.png", "category/album_artist_s.png", "Album Artist", album_artist_tile_cb, NULL };
    items[4] = (icon_grid_item_t){ "category/all.png", "category/all_s.png", "All Songs", all_songs_tile_cb, NULL };
    /* No dedicated "playlist" icon exists anywhere in the stock theme pack
     * (assets/theme2/category/ has album/album_artist/all/artist/explorer/
     * genre/item/net_radio and nothing else playlist-shaped) -- reusing
     * genre.png/genre_s.png here since Genres no longer has a tile of its
     * own to need it. */
    items[5] = (icon_grid_item_t){ "category/genre.png", "category/genre_s.png", "Playlists", playlists_tile_cb, NULL };
    lv_obj_t * scr = build_icon_grid_screen("Music", generic_back_cb, items, 6, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void subsonic_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(subsonic_entry_screen);
}

/* Shared click handler for every plugin-registered Stream Media tile below
 * -- user_data is the tile's index into plugin_manager's own
 * plugin_stream_tiles[] (not an LVGL object), same index-not-object shape
 * plugin_list_row_click_cb() already uses. */
static void plugin_stream_tile_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_stream_tile_clicked(index);
}

/* Subsonic is the one built-in, real, working option here -- Qobuz/Tidal
 * (paid regional subscriptions + a real API integration this project's
 * author couldn't personally test or verify against) and Net Radio (never
 * wired up past a placeholder) were removed rather than left as dead/
 * stalled tiles. Its icon (stream_media/subsonic.png/_s.png) isn't from
 * the stock theme pack at all (Subsonic isn't a stock HiBy feature) --
 * see assets.c's THEME_OVERRIDE_ROOT for how this app adds its own new
 * asset on top of the stock resource pack despite that living on
 * read-only storage on a real device.
 *
 * Any plugin.register_stream_media_tile() calls (a real Net Radio plugin,
 * for instance) are appended after Subsonic -- unlike Home (already full
 * at 6 tiles, can't scroll), Stream Media has real room: 1 built-in +
 * PLUGIN_MAX_STREAM_TILES caps the total at 6, the same "fills exactly 3
 * rows" ceiling proven out for Home. */
static lv_obj_t * build_stream_media_screen(void) {
    static icon_grid_item_t items[1 + PLUGIN_MAX_STREAM_TILES];
    items[0] = (icon_grid_item_t){ "stream_media/subsonic.png", "stream_media/subsonic_s.png", "Subsonic", subsonic_tile_cb, NULL };

    int count = 1;
    int plugin_count = plugin_manager_get_stream_tile_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_STREAM_TILES; i++) {
        items[count++] = (icon_grid_item_t){
            plugin_manager_get_stream_tile_icon(i), plugin_manager_get_stream_tile_icon_selected(i),
            plugin_manager_get_stream_tile_label(i), plugin_stream_tile_click_cb, (void *) (intptr_t) i
        };
    }

    lv_obj_t * scr = build_icon_grid_screen("Stream Media", generic_back_cb, items, count, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* ---- Wi-Fi settings screen ----
 *
 * Enable/disable toggle at top; everything else (info/manual entry/DNS,
 * memorized networks, available networks) only appears once Wi-Fi is on,
 * rebuilt into the one dynamic wifi_list container by populate_wifi_screen()
 * -- reachable both from the Wireless submenu's Wi-Fi tile and from a
 * long-press on the quick-access drawer's wifi icon (see
 * quick_drawer_wifi_long_press_cb near build_quick_drawer). Scan/connect/
 * disconnect/forget all share the same background-thread-plus-polled-flag
 * shape as the Subsonic connect/download machinery above, and reuse its
 * subsonic_downloading_screen/label as the shared "please wait" interstitial
 * rather than building a second one. */

#define WIFI_MAX_RESULTS 32
#define WIFI_MAX_SAVED 32
static wifi_network_t wifi_scan_results[WIFI_MAX_RESULTS];
static int wifi_scan_result_count = 0;
static wifi_saved_network_t wifi_saved_results[WIFI_MAX_SAVED];
static int wifi_saved_result_count = 0;
static lv_obj_t * wifi_list;
static char wifi_connect_pending_ssid[WIFI_MAX_SSID_LEN];

static void start_wifi_scan(void);

static pthread_t wifi_connect_thread;
static bool wifi_connect_active = false;
static volatile bool wifi_connect_done_flag = false;
static volatile bool wifi_connect_succeeded = false;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char password[256];
} wifi_connect_request_t;

static void * wifi_connect_thread_func(void * arg) {
    wifi_connect_request_t * req = (wifi_connect_request_t *) arg;
    bool accepted = wifi_control_connect(req->ssid, req->password[0] != '\0' ? req->password : NULL);
    free(req);

    /* wifi_control_connect() returning true only confirms wpa_cli accepted
     * the configuration, not that association actually happened -- a wrong
     * password fails silently at that layer (see its own doc comment).
     * Poll the real link state for a few seconds before calling this a
     * genuine success, same real-world wait the "Connecting to X..." screen
     * already covers. */
    bool associated = false;
    if (accepted) {
        for (int i = 0; i < 10 && !associated; i++) {
            int level;
            associated = wifi_get_status(&level);
            if (!associated) usleep(500000);
        }
    }
    wifi_connect_succeeded = accepted && associated;
    wifi_connect_done_flag = true; /* written last -- poll_wifi_connect only checks this flag */
    return NULL;
}

static void start_wifi_connect(const char * ssid, const char * password) {
    wifi_connect_request_t * req = malloc(sizeof(*req));
    snprintf(req->ssid, sizeof(req->ssid), "%s", ssid);
    snprintf(req->password, sizeof(req->password), "%s", password ? password : "");

    wifi_connect_done_flag = false;
    wifi_connect_active = true;

    lv_label_set_text_fmt(subsonic_downloading_label, "Connecting to\n%s...", ssid);
    nav_push(subsonic_downloading_screen);

    pthread_create(&wifi_connect_thread, NULL, wifi_connect_thread_func, req);
}

static void poll_wifi_connect(void) {
    if (!wifi_connect_active || !wifi_connect_done_flag) return;

    wifi_connect_active = false;
    pthread_join(wifi_connect_thread, NULL);
    nav_pop(); /* leave the connecting screen, back to the network list */
    if (!wifi_connect_succeeded) {
        show_error_toast("Couldn't connect to Wi-Fi network");
    }
    start_wifi_scan(); /* refresh so the list reflects the new connection state */
}

/* Reconnecting to an already-memorized network -- deliberately a separate
 * thread/flag pair from the plain connect above (not a shared "mode" on the
 * same one) rather than reusing wifi_connect_active's state, matching this
 * file's existing convention of one dedicated pair per background Wi-Fi
 * operation (scan/connect/disconnect/forget already each have their own).
 * See wifi_control_connect_saved()'s own doc comment for why this can't
 * just call start_wifi_connect(ssid, NULL) instead. */
static pthread_t wifi_connect_saved_thread;
static bool wifi_connect_saved_active = false;
static volatile bool wifi_connect_saved_done_flag = false;
static volatile bool wifi_connect_saved_succeeded = false;

typedef struct {
    int id;
} wifi_connect_saved_request_t;

static void * wifi_connect_saved_thread_func(void * arg) {
    wifi_connect_saved_request_t * req = (wifi_connect_saved_request_t *) arg;
    bool accepted = wifi_control_connect_saved(req->id);
    free(req);

    bool associated = false;
    if (accepted) {
        for (int i = 0; i < 10 && !associated; i++) {
            int level;
            associated = wifi_get_status(&level);
            if (!associated) usleep(500000);
        }
    }
    wifi_connect_saved_succeeded = accepted && associated;
    wifi_connect_saved_done_flag = true; /* written last -- poll_wifi_connect_saved only checks this flag */
    return NULL;
}

static void start_wifi_connect_saved(int id, const char * ssid) {
    wifi_connect_saved_request_t * req = malloc(sizeof(*req));
    req->id = id;

    wifi_connect_saved_done_flag = false;
    wifi_connect_saved_active = true;

    lv_label_set_text_fmt(subsonic_downloading_label, "Connecting to\n%s...", ssid);
    nav_push(subsonic_downloading_screen);

    pthread_create(&wifi_connect_saved_thread, NULL, wifi_connect_saved_thread_func, req);
}

static void poll_wifi_connect_saved(void) {
    if (!wifi_connect_saved_active || !wifi_connect_saved_done_flag) return;

    wifi_connect_saved_active = false;
    pthread_join(wifi_connect_saved_thread, NULL);
    nav_pop(); /* leave the connecting screen, back to the network list */
    if (!wifi_connect_saved_succeeded) {
        show_error_toast("Couldn't connect to Wi-Fi network");
    }
    start_wifi_scan(); /* refresh so the list reflects the new connection state */
}

static pthread_t wifi_disconnect_thread;
static bool wifi_disconnect_active = false;
static volatile bool wifi_disconnect_done_flag = false;

static void * wifi_disconnect_thread_func(void * arg) {
    (void) arg;
    wifi_control_disconnect();
    wifi_disconnect_done_flag = true; /* written last -- poll_wifi_disconnect only checks this flag */
    return NULL;
}

static void start_wifi_disconnect(void) {
    wifi_disconnect_done_flag = false;
    wifi_disconnect_active = true;
    pthread_create(&wifi_disconnect_thread, NULL, wifi_disconnect_thread_func, NULL);
}

static void poll_wifi_disconnect(void) {
    if (!wifi_disconnect_active || !wifi_disconnect_done_flag) return;
    wifi_disconnect_active = false;
    pthread_join(wifi_disconnect_thread, NULL);
    start_wifi_scan(); /* refresh so the list reflects the disconnected state */
}

static void wifi_password_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    start_wifi_connect(wifi_connect_pending_ssid, text);
}

static void wifi_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    wifi_network_t * net = &wifi_scan_results[index];
    if (net->is_current) {
        start_wifi_disconnect();
        return;
    }

    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", net->ssid);
    if (net->secured) {
        show_text_entry("Wi-Fi Password", NULL, true, false, wifi_password_entered_cb, NULL);
    } else {
        start_wifi_connect(net->ssid, NULL);
    }
}

static void wifi_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_wifi_scan();
}

static pthread_t wifi_scan_thread;
static bool wifi_scan_active = false;
static volatile bool wifi_scan_done_flag = false;

static void * wifi_scan_thread_func(void * arg) {
    (void) arg;
    /* No auto-enable here anymore -- the screen's own toggle row is now the
     * explicit way to turn Wi-Fi on, so a stray Rescan tap while it's off
     * should just find nothing rather than silently flipping the radio on
     * behind the toggle's back. */
    wifi_control_scan_start();
    sleep(3); /* wpa_cli's scan is async -- give the radio time before reading scan_results */
    wifi_scan_result_count = wifi_control_get_results(wifi_scan_results, WIFI_MAX_RESULTS);
    wifi_scan_done_flag = true; /* written last -- poll_wifi_scan only checks this flag */
    return NULL;
}

/* Runs fully in the background, same as the stock player -- no busy
 * screen, no navigation at all. The list just updates in place
 * (populate_wifi_screen(), below) once the scan finishes, whichever screen
 * the user happens to be on by then. An earlier version pushed a
 * "Scanning for networks..." interstitial shared with bt scan/the bt
 * toggle's own overlay -- see git history if that's ever needed again, but
 * it was also the source of a real, repeatedly-hit stuck-screen bug
 * (multiple uncoordinated users of one shared overlay, plus the overlay
 * getting buried under further navigation), which not having an overlay at
 * all sidesteps entirely. */
static void start_wifi_scan(void) {
    if (wifi_scan_active) return;

    wifi_scan_done_flag = false;
    wifi_scan_active = true;

    pthread_create(&wifi_scan_thread, NULL, wifi_scan_thread_func, NULL);
}

static void poll_wifi_scan(void) {
    if (!wifi_scan_active || !wifi_scan_done_flag) return;

    wifi_scan_active = false;
    pthread_join(wifi_scan_thread, NULL);
    populate_wifi_screen(wifi_control_is_enabled()); /* refresh the list either way -- worth keeping current even if the user already backed out */
}

static pthread_t wifi_forget_thread;
static bool wifi_forget_active = false;
static volatile bool wifi_forget_done_flag = false;

typedef struct {
    int id;
} wifi_forget_request_t;

static void * wifi_forget_thread_func(void * arg) {
    wifi_forget_request_t * req = (wifi_forget_request_t *) arg;
    wifi_control_forget(req->id);
    free(req);
    wifi_forget_done_flag = true; /* written last -- poll_wifi_forget only checks this flag */
    return NULL;
}

static void start_wifi_forget(int id) {
    wifi_forget_request_t * req = malloc(sizeof(*req));
    req->id = id;
    wifi_forget_done_flag = false;
    wifi_forget_active = true;
    pthread_create(&wifi_forget_thread, NULL, wifi_forget_thread_func, req);
}

static void poll_wifi_forget(void) {
    if (!wifi_forget_active || !wifi_forget_done_flag) return;
    wifi_forget_active = false;
    pthread_join(wifi_forget_thread, NULL);
    start_wifi_scan(); /* refresh both memorized and available sections */
}

/* ---- Wi-Fi saved-network action popup ------------------------------------
 * Real-device bug report: tapping a memorized network used to call
 * start_wifi_forget() directly and unconditionally -- there was no way to
 * just reconnect to one, and no confirmation before it got forgotten either.
 * Mirrors bt_action_popup's exact shape (same hand-built top-layer overlay,
 * same tap-outside-to-dismiss backdrop) for a consistent feel between the
 * two screens, per that same feedback ("just like on the bluetooth page").
 * Unlike Bluetooth's popup, both actions are always offered here -- a
 * memorized network is by definition not the live association status, so
 * "Connect" is always at least plausible (also covers reconnecting after a
 * manual disconnect), and "Forget" always applies to anything memorized. */
static lv_obj_t * wifi_action_popup;
static lv_obj_t * wifi_action_popup_backdrop;
static lv_obj_t * wifi_action_popup_title;
static lv_obj_t * wifi_action_connect_row;
static lv_obj_t * wifi_action_forget_row;
static int wifi_action_popup_network_index = -1;

static void hide_wifi_action_popup(void) {
    lv_obj_add_flag(wifi_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_action_popup, LV_OBJ_FLAG_HIDDEN);
    wifi_action_popup_network_index = -1;
}

static void wifi_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_wifi_action_popup();
}

static void wifi_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (wifi_action_popup_network_index < 0 || wifi_action_popup_network_index >= wifi_saved_result_count) {
        hide_wifi_action_popup();
        return;
    }
    wifi_saved_network_t * net = &wifi_saved_results[wifi_action_popup_network_index];
    int id = net->id;
    char ssid[WIFI_MAX_SSID_LEN];
    snprintf(ssid, sizeof(ssid), "%s", net->ssid);
    hide_wifi_action_popup();
    start_wifi_connect_saved(id, ssid);
}

static void wifi_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (wifi_action_popup_network_index < 0 || wifi_action_popup_network_index >= wifi_saved_result_count) {
        hide_wifi_action_popup();
        return;
    }
    int id = wifi_saved_results[wifi_action_popup_network_index].id;
    hide_wifi_action_popup();
    start_wifi_forget(id);
}

static void build_wifi_action_popup(void) {
    lv_obj_t * top = lv_layer_top();

    wifi_action_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(wifi_action_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(wifi_action_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_action_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(wifi_action_popup_backdrop, 0, 0);
    lv_obj_remove_flag(wifi_action_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_action_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(wifi_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(wifi_action_popup_backdrop, wifi_action_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    wifi_action_popup = lv_obj_create(top);
    lv_obj_set_size(wifi_action_popup, 400, 220);
    lv_obj_align(wifi_action_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(wifi_action_popup, 16, 0);
    lv_obj_add_style(wifi_action_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(wifi_action_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_action_popup, 0, 0);
    lv_obj_remove_flag(wifi_action_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_action_popup, LV_OBJ_FLAG_HIDDEN);

    wifi_action_popup_title = lv_label_create(wifi_action_popup);
    lv_obj_set_width(wifi_action_popup_title, lv_pct(90));
    lv_label_set_long_mode(wifi_action_popup_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(wifi_action_popup_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(wifi_action_popup_title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(wifi_action_popup_title, ui_size_22, 0);
    lv_obj_align(wifi_action_popup_title, LV_ALIGN_TOP_MID, 0, 20);

    wifi_action_connect_row = lv_obj_create(wifi_action_popup);
    lv_obj_set_size(wifi_action_connect_row, lv_pct(90), 56);
    lv_obj_set_style_radius(wifi_action_connect_row, 12, 0);
    lv_obj_set_style_bg_opa(wifi_action_connect_row, 0, 0);
    lv_obj_set_style_border_width(wifi_action_connect_row, 0, 0);
    lv_obj_remove_flag(wifi_action_connect_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_action_connect_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_action_connect_row, wifi_action_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(wifi_action_connect_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_t * connect_label = lv_label_create(wifi_action_connect_row);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_set_style_text_color(connect_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(connect_label, ui_size_20, 0);
    lv_obj_center(connect_label);

    wifi_action_forget_row = lv_obj_create(wifi_action_popup);
    lv_obj_set_size(wifi_action_forget_row, lv_pct(90), 56);
    lv_obj_set_style_radius(wifi_action_forget_row, 12, 0);
    lv_obj_set_style_bg_opa(wifi_action_forget_row, 0, 0);
    lv_obj_set_style_border_width(wifi_action_forget_row, 0, 0);
    lv_obj_remove_flag(wifi_action_forget_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_action_forget_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_action_forget_row, wifi_action_forget_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(wifi_action_forget_row, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_t * forget_label = lv_label_create(wifi_action_forget_row);
    lv_label_set_text(forget_label, "Forget");
    lv_obj_set_style_text_color(forget_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(forget_label, ui_size_20, 0);
    lv_obj_center(forget_label);
}

static void show_wifi_action_popup(int index) {
    wifi_action_popup_network_index = index;
    lv_label_set_text(wifi_action_popup_title, wifi_saved_results[index].ssid);

    lv_obj_remove_flag(wifi_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(wifi_action_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifi_action_popup_backdrop);
    lv_obj_move_foreground(wifi_action_popup);
}

static void wifi_saved_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_wifi_action_popup(index);
}

/* ---- Wi-Fi Info screen (read-only) ---- */

static lv_obj_t * wifi_info_screen;
static lv_obj_t * wifi_info_list;

static void populate_wifi_info_screen(void) {
    lv_obj_clean(wifi_info_list);

    wifi_info_t info;
    if (!wifi_control_get_info(&info)) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, "Not connected");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }

    static const char * signal_labels[] = { "Weak", "Fair", "Good", "Excellent" };
    int signal_index = (info.signal_level >= 0 && info.signal_level <= 3) ? info.signal_level : 0;
    char lines[5][80];
    snprintf(lines[0], sizeof(lines[0]), "SSID: %s", info.ssid);
    snprintf(lines[1], sizeof(lines[1]), "IP Address: %s", info.ip[0] ? info.ip : "-");
    snprintf(lines[2], sizeof(lines[2]), "Gateway: %s", info.gateway[0] ? info.gateway : "-");
    snprintf(lines[3], sizeof(lines[3]), "MAC Address: %s", info.mac[0] ? info.mac : "-");
    snprintf(lines[4], sizeof(lines[4]), "Signal: %s", signal_labels[signal_index]);

    for (int i = 0; i < 5; i++) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, lines[i]);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        lv_obj_set_style_pad_top(label, 12, 0);
    }
}

static lv_obj_t * build_wifi_info_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Wi-Fi Info", &title_label, &wifi_info_list);
}

static void open_wifi_info_screen(void) {
    populate_wifi_info_screen();
    nav_push(wifi_info_screen);
}

static void wifi_info_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_info_screen();
}

/* ---- Manual SSID entry -- chains the same show_text_entry() flow scan
 * results already use for a secured network's password, just starting from
 * a typed SSID instead of a tapped scan row. An empty password submission
 * connects as an open network (start_wifi_connect() already treats an empty
 * password as "no password"), so there's no separate "is this secured?"
 * question to ask up front. ---- */

static void wifi_manual_ssid_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", text);
    show_text_entry("Wi-Fi Password", NULL, true, false, wifi_password_entered_cb, NULL);
}

static void wifi_manual_entry_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Network Name (SSID)", NULL, false, false, wifi_manual_ssid_entered_cb, NULL);
}

/* ---- DNS settings screen -- view/edit the 2 servers currently in
 * /etc/resolv.conf (see wifi_control_get_dns()'s own doc comment for why
 * this is a "until the next DHCP renewal" override, not a permanent one).
 * ---- */

static lv_obj_t * wifi_dns_screen;
static lv_obj_t * wifi_dns_list;
static char wifi_dns_servers[2][16];
static int wifi_dns_server_count = 0;

static void wifi_dns_row_cb(lv_event_t * e);

static void populate_wifi_dns_screen(void) {
    lv_obj_clean(wifi_dns_list);

    char current[2][16];
    int count = wifi_control_get_dns(current, 2);
    for (int i = 0; i < 2; i++) {
        snprintf(wifi_dns_servers[i], sizeof(wifi_dns_servers[i]), "%s", i < count ? current[i] : "");
    }
    wifi_dns_server_count = count;

    static const char * slot_labels[2] = { "Primary DNS", "Secondary DNS" };
    for (int i = 0; i < 2; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%s: %s", slot_labels[i], wifi_dns_servers[i][0] ? wifi_dns_servers[i] : "Not set");
        lv_obj_t * row = add_pill_chevron_row(wifi_dns_list, text, NULL);
        lv_obj_add_event_cb(row, wifi_dns_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void wifi_dns_entered_cb(const char * text, void * user_data) {
    int slot = (int) (intptr_t) user_data;
    snprintf(wifi_dns_servers[slot], sizeof(wifi_dns_servers[slot]), "%s", text);
    if (slot + 1 > wifi_dns_server_count) wifi_dns_server_count = slot + 1;

    const char * servers[2] = { wifi_dns_servers[0], wifi_dns_servers[1] };
    wifi_control_set_dns(servers, wifi_dns_server_count);
    populate_wifi_dns_screen();
}

static void wifi_dns_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int slot = (int) (intptr_t) lv_event_get_user_data(e);
    show_text_entry(slot == 0 ? "Primary DNS" : "Secondary DNS", wifi_dns_servers[slot], false, false, wifi_dns_entered_cb,
                    (void *) (intptr_t) slot);
}

static lv_obj_t * build_wifi_dns_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("DNS Settings", &title_label, &wifi_dns_list);
}

static void open_wifi_dns_screen(void) {
    populate_wifi_dns_screen();
    nav_push(wifi_dns_screen);
}

static void wifi_dns_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_dns_screen();
}

/* ---- Master rebuild for the Wi-Fi screen's one dynamic list: toggle row,
 * then (only while enabled) the info/manual-entry/DNS rows, memorized
 * networks, and available networks -- called after every state change
 * (toggle, scan, connect, disconnect, forget) rather than patched
 * incrementally, same full-rebuild convention as every other list in this
 * file. ---- */
/* enabled is passed in rather than read via wifi_control_is_enabled()
 * internally so the tap-to-toggle handler (quick_drawer_wifi_event_cb) can
 * pass its own optimistic target state instead of the real (not-yet-caught-
 * up) one -- see that function's own comment. Every other caller just
 * passes the real wifi_control_is_enabled() value, same as before this was
 * a parameter. */
static void populate_wifi_screen(bool enabled) {
    lv_obj_clean(wifi_list);

    /* quick_drawer_wifi_event_cb is the SAME real toggle-thread trigger the
     * drawer's own wifi icon uses -- no drawer-specific logic in it, so
     * it's reused directly here rather than duplicating the thread-kickoff
     * boilerplate. poll_wifi_toggle() repopulates this screen once the
     * toggle completes. */
    add_pill_toggle_row(wifi_list, "Wi-Fi", enabled, quick_drawer_wifi_event_cb);

    if (!enabled) return;

    add_pill_chevron_row(wifi_list, "Wi-Fi Info", wifi_info_row_cb);
    add_pill_chevron_row(wifi_list, "Manual SSID Entry", wifi_manual_entry_row_cb);
    add_pill_chevron_row(wifi_list, "DNS Settings", wifi_dns_settings_row_cb);

    add_section_header(wifi_list, "Memorized Networks");
    wifi_saved_result_count = wifi_control_list_saved(wifi_saved_results, WIFI_MAX_SAVED);
    if (wifi_saved_result_count == 0) {
        lv_obj_t * label = lv_label_create(wifi_list);
        lv_label_set_text(label, "No memorized networks");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }
    /* Real-device bug report: no indicator of which saved network is
     * actually connected -- Available Networks below already shows this
     * (net->is_current, from the scan results), but the scan and the
     * saved-network list are two entirely separate queries (wpa_cli
     * status vs. list_networks), so nothing here ever cross-referenced
     * them. wifi_control_get_info() (already used by the Wi-Fi Info
     * screen) is the same "ask wpa_cli status directly" source of truth
     * as is_current itself, just reused here by SSID match instead of
     * scan-result identity. */
    wifi_info_t current_wifi_info;
    bool wifi_currently_connected = wifi_control_get_info(&current_wifi_info);
    for (int i = 0; i < wifi_saved_result_count; i++) {
        lv_obj_t * row = lv_obj_create(wifi_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        bool is_connected = wifi_currently_connected && strcmp(wifi_saved_results[i].ssid, current_wifi_info.ssid) == 0;
        char text[WIFI_MAX_SSID_LEN + 20];
        snprintf(text, sizeof(text), "%s%s", wifi_saved_results[i].ssid, is_connected ? "  - Connected" : "");

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, text);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_saved_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }

    add_section_header(wifi_list, "Available Networks");
    for (int i = 0; i < wifi_scan_result_count; i++) {
        wifi_network_t * net = &wifi_scan_results[i];

        lv_obj_t * row = lv_obj_create(wifi_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * icon = lv_image_create(row);
        char icon_path[40];
        snprintf(icon_path, sizeof(icon_path), "topbar/wifi_connect_%d.png", net->signal_level);
        lv_image_set_src(icon, asset_path(icon_path));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);

        lv_obj_t * label = lv_label_create(row);
        char text[96];
        snprintf(text, sizeof(text), "%s%s%s", net->ssid, net->secured ? "  (secured)" : "",
                 net->is_current ? "  - Connected" : "");
        lv_label_set_text(label, text);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 64, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
    if (wifi_scan_result_count == 0) {
        lv_obj_t * label = lv_label_create(wifi_list);
        lv_label_set_text(label, "No networks found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }
}

static lv_obj_t * build_wifi_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    lv_obj_t * scr = build_subsonic_list_screen("Wi-Fi", &title_label, &wifi_list);

    lv_obj_t * rescan_btn = lv_label_create(scr);
    lv_label_set_text(rescan_btn, "Rescan");
    lv_obj_set_style_text_color(rescan_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(rescan_btn, ui_size_20, 0);
    lv_obj_align(rescan_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rescan_btn, wifi_rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

static void open_wifi_screen(void) {
    bool enabled = wifi_control_is_enabled();
    populate_wifi_screen(enabled);
    nav_push(wifi_screen);
    if (enabled) start_wifi_scan(); /* auto-refresh -- matches the old always-scan-on-open behavior, but only when there's a radio to scan with */
}

static void wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_screen();
}

/* ---- Bluetooth settings screen ----
 *
 * Same shape as the Wi-Fi screen above: full scan + pair/connect, reachable
 * from the Wireless submenu's Bluetooth tile and from a long-press on the
 * drawer's bt icon. bt_control_scan() blocks for its whole scan window
 * itself (bluetoothctl's own --timeout), so the scan thread has no separate
 * sleep to add, unlike wifi_scan_thread_func above. */

static lv_obj_t * bt_list;

static void start_bt_scan(void);

static pthread_t bt_connect_thread;
static bool bt_connect_active = false;
static volatile bool bt_connect_done_flag = false;
static volatile bool bt_connect_succeeded = false;

/* Which device's row (see add_bt_device_row()) should show inline
 * "Connecting..."/"Failed to connect" status -- an earlier version of this
 * blocked the whole screen with a "Pairing & connecting..." overlay
 * instead, same class of issue as the scan/toggle overlays removed
 * elsewhere in this file (see start_wifi_scan()'s comment): running fully
 * in the background with inline per-row feedback, matching the stock
 * player, means there's no screen to get stuck on at all. */
static char bt_connecting_mac[18] = "";
static char bt_connect_failed_mac[18] = "";

typedef struct {
    char mac[18];
} bt_connect_request_t;

static void * bt_connect_thread_func(void * arg) {
    bt_connect_request_t * req = (bt_connect_request_t *) arg;
    bt_connect_succeeded = bt_control_connect(req->mac);
    free(req);
    bt_connect_done_flag = true; /* written last -- poll_bt_connect only checks this flag */
    return NULL;
}

static void start_bt_connect(const char * mac) {
    if (bt_connect_active) return; /* one at a time -- bt_connect_thread/succeeded are single shared slots */

    bt_connect_request_t * req = malloc(sizeof(*req));
    snprintf(req->mac, sizeof(req->mac), "%s", mac);

    bt_connect_done_flag = false;
    bt_connect_active = true;
    snprintf(bt_connecting_mac, sizeof(bt_connecting_mac), "%s", mac);
    bt_connect_failed_mac[0] = '\0';
    populate_bt_screen(); /* immediate inline "Connecting..." on that row */

    pthread_create(&bt_connect_thread, NULL, bt_connect_thread_func, req);
}

static void poll_bt_connect(void) {
    if (!bt_connect_active || !bt_connect_done_flag) return;

    bt_connect_active = false;
    pthread_join(bt_connect_thread, NULL);
    if (!bt_connect_succeeded) {
        snprintf(bt_connect_failed_mac, sizeof(bt_connect_failed_mac), "%s", bt_connecting_mac);
    }
    bt_connecting_mac[0] = '\0';
    start_bt_scan(); /* refresh so the list reflects the new pair/connect state -- runs in the background too, see its own comment */
}

static pthread_t bt_forget_thread;
static bool bt_forget_active = false;
static volatile bool bt_forget_done_flag = false;

typedef struct {
    char mac[18];
} bt_forget_request_t;

/* Kept alive past the thread completing (freed in poll_bt_forget() instead
 * of the thread func itself) so the completion handler still knows which
 * device to update in bt_scan_results -- see poll_bt_forget()'s own
 * comment for why. */
static bt_forget_request_t * bt_forget_pending_req = NULL;

static void * bt_forget_thread_func(void * arg) {
    bt_forget_request_t * req = (bt_forget_request_t *) arg;
    bt_control_forget(req->mac);
    bt_forget_done_flag = true; /* written last -- poll_bt_forget only checks this flag */
    return NULL;
}

static void start_bt_forget(const char * mac) {
    bt_forget_request_t * req = malloc(sizeof(*req));
    snprintf(req->mac, sizeof(req->mac), "%s", mac);
    bt_forget_pending_req = req;

    bt_forget_done_flag = false;
    bt_forget_active = true;
    pthread_create(&bt_forget_thread, NULL, bt_forget_thread_func, req);
}

/* Real-device feedback: forgetting a device used to refresh the list via
 * start_bt_scan() -- a brand new 6+ second discovery scan with its own busy
 * overlay flashing on screen, surprising ("it shouldn't be triggered") for
 * what's really just a local state change we already know the outcome of.
 * Updating bt_scan_results directly and redrawing is instant and needs no
 * scan at all -- the forgotten device just moves from the Paired section to
 * Available (or disappears next real scan, if it's not actually in range). */
static void poll_bt_forget(void) {
    if (!bt_forget_active || !bt_forget_done_flag) return;
    bt_forget_active = false;
    pthread_join(bt_forget_thread, NULL);

    for (int i = 0; i < bt_scan_result_count; i++) {
        if (strcmp(bt_scan_results[i].mac, bt_forget_pending_req->mac) == 0) {
            bt_scan_results[i].paired = false;
            bt_scan_results[i].connected = false;
            break;
        }
    }
    free(bt_forget_pending_req);
    bt_forget_pending_req = NULL;
    populate_bt_screen();
}

/* ---- Bluetooth device action popup --------------------------------------
 * Tapping a device row used to implicitly Forget (if ->connected) or
 * Connect (otherwise) -- that meant a paired-but-not-currently-connected
 * device had NO path to being forgotten at all, since the only route to
 * start_bt_forget() required ->connected. This replaces that with an
 * explicit popup offering just the actions that make sense for the
 * device's actual state. Same hand-built top-layer overlay shape as
 * error_toast/volume_popup above (this codebase doesn't use LVGL's
 * lv_msgbox anywhere), plus a tap-outside-to-dismiss backdrop. */
static lv_obj_t * bt_action_popup;
static lv_obj_t * bt_action_popup_backdrop;
static lv_obj_t * bt_action_popup_title;
static lv_obj_t * bt_action_connect_row;
static lv_obj_t * bt_action_forget_row;
static int bt_action_popup_device_index = -1;

static void hide_bt_action_popup(void) {
    lv_obj_add_flag(bt_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bt_action_popup, LV_OBJ_FLAG_HIDDEN);
    bt_action_popup_device_index = -1;
}

static void bt_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_action_popup();
}

static void bt_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (bt_action_popup_device_index < 0 || bt_action_popup_device_index >= bt_scan_result_count) {
        hide_bt_action_popup();
        return;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", bt_scan_results[bt_action_popup_device_index].mac);
    hide_bt_action_popup();
    start_bt_connect(mac);
}

static void bt_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (bt_action_popup_device_index < 0 || bt_action_popup_device_index >= bt_scan_result_count) {
        hide_bt_action_popup();
        return;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", bt_scan_results[bt_action_popup_device_index].mac);
    hide_bt_action_popup();
    start_bt_forget(mac);
}

static void build_bt_action_popup(void) {
    lv_obj_t * top = lv_layer_top();

    bt_action_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(bt_action_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(bt_action_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bt_action_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bt_action_popup_backdrop, 0, 0);
    lv_obj_remove_flag(bt_action_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bt_action_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bt_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(bt_action_popup_backdrop, bt_action_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    bt_action_popup = lv_obj_create(top);
    lv_obj_set_size(bt_action_popup, 400, 220);
    lv_obj_align(bt_action_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(bt_action_popup, 16, 0);
    lv_obj_add_style(bt_action_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(bt_action_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bt_action_popup, 0, 0);
    lv_obj_remove_flag(bt_action_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bt_action_popup, LV_OBJ_FLAG_HIDDEN);

    bt_action_popup_title = lv_label_create(bt_action_popup);
    lv_obj_set_width(bt_action_popup_title, lv_pct(90));
    lv_label_set_long_mode(bt_action_popup_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(bt_action_popup_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(bt_action_popup_title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(bt_action_popup_title, ui_size_22, 0);
    lv_obj_align(bt_action_popup_title, LV_ALIGN_TOP_MID, 0, 20);

    bt_action_connect_row = lv_obj_create(bt_action_popup);
    lv_obj_set_size(bt_action_connect_row, lv_pct(90), 56);
    lv_obj_set_style_radius(bt_action_connect_row, 12, 0);
    lv_obj_set_style_bg_opa(bt_action_connect_row, 0, 0);
    lv_obj_set_style_border_width(bt_action_connect_row, 0, 0);
    lv_obj_remove_flag(bt_action_connect_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bt_action_connect_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bt_action_connect_row, bt_action_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * connect_label = lv_label_create(bt_action_connect_row);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_set_style_text_color(connect_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(connect_label, ui_size_20, 0);
    lv_obj_center(connect_label);

    bt_action_forget_row = lv_obj_create(bt_action_popup);
    lv_obj_set_size(bt_action_forget_row, lv_pct(90), 56);
    lv_obj_set_style_radius(bt_action_forget_row, 12, 0);
    lv_obj_set_style_bg_opa(bt_action_forget_row, 0, 0);
    lv_obj_set_style_border_width(bt_action_forget_row, 0, 0);
    lv_obj_remove_flag(bt_action_forget_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bt_action_forget_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bt_action_forget_row, bt_action_forget_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * forget_label = lv_label_create(bt_action_forget_row);
    lv_label_set_text(forget_label, "Forget");
    lv_obj_set_style_text_color(forget_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(forget_label, ui_size_20, 0);
    lv_obj_center(forget_label);
}

static void show_bt_action_popup(int index) {
    bt_action_popup_device_index = index;
    bt_device_t * dev = &bt_scan_results[index];

    lv_label_set_text(bt_action_popup_title, dev->name[0] != '\0' ? dev->name : dev->mac);

    /* Connect makes sense unless already connected. Forget makes sense for
     * anything paired OR connected (the "OR connected" is just a safety net
     * -- a connected-but-somehow-not-paired device shouldn't be able to land
     * on a popup with neither action available). */
    bool show_connect = !dev->connected;
    bool show_forget = dev->paired || dev->connected;
    if (show_connect) lv_obj_remove_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    if (show_forget) lv_obj_remove_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(bt_action_connect_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_align(bt_action_forget_row, LV_ALIGN_TOP_MID, 0, show_connect ? 136 : 70);

    lv_obj_remove_flag(bt_action_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(bt_action_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bt_action_popup_backdrop);
    lv_obj_move_foreground(bt_action_popup);
}

static void bt_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_bt_action_popup(index);
}

static void bt_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_bt_scan();
}

/* Real-device finding (confirmed via `bluetoothctl devices` directly): a
 * device with no advertised name doesn't come back with an empty name at
 * all -- bluetoothctl itself substitutes the device's own MAC address,
 * dashes instead of colons (e.g. mac "5D:C4:96:7E:56:88" -> name
 * "5D-C4-96-7E-56-88"), as a placeholder. bt_scan_results[i].name[0] ==
 * '\0' (an earlier version of the "hide unnamed devices" filter below)
 * essentially never matches real-world unnamed devices because of this --
 * confirmed live as "not reliable, doesn't hide them". Detects that exact
 * placeholder shape instead of assuming an empty string. */
static bool bt_name_is_mac_placeholder(const bt_device_t * dev) {
    if (dev->name[0] == '\0') return true;
    size_t mac_len = strlen(dev->mac);
    if (strlen(dev->name) != mac_len) return false;
    for (size_t i = 0; i < mac_len; i++) {
        char m = dev->mac[i];
        char n = dev->name[i];
        if (m == ':') { if (n != '-' && n != ':') return false; }
        else if (m != n) return false;
    }
    return true;
}

/* Extra row height (beyond LIST_ROW_WIDTH's normal LIST_ROW_HEIGHT) when a
 * row shows the codec subtitle line -- one more small-font line plus a
 * little breathing room, not a full second LIST_ROW_HEIGHT line. */
#define BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT 40

static void add_bt_device_row(lv_obj_t * parent, int index) {
    bt_device_t * dev = &bt_scan_results[index];

    /* Codec is only meaningful for the ONE device actually carrying A2DP
     * audio right now -- dev->connected alone isn't enough (a paired,
     * link-level-connected non-audio BLE peripheral would also read
     * connected=true here), so this also checks the row's own MAC against
     * bt_connected_mac_cached, the accessory bt_control_get_connected_
     * device_mac()/_codec() actually queried. Both come from the same
     * background poll as the rest of this screen's live state (see
     * refresh_bt_icon_thread_func()) -- never a synchronous bluealsa-cli
     * call here on the UI thread. */
    bool show_codec = dev->connected && bt_connected_codec_cached[0] != '\0' &&
                       strcasecmp(dev->mac, bt_connected_mac_cached) == 0;

    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT + (show_codec ? BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT : 0));
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    char text[96];
    const char * status;
    if (strcmp(dev->mac, bt_connecting_mac) == 0) status = "  - Connecting...";
    else if (strcmp(dev->mac, bt_connect_failed_mac) == 0) status = "  - Failed to connect";
    else if (dev->connected) status = "  - Connected";
    else if (dev->paired) status = "  - Paired";
    else status = "";
    snprintf(text, sizeof(text), "%s%s", dev->name[0] != '\0' ? dev->name : dev->mac, status);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    if (show_codec) {
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, LIST_ROW_LABEL_INSET, 14);
    } else {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    }

    if (show_codec) {
        lv_obj_t * codec_label = lv_label_create(row);
        lv_label_set_text(codec_label, bt_connected_codec_cached);
        lv_obj_add_style(codec_label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(codec_label, ui_size_16, 0);
        lv_obj_align(codec_label, LV_ALIGN_BOTTOM_LEFT, LIST_ROW_LABEL_INSET, -12);
    }

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, bt_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
}

static pthread_t bt_scan_thread;
static bool bt_scan_active = false;
static volatile bool bt_scan_done_flag = false;

static void * bt_scan_thread_func(void * arg) {
    (void) arg;
    bt_scan_result_count = bt_control_scan(6, bt_scan_results, BT_MAX_RESULTS);
    bt_scan_done_flag = true; /* written last -- poll_bt_scan only checks this flag */
    return NULL;
}

/* Runs fully in the background, same as the stock player -- no busy
 * screen, no navigation at all. See start_wifi_scan()'s own comment for
 * why (same mechanism, same real incident, just the Bluetooth side of it). */
static void start_bt_scan(void) {
    if (bt_scan_active) return;

    bt_scan_done_flag = false;
    bt_scan_active = true;

    pthread_create(&bt_scan_thread, NULL, bt_scan_thread_func, NULL);
}

static void poll_bt_scan(void) {
    if (!bt_scan_active || !bt_scan_done_flag) return;

    bt_scan_active = false;
    pthread_join(bt_scan_thread, NULL);
    populate_bt_screen(); /* refresh the list either way -- worth keeping current even if the user already backed out */
}

/* ---- Bluetooth DAC screen -- lets this device accept incoming A2DP audio
 * (another device streaming TO it, using it as an external DAC) rather than
 * only ever sending audio out. See bt_control_apply_output_settings()'s own
 * doc comment for exactly what toggling this does at the bluealsa/
 * discoverability level -- it's a real running-service restart, not just a
 * flag flip. ---- */

static lv_obj_t * bt_dac_screen;
static lv_obj_t * bt_dac_list;

static void bt_dac_enable_row_cb(lv_event_t * e);

/* Explanation + a single "Enable" action, not a toggle -- turning Bluetooth
 * DAC on takes over the whole screen with its own overlay (see
 * build_bt_dac_overlay_screen() below), matching USB DAC mode's own design
 * (build_usb_dac_overlay_screen()): the device can't sensibly be used as a
 * normal music player while it's set up to receive and play audio FROM
 * another device, so that isn't something a background toggle should hide.
 * open_bt_dac_screen() only ever reaches this screen when Bluetooth DAC is
 * currently off -- while on, it goes straight to the overlay instead. */
static void populate_bt_dac_screen(void) {
    lv_obj_clean(bt_dac_list);

    lv_obj_t * explanation = lv_label_create(bt_dac_list);
    lv_label_set_text(explanation,
                      "When on, this device stays visible and pairable to other Bluetooth devices, "
                      "so a phone or computer can stream audio TO it and play through this device's "
                      "own output -- using it as an external DAC.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, ui_size_16, 0);
    lv_obj_set_style_pad_left(explanation, 24, 0);
    lv_obj_set_style_pad_top(explanation, 12, 0);
    lv_obj_set_style_pad_bottom(explanation, 12, 0);

    lv_obj_t * row = add_pill_row_base(bt_dac_list, "Enable Bluetooth DAC");
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, bt_dac_enable_row_cb, LV_EVENT_CLICKED, NULL);
}

/* Shared by bt_dac_enable_row_cb() and open_bt_dac_screen()'s "already
 * enabled" shortcut -- ALWAYS re-applies the actual bluealsa/bt-agent
 * sink configuration rather than trusting current_settings.bt_dac_mode_enabled
 * as proof the underlying pipeline is really in that state. Real-device
 * incident: the two could desync -- the flag stayed true (and the overlay
 * kept showing on re-entry) while bluealsa had actually drifted back to
 * its plain a2dp-source baseline (observed after live debugging that
 * involved manually restarting bluetoothd/bluealsa outside the app's own
 * control, but the same desync could happen from any crash or external
 * interference) -- leaving the user staring at a "Bluetooth DAC mode"
 * screen that silently wasn't receiving audio at all, with nothing to
 * indicate why. Re-applying unconditionally on every visit, not just the
 * first enable, is cheap (a few subprocess spawns) and makes the overlay
 * a reliable guarantee of the underlying state instead of a one-time
 * side effect that can go stale. */
static void enable_bt_dac_and_show_overlay(void) {
    current_settings.bt_dac_mode_enabled = true;
    settings_save(&current_settings);
    start_bt_apply_output_settings(true, current_settings.bt_volume_sync_enabled);

    /* This device's incoming-audio consumers (aplay -D bluealsa for
     * Bluetooth DAC, shairport -o ot for AirPlay) and this app's own
     * tinyalsa playback all target the same physical ALSA hardware (hw:0,0
     * / "hibysoundcard") -- real-device testing confirmed fighting over it
     * is a genuine problem, not just a theoretical one. So all three are
     * mutually exclusive: enabling Bluetooth DAC stops local playback (see
     * play_track_at_from()/toggle_play_pause()'s own guards) and turns off
     * AirPlay if it was on. */
    if (audio_is_playing()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    if (current_settings.wifi_dac_mode_enabled) {
        current_settings.wifi_dac_mode_enabled = false;
        settings_save(&current_settings);
        airplay_control_stop();
    }

    nav_push(bt_dac_overlay_screen);
}

static void bt_dac_enable_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    enable_bt_dac_and_show_overlay();
}

static lv_obj_t * build_bt_dac_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Bluetooth DAC", &title_label, &bt_dac_list);
}

static void open_bt_dac_screen(void) {
    /* Bluetooth itself has to be on before any of this makes sense -- only
     * actually reachable via the DAC home tile's "Bluetooth DAC" row
     * (build_dac_home_screen()): the Bluetooth settings screen's own
     * "Bluetooth DAC" chevron row only exists while populate_bt_screen()
     * has already gated its whole extra-rows section on Bluetooth being
     * powered (see its own "if (!powered) return;"), so that path can
     * never reach here with Bluetooth off in the first place -- but the DAC
     * home tile has no such gating, so tapping it with Bluetooth off used
     * to head straight into enable_bt_dac_and_show_overlay(), which just
     * spawns bluealsa/bt-agent against a radio that isn't even on. */
    if (!bt_is_powered_cached) {
        show_error_toast("Enable Bluetooth in settings to use BT DAC mode");
        return;
    }
    /* Already on -- re-apply and go straight to the overlay rather than
     * showing the "explanation + Enable" screen for a mode that's already
     * supposed to be active (see enable_bt_dac_and_show_overlay()'s own
     * comment on why re-applying here, not just trusting the flag,
     * matters). */
    if (current_settings.bt_dac_mode_enabled) {
        enable_bt_dac_and_show_overlay();
        return;
    }
    populate_bt_dac_screen();
    nav_push(bt_dac_screen);
}

static void bt_dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bt_dac_screen();
}

/* ---- Bluetooth DAC overlay + leave-confirmation popup -- same shape as
 * USB DAC mode's own (build_usb_dac_overlay_screen()/
 * build_usb_dac_leave_popup()): full-screen takeover, no swipe-to-back, the
 * only way out is the back button which asks for confirmation first. ---- */
static lv_obj_t * bt_dac_leave_popup;
static lv_obj_t * bt_dac_leave_popup_backdrop;

static void hide_bt_dac_leave_popup(void) {
    lv_obj_add_flag(bt_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bt_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);
}

static void bt_dac_leave_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
}

static void bt_dac_leave_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
}

static void bt_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
    nav_pop(); /* leave the DAC overlay screen */

    current_settings.bt_dac_mode_enabled = false;
    settings_save(&current_settings);
    start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
}

static void bt_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_remove_flag(bt_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(bt_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bt_dac_leave_popup_backdrop);
    lv_obj_move_foreground(bt_dac_leave_popup);
}

static void build_bt_dac_leave_popup(void) {
    lv_obj_t * top = lv_layer_top();

    bt_dac_leave_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(bt_dac_leave_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(bt_dac_leave_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bt_dac_leave_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bt_dac_leave_popup_backdrop, 0, 0);
    lv_obj_remove_flag(bt_dac_leave_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bt_dac_leave_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bt_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(bt_dac_leave_popup_backdrop, bt_dac_leave_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    bt_dac_leave_popup = lv_obj_create(top);
    lv_obj_set_size(bt_dac_leave_popup, 400, 220);
    lv_obj_align(bt_dac_leave_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(bt_dac_leave_popup, 16, 0);
    lv_obj_add_style(bt_dac_leave_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(bt_dac_leave_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bt_dac_leave_popup, 0, 0);
    lv_obj_remove_flag(bt_dac_leave_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bt_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(bt_dac_leave_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "Leave Bluetooth DAC mode?");

    lv_obj_t * leave_row = lv_obj_create(bt_dac_leave_popup);
    lv_obj_set_size(leave_row, lv_pct(90), 56);
    lv_obj_align(leave_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(leave_row, 12, 0);
    lv_obj_set_style_bg_opa(leave_row, 0, 0);
    lv_obj_set_style_border_width(leave_row, 0, 0);
    lv_obj_remove_flag(leave_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(leave_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(leave_row, bt_dac_leave_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * leave_label = lv_label_create(leave_row);
    lv_label_set_text(leave_label, "Leave");
    lv_obj_set_style_text_color(leave_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(leave_label, ui_size_20, 0);
    lv_obj_center(leave_label);

    lv_obj_t * cancel_row = lv_obj_create(bt_dac_leave_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, bt_dac_leave_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, ui_size_20, 0);
    lv_obj_center(cancel_label);
}

static lv_obj_t * build_bt_dac_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, bt_dac_overlay_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("bt/bt.png"));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t * status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Bluetooth DAC mode");
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, ui_size_28, 0);
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

    lv_obj_t * hint_label = lv_label_create(scr);
    lv_label_set_text(hint_label, "This device is now receiving Bluetooth audio");
    lv_obj_add_style(hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(hint_label, ui_size_16, 0);
    lv_obj_align_to(hint_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    return scr;
}

/* ---- Codec selection screen -- single-select list, current choice shown
 * via an accent-colored border rather than a checkmark glyph (same
 * indicator build_accent_color_screen's swatches use; safer than a unicode
 * checkmark that this project's subset LVGL fonts may not actually have a
 * glyph for). Writes straight into /usr/data/alsa.conf via
 * bt_control_set_codec() -- see its own doc comment for the LDAC_HQ/
 * LDAC_SQ caveat. ---- */

typedef struct {
    const char * value; /* matches player_settings_t.bt_codec / bt_control_set_codec()'s argument */
    const char * label;
} bt_codec_option_t;

static const bt_codec_option_t bt_codec_options[] = {
    { "auto", "Auto" },      { "ldac_hq", "LDAC Quality" }, { "ldac_sq", "LDAC Standard" },
    { "aptx", "aptX" },      { "aac", "AAC" },              { "sbc", "SBC" },
};
#define BT_CODEC_OPTION_COUNT (sizeof(bt_codec_options) / sizeof(bt_codec_options[0]))

static lv_obj_t * bt_codec_screen;
static lv_obj_t * bt_codec_list;

static void bt_codec_option_row_cb(lv_event_t * e);

static void populate_bt_codec_screen(void) {
    lv_obj_clean(bt_codec_list);
    for (size_t i = 0; i < BT_CODEC_OPTION_COUNT; i++) {
        bool selected = strcmp(current_settings.bt_codec, bt_codec_options[i].value) == 0;
        lv_obj_t * row = add_pill_row_base(bt_codec_list, bt_codec_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, bt_codec_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void bt_codec_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    snprintf(current_settings.bt_codec, sizeof(current_settings.bt_codec), "%s", bt_codec_options[index].value);
    settings_save(&current_settings);
    bt_control_set_codec(current_settings.bt_codec);
    populate_bt_codec_screen();
}

static lv_obj_t * build_bt_codec_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Codec", &title_label, &bt_codec_list);
}

static void open_bt_codec_screen(void) {
    populate_bt_codec_screen();
    nav_push(bt_codec_screen);
}

static void bt_codec_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bt_codec_screen();
}

/* ---- Resume Last Track selection screen (Settings > Playback > Resume Last
 * Track) -- same accent-colored-border single-select shape as Font Size
 * just below, but takes effect on the NEXT cold boot (there's nothing to
 * apply live -- this only ever matters at startup), so no reboot popup is
 * needed, just a toast confirming the choice. See gui_init()'s own resume
 * path for what each option actually does and the Subsonic-cache guard it
 * shares with Car Mode's separate, always-on resume mechanism. ---- */

typedef struct {
    int mode; /* matches player_settings_t.resume_mode */
    const char * label;
} resume_mode_option_t;

static const resume_mode_option_t resume_mode_options[] = {
    { 0, "Off" }, { 1, "Resume and Play" }, { 2, "Resume, but Paused" },
};
#define RESUME_MODE_OPTION_COUNT (sizeof(resume_mode_options) / sizeof(resume_mode_options[0]))

static void resume_mode_option_row_cb(lv_event_t * e);

static void populate_resume_mode_screen(void) {
    lv_obj_clean(resume_mode_list);
    for (size_t i = 0; i < RESUME_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.resume_mode == resume_mode_options[i].mode;
        lv_obj_t * row = add_pill_row_base(resume_mode_list, resume_mode_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, resume_mode_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void resume_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.resume_mode = resume_mode_options[index].mode;
    settings_save(&current_settings);
    populate_resume_mode_screen();
    show_info_toast("Applies next time you launch the app");
}

static lv_obj_t * build_resume_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Resume Last Track", &title_label, &resume_mode_list);
}

static void open_resume_mode_screen(void) {
    populate_resume_mode_screen();
    nav_push(resume_mode_screen);
}

static void resume_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_resume_mode_screen();
}

/* ---- Font size selection screen (Settings > Font Size) -- same
 * accent-colored-border single-select shape as the Bluetooth codec screen
 * just above, but selecting a row only saves the setting and shows a
 * toast rather than applying anything live -- see gui.c's own
 * apply_font_size_tier() and fallback_font.h's fallback_font_init_early()
 * for why an already-built screen can't be restyled in place, and needs
 * an app restart to pick up the new tier instead. ---- */

typedef struct {
    int tier; /* matches player_settings_t.font_size_tier */
    const char * label;
} font_size_option_t;

static const font_size_option_t font_size_options[] = {
    { 0, "Small" }, { 1, "Medium" }, { 2, "BlindMF" },
};
#define FONT_SIZE_OPTION_COUNT (sizeof(font_size_options) / sizeof(font_size_options[0]))

static lv_obj_t * font_size_screen;
static lv_obj_t * font_size_list;

static void font_size_option_row_cb(lv_event_t * e);

static void populate_font_size_screen(void) {
    lv_obj_clean(font_size_list);
    for (size_t i = 0; i < FONT_SIZE_OPTION_COUNT; i++) {
        bool selected = current_settings.font_size_tier == font_size_options[i].tier;
        lv_obj_t * row = add_pill_row_base(font_size_list, font_size_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, font_size_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

/* Defined later, alongside build_eq_reset_popup()/build_factory_reset_popup()
 * (same hand-built top-layer confirmation-popup shape) -- forward-declared
 * here since font_size_option_row_cb() needs it well before that point in
 * the file. */
static void show_font_size_reboot_popup(void);

static void font_size_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.font_size_tier = font_size_options[index].tier;
    settings_save(&current_settings);
    populate_font_size_screen();
    show_font_size_reboot_popup();
}

static lv_obj_t * build_font_size_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Font Size", &title_label, &font_size_list);
}

static void open_font_size_screen(void) {
    populate_font_size_screen();
    nav_push(font_size_screen);
}

static void font_size_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_font_size_screen();
}

/* ---- USB device mode (Settings > USB Mode) -- single-select list, same
 * shape as the Bluetooth codec screen just above (accent-colored border on
 * the current choice). Applying a mode is real configfs/sysfs work (see
 * usb_mode_control.c), so it runs on its own thread and gets polled from
 * update_timer_cb like every other real-device toggle in this file, rather
 * than blocking the UI thread the way bt_codec_option_row_cb's direct call
 * does. ---- */

typedef struct {
    usb_mode_t mode;
    const char * label;
} usb_mode_option_t;

static const usb_mode_option_t usb_mode_options[] = {
    { USB_MODE_STORAGE, "Storage" },
    { USB_MODE_DAC, "USB DAC" },
    { USB_MODE_ADB, "ADB" },
};
#define USB_MODE_OPTION_COUNT (sizeof(usb_mode_options) / sizeof(usb_mode_options[0]))

static lv_obj_t * usb_mode_screen;
static lv_obj_t * usb_mode_list;

static pthread_t usb_mode_switch_thread;
static bool usb_mode_switch_active = false;
static volatile bool usb_mode_switch_done_flag = false;
static volatile bool usb_mode_switch_succeeded = false;
static usb_mode_t usb_mode_switch_target;

static void usb_mode_option_row_cb(lv_event_t * e);
static void usb_mode_adb_toggle_cb(lv_event_t * e);

/* ADB is a separate toggle, not a third equal option alongside Storage/DAC
 * -- matches the stock firmware's own USB device mode screen (confirmed by
 * the user against a real stock device: ADB overrides Storage/DAC while
 * on, turning it off falls back to the other two). This also matches the
 * actual gadget architecture: ADB lives under its own "adb_demo" configfs
 * directory, entirely separate from the "android0" one Storage and DAC
 * share (see usb_mode_control.c), so it really is orthogonal rather than a
 * third position in the same rotation. */
static void populate_usb_mode_screen(void) {
    lv_obj_clean(usb_mode_list);

    bool adb_active = current_settings.usb_mode == (int) USB_MODE_ADB;
    add_pill_toggle_row(usb_mode_list, "ADB", adb_active, usb_mode_adb_toggle_cb);

    for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
        if (usb_mode_options[i].mode == USB_MODE_ADB) continue; /* handled by the toggle above */

        bool selected = !adb_active && current_settings.usb_mode == (int) usb_mode_options[i].mode;
        lv_obj_t * row = add_pill_row_base(usb_mode_list, usb_mode_options[i].label);
        lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
        lv_obj_set_style_border_color(row, accent_lv_color(), 0);

        if (adb_active) {
            /* Storage/DAC aren't meaningful choices while ADB owns the USB
             * port -- dimmed and inert rather than removed, so the row
             * stays put (no layout jump) and it's visually clear these
             * exist but aren't the current mode. */
            lv_obj_set_style_opa(row, LV_OPA_50, 0);
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, usb_mode_option_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        }
    }
}

static void * usb_mode_switch_thread_func(void * arg) {
    (void) arg;
    usb_mode_switch_succeeded = usb_mode_control_apply(usb_mode_switch_target);
    usb_mode_switch_done_flag = true; /* written last -- poll_usb_mode_switch only checks this flag */
    return NULL;
}

static void start_usb_mode_switch(usb_mode_t target) {
    if (usb_mode_switch_active) return; /* already switching -- ignore taps until it lands, same guard as wifi/bt toggles */
    usb_mode_switch_target = target;
    usb_mode_switch_active = true;
    usb_mode_switch_done_flag = false;
    pthread_create(&usb_mode_switch_thread, NULL, usb_mode_switch_thread_func, NULL);
}

static void poll_usb_mode_switch(void) {
    if (!usb_mode_switch_active || !usb_mode_switch_done_flag) return;
    usb_mode_switch_active = false;
    pthread_join(usb_mode_switch_thread, NULL);

    /* usb_mode_control_apply() only reports success once the target
     * gadget is actually verified bound, not just "the script ran" -- on
     * failure current_settings.usb_mode is deliberately left untouched
     * (the switch genuinely didn't happen) rather than optimistically
     * recording what was merely requested. */
    if (!usb_mode_switch_succeeded) {
        const char * label = "USB mode";
        for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
            if (usb_mode_options[i].mode == usb_mode_switch_target) { label = usb_mode_options[i].label; break; }
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Failed to switch to %s", label);
        show_error_toast(msg);
        populate_usb_mode_screen(); /* in case current_settings.usb_mode's actual row needs re-highlighting */
        return;
    }

    current_settings.usb_mode = (int) usb_mode_switch_target;
    settings_save(&current_settings);
    populate_usb_mode_screen(); /* refresh which row shows the accent border */

    /* DAC mode takes over the whole screen with its own overlay (matching
     * the stock firmware's hiby_usb.view) -- the device can't sensibly be
     * used as a normal music player while it's presenting itself to a PC
     * as a USB sound card. Storage/ADB have no such conflict (the app
     * keeps running normally), so they don't get an overlay. */
    if (usb_mode_switch_target == USB_MODE_DAC) {
        /* Same three-way mutual exclusion as bt_dac_toggle_cb()/
         * airplay_toggle_cb() -- all three (local playback, Bluetooth DAC,
         * AirPlay) fight over the same hw:0,0 output. usb_dac_bridge_start()
         * (called from usb_mode_control_apply(), already run on the
         * background thread by this point) already called audio_stop()
         * itself, but that only stops the backend -- the play button icon
         * needs its own explicit refresh, same as every other call site
         * that stops playback out from under the UI. */
        if (audio_is_playing() || audio_is_paused()) {
            audio_stop();
            set_play_button_state(false);
            plugin_manager_notify_stopped();
        }
        if (current_settings.bt_dac_mode_enabled) {
            current_settings.bt_dac_mode_enabled = false;
            settings_save(&current_settings);
            start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        }
        if (current_settings.wifi_dac_mode_enabled) {
            current_settings.wifi_dac_mode_enabled = false;
            settings_save(&current_settings);
            airplay_control_stop();
        }
        nav_push(usb_dac_overlay_screen);
    }
}

static void usb_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    usb_mode_t target = usb_mode_options[index].mode;
    if (current_settings.usb_mode == (int) target) return;
    start_usb_mode_switch(target);
}

static void usb_mode_adb_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool currently_adb = current_settings.usb_mode == (int) USB_MODE_ADB;
    /* Turning ADB off falls back to Storage -- the stock default, and the
     * safer of the other two since DAC takes over the whole screen with
     * its own overlay rather than just quietly changing what the USB port
     * does. */
    start_usb_mode_switch(currently_adb ? USB_MODE_STORAGE : USB_MODE_ADB);
}

static lv_obj_t * build_usb_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("USB Mode", &title_label, &usb_mode_list);
}

static void open_usb_mode_screen(void) {
    /* current_settings.usb_mode is only a UI hint (see settings.h's own
     * comment: never re-applied to hardware on startup, so it can't be
     * trusted to reflect reality after a reboot or any change made outside
     * this app) -- correct it against live gadget state whenever that
     * state is unambiguous, rather than showing a stale/wrong row
     * highlighted, which is exactly what was happening before (always
     * defaulted to showing Storage selected regardless of what was
     * actually connected). Just sysfs reads, fast enough for the UI
     * thread. */
    usb_mode_t detected;
    if (usb_mode_control_detect_current(&detected) && current_settings.usb_mode != (int) detected) {
        current_settings.usb_mode = (int) detected;
        settings_save(&current_settings);
    }
    populate_usb_mode_screen();
    nav_push(usb_mode_screen);
}

static void usb_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_usb_mode_screen();
}

/* ---- USB DAC mode overlay + leave-confirmation popup --------------------
 * Full-screen takeover matching the stock firmware's hiby_usb.view (a
 * centered USB icon + a top-left back button, nothing else) -- deliberately
 * skips finalize_screen_navigation()'s swipe-to-back/swipe-to-player-screen
 * wiring, same reasoning as build_import_wifi_screen()'s own comment: the
 * only way out is the back button, which asks for confirmation rather than
 * leaving immediately, and an accidental swipe bypassing that would defeat
 * the point. The confirmation popup itself reuses bt_action_popup's own
 * hand-built top-layer overlay shape (this codebase doesn't use LVGL's
 * lv_msgbox anywhere). ---- */
static lv_obj_t * usb_dac_leave_popup;
static lv_obj_t * usb_dac_leave_popup_backdrop;

static void hide_usb_dac_leave_popup(void) {
    lv_obj_add_flag(usb_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usb_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);
}

static void usb_dac_leave_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup(); /* tap-outside = cancel, matches bt_action_popup precedent */
}

static void usb_dac_leave_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup();
}

static void usb_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup();
    nav_pop(); /* leave the DAC overlay screen */
    start_usb_mode_switch(USB_MODE_STORAGE); /* "switch back to Storage (Default)" */
}

static void usb_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_remove_flag(usb_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(usb_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(usb_dac_leave_popup_backdrop);
    lv_obj_move_foreground(usb_dac_leave_popup);
}

static void build_usb_dac_leave_popup(void) {
    lv_obj_t * top = lv_layer_top();

    usb_dac_leave_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(usb_dac_leave_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(usb_dac_leave_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(usb_dac_leave_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(usb_dac_leave_popup_backdrop, 0, 0);
    lv_obj_remove_flag(usb_dac_leave_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usb_dac_leave_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(usb_dac_leave_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(usb_dac_leave_popup_backdrop, usb_dac_leave_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    usb_dac_leave_popup = lv_obj_create(top);
    lv_obj_set_size(usb_dac_leave_popup, 400, 220);
    lv_obj_align(usb_dac_leave_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(usb_dac_leave_popup, 16, 0);
    lv_obj_add_style(usb_dac_leave_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(usb_dac_leave_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(usb_dac_leave_popup, 0, 0);
    lv_obj_remove_flag(usb_dac_leave_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usb_dac_leave_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(usb_dac_leave_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "Leave USB DAC mode?");

    lv_obj_t * leave_row = lv_obj_create(usb_dac_leave_popup);
    lv_obj_set_size(leave_row, lv_pct(90), 56);
    lv_obj_align(leave_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(leave_row, 12, 0);
    lv_obj_set_style_bg_opa(leave_row, 0, 0);
    lv_obj_set_style_border_width(leave_row, 0, 0);
    lv_obj_remove_flag(leave_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(leave_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(leave_row, usb_dac_leave_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * leave_label = lv_label_create(leave_row);
    lv_label_set_text(leave_label, "Leave");
    lv_obj_set_style_text_color(leave_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(leave_label, ui_size_20, 0);
    lv_obj_center(leave_label);

    lv_obj_t * cancel_row = lv_obj_create(usb_dac_leave_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, usb_dac_leave_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, ui_size_20, 0);
    lv_obj_center(cancel_label);
}

static lv_obj_t * build_usb_dac_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, usb_dac_overlay_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("usb/usb.png"));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t * status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "USB DAC mode");
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, ui_size_28, 0);
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

    lv_obj_t * hint_label = lv_label_create(scr);
    lv_label_set_text(hint_label, "This device is now a USB sound card");
    lv_obj_add_style(hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(hint_label, ui_size_16, 0);
    lv_obj_align_to(hint_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    return scr;
}

static void bt_volume_sync_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_volume_sync_enabled = !current_settings.bt_volume_sync_enabled;
    settings_save(&current_settings);
    start_bt_apply_output_settings(current_settings.bt_dac_mode_enabled, current_settings.bt_volume_sync_enabled);
    populate_bt_screen();
}

/* Real-device feedback: "bluetooth screen is showing a lot of devices
 * without a name" -- nearby BLE beacons/accessories that never broadcast
 * one show up as raw MAC addresses (add_bt_device_row()'s own fallback)
 * cluttering the Available Devices list. Purely a display filter, unlike
 * the volume-sync toggle above -- no bluealsa/bluetoothctl state to push,
 * just a settings flag and a re-render. Paired devices are never filtered
 * by this (see populate_bt_screen()'s own loop) -- you wouldn't have
 * paired with one you couldn't identify in the first place. */
static void bt_hide_unnamed_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_hide_unnamed_devices = !current_settings.bt_hide_unnamed_devices;
    settings_save(&current_settings);
    populate_bt_screen();
}

/* ---- Master rebuild for the Bluetooth screen's one dynamic list: toggle
 * row, then (only while powered) volume sync/DAC/Codec rows, Paired
 * Devices, and Available Devices -- same full-rebuild-on-any-change
 * convention as populate_wifi_screen() above. bt_scan_results already holds
 * both paired and freshly-discovered devices from one bt_control_scan()
 * call; this just splits that single list into the two sections by
 * ->paired rather than needing two separate scans. ---- */
static void populate_bt_screen(void) {
    /* Real-device bug: this screen rebuilds from scratch on every ~5s
     * connection poll tick (see poll_refresh_bt_icon()'s own call site),
     * and lv_obj_clean() below resets the list's scroll position to the
     * top along with destroying its children -- confirmed live: with
     * enough devices to need scrolling, the periodic rebuild snapped the
     * view back to the top every cycle, making it impossible to actually
     * read anything past the first screenful. Captured before the clean
     * and restored after rebuilding (both exit paths -- the early return
     * just below when Bluetooth is off has nothing to restore into, but
     * costs nothing to always capture/restore symmetrically). */
    int32_t saved_scroll_y = lv_obj_get_scroll_y(bt_list);

    lv_obj_clean(bt_list);

    /* bt_is_powered_cached, not bt_control_is_powered() -- same reasoning
     * as external_dac_block_reason()'s own use of the cache (see its
     * comment): calling bt_control_is_powered() directly here forked
     * bluetoothctl show synchronously on the UI thread every time this
     * screen opened, which on a real device produced a multi-second freeze
     * entering the Bluetooth submenu (a milder version of the same
     * pre-existing hang class start_refresh_bt_icon()'s own comment
     * documents). The cache is kept fresh in the background regardless. */
    bool powered = bt_is_powered_cached;
    /* quick_drawer_bt_event_cb is the SAME real toggle-thread trigger the
     * drawer's own bt icon uses -- see populate_wifi_screen()'s identical
     * reasoning for wifi. */
    add_pill_toggle_row(bt_list, "Bluetooth", powered, quick_drawer_bt_event_cb);

    if (!powered) {
        lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
        return;
    }

    add_pill_toggle_row(bt_list, "Bluetooth Volume Sync", current_settings.bt_volume_sync_enabled,
                        bt_volume_sync_toggle_cb);
    add_pill_chevron_row(bt_list, "Bluetooth DAC", bt_dac_settings_row_cb);
    add_pill_chevron_row(bt_list, "Codec", bt_codec_settings_row_cb);
    add_pill_toggle_row(bt_list, "Hide Unnamed Devices", current_settings.bt_hide_unnamed_devices,
                        bt_hide_unnamed_toggle_cb);

    add_section_header(bt_list, "Paired Devices");
    int paired_shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (!bt_scan_results[i].paired) continue;
        add_bt_device_row(bt_list, i);
        paired_shown++;
    }
    if (paired_shown == 0) {
        lv_obj_t * label = lv_label_create(bt_list);
        lv_label_set_text(label, "No paired devices");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }

    add_section_header(bt_list, "Available Devices");
    int available_shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (bt_scan_results[i].paired) continue;
        if (current_settings.bt_hide_unnamed_devices && bt_name_is_mac_placeholder(&bt_scan_results[i])) continue;
        add_bt_device_row(bt_list, i);
        available_shown++;
    }
    if (available_shown == 0) {
        lv_obj_t * label = lv_label_create(bt_list);
        lv_label_set_text(label, "No devices found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
    }

    lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
}

static lv_obj_t * build_bluetooth_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    lv_obj_t * scr = build_subsonic_list_screen("Bluetooth", &title_label, &bt_list);

    lv_obj_t * rescan_btn = lv_label_create(scr);
    lv_label_set_text(rescan_btn, "Rescan");
    lv_obj_set_style_text_color(rescan_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(rescan_btn, ui_size_20, 0);
    lv_obj_align(rescan_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rescan_btn, bt_rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

static void open_bluetooth_screen(void) {
    populate_bt_screen();
    nav_push(bt_screen);
    if (bt_is_powered_cached) start_bt_scan(); /* auto-refresh -- matches the old always-scan-on-open behavior, but only when there's a radio to scan with -- cached, not a fresh bt_control_is_powered() call, same reasoning as populate_bt_screen() */
}

static void bt_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bluetooth_screen();
}

/* ---- Import via Wi-Fi screen -- this is just the on-device "here's the
 * address to open" front end; the actual file-manager webapp is the stock
 * firmware's own thttpd + CGI binaries, managed via import_web.h. ---- */

static lv_obj_t * import_wifi_screen;
static lv_obj_t * import_wifi_status_label;
static lv_obj_t * import_wifi_url_label;
#if LV_USE_QRCODE
static lv_obj_t * import_wifi_qrcode;
#endif

static void populate_import_wifi_screen(void) {
    wifi_info_t info;
    if (!wifi_control_get_info(&info) || info.ip[0] == '\0') {
        lv_label_set_text(import_wifi_status_label, "Connect to Wi-Fi first");
        lv_label_set_text(import_wifi_url_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(import_wifi_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        return;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://%s:4399", info.ip);
    lv_label_set_text(import_wifi_status_label, "Open this address on your phone or computer:");
    lv_label_set_text(import_wifi_url_label, url);
#if LV_USE_QRCODE
    lv_obj_remove_flag(import_wifi_qrcode, LV_OBJ_FLAG_HIDDEN);
    lv_qrcode_update(import_wifi_qrcode, url, strlen(url));
#endif
}

/* ---- "Update music database?" confirmation, shown on leaving Import via
 * Wi-Fi -- mirrors build_delete_song_popup()'s own centered-card/two-row
 * shape. Real-device feedback: a full rescan used to run unconditionally on
 * every exit from this screen, even if the user never actually uploaded
 * anything (just looked at the QR code, or backed out immediately) --
 * wasteful on a real library, and gave no way to skip it. Leaving the screen
 * and tearing down the HTTP server (import_web_stop(), now backgrounded --
 * see poll_import_web_stop()'s own comment) both still happen
 * unconditionally regardless of the choice made here; only the rescan
 * itself is behind it. This popup is shown once that teardown finishes,
 * not right when back is tapped. */
static lv_obj_t * import_rescan_popup;
static lv_obj_t * import_rescan_popup_backdrop;

static void hide_import_rescan_popup(void) {
    lv_obj_add_flag(import_rescan_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(import_rescan_popup, LV_OBJ_FLAG_HIDDEN);
}

static void import_rescan_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_import_rescan_popup();
}

static void import_rescan_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_import_rescan_popup();
}

static void import_rescan_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_import_rescan_popup();
    start_library_rescan();
}

static void build_import_rescan_popup(void) {
    lv_obj_t * top = lv_layer_top();

    import_rescan_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(import_rescan_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(import_rescan_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(import_rescan_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(import_rescan_popup_backdrop, 0, 0);
    lv_obj_remove_flag(import_rescan_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(import_rescan_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(import_rescan_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(import_rescan_popup_backdrop, import_rescan_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    import_rescan_popup = lv_obj_create(top);
    lv_obj_set_size(import_rescan_popup, 420, 220);
    lv_obj_align(import_rescan_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(import_rescan_popup, 16, 0);
    lv_obj_add_style(import_rescan_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(import_rescan_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(import_rescan_popup, 0, 0);
    lv_obj_remove_flag(import_rescan_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(import_rescan_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(import_rescan_popup);
    lv_label_set_text(title, "Update music database?");
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * update_row = lv_obj_create(import_rescan_popup);
    lv_obj_set_size(update_row, lv_pct(90), 56);
    lv_obj_align(update_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(update_row, 12, 0);
    lv_obj_set_style_bg_opa(update_row, 0, 0);
    lv_obj_set_style_border_width(update_row, 0, 0);
    lv_obj_remove_flag(update_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(update_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(update_row, import_rescan_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * update_label = lv_label_create(update_row);
    lv_label_set_text(update_label, "Update");
    lv_obj_set_style_text_color(update_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(update_label, ui_size_20, 0);
    lv_obj_center(update_label);

    lv_obj_t * not_now_row = lv_obj_create(import_rescan_popup);
    lv_obj_set_size(not_now_row, lv_pct(90), 56);
    lv_obj_align(not_now_row, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_radius(not_now_row, 12, 0);
    lv_obj_set_style_bg_opa(not_now_row, 0, 0);
    lv_obj_set_style_border_width(not_now_row, 0, 0);
    lv_obj_remove_flag(not_now_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(not_now_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(not_now_row, import_rescan_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * not_now_label = lv_label_create(not_now_row);
    lv_label_set_text(not_now_label, "Not now");
    lv_obj_add_style(not_now_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(not_now_label, ui_size_20, 0);
    lv_obj_center(not_now_label);
}

/* Real-device feedback: leaving Import via Wi-Fi used to just hang -- no
 * visible feedback at all -- for however long import_web_stop() took (its
 * own comment: killall -9 across up to 8 process names, plus a bounded
 * pgrep-polled wait for all of them to actually be reaped, up to ~500ms of
 * genuine subprocess churn) before the screen finally changed, since that
 * call used to run right on this UI-thread click handler. Same background-
 * thread-plus-polled-done-flag shape as every other multi-hundred-ms
 * operation in this file (start_wifi_connect()/poll_wifi_connect() is the
 * closest match: same "push the shared busy screen, no numeric progress to
 * show" shape), reusing subsonic_downloading_screen/_label rather than
 * building new UI just for this one line of text. */
static pthread_t import_web_stop_thread;
static bool import_web_stop_active = false;
static volatile bool import_web_stop_done_flag = false;
/* Import via Wi-Fi screen's own stack slot, recorded right before the busy
 * screen gets pushed on top of it -- spliced out via nav_remove_stack_slot()
 * once teardown finishes (same pattern text_entry_commit()/
 * poll_subsonic_download() already use), so backing out later lands on
 * whatever was open before Import via Wi-Fi, not on this now-defunct
 * "Closing Web Server..." screen or a stale extra copy of the screen
 * underneath it. */
static int import_web_stop_nav_slot = -1;

static void * import_web_stop_thread_func(void * arg) {
    (void) arg;
    import_web_stop();
    import_web_stop_done_flag = true; /* written last -- poll_import_web_stop() only checks this flag */
    return NULL;
}

static void poll_import_web_stop(void) {
    if (!import_web_stop_active || !import_web_stop_done_flag) return;

    import_web_stop_active = false;
    pthread_join(import_web_stop_thread, NULL);

    if (import_web_stop_nav_slot >= 0 && import_web_stop_nav_slot < nav_depth) {
        nav_remove_stack_slot(import_web_stop_nav_slot);
    }
    nav_pop(); /* leave the busy screen */
    import_web_stop_nav_slot = -1;

    lv_obj_remove_flag(import_rescan_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(import_rescan_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(import_rescan_popup_backdrop);
    lv_obj_move_foreground(import_rescan_popup);
}

static void import_wifi_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    import_web_stop_nav_slot = nav_depth - 1; /* this screen's own slot, before pushing the busy screen on top of it */
    lv_label_set_text(subsonic_downloading_label, "Closing\nWeb Server...");
    nav_push(subsonic_downloading_screen);

    import_web_stop_done_flag = false;
    import_web_stop_active = true;
    pthread_create(&import_web_stop_thread, NULL, import_web_stop_thread_func, NULL);
}

static lv_obj_t * build_import_wifi_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, import_wifi_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, "Import via Wi-Fi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_28, 0);

    import_wifi_status_label = lv_label_create(scr);
    lv_obj_set_width(import_wifi_status_label, lv_pct(90));
    lv_label_set_long_mode(import_wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(import_wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(import_wifi_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(import_wifi_status_label, ui_size_20, 0);
    lv_obj_align(import_wifi_status_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 30);

#if LV_USE_QRCODE
    import_wifi_qrcode = lv_qrcode_create(scr);
    lv_qrcode_set_size(import_wifi_qrcode, 220);
    lv_qrcode_set_dark_color(import_wifi_qrcode, lv_color_black());
    lv_qrcode_set_light_color(import_wifi_qrcode, lv_color_white());
    lv_obj_set_style_border_width(import_wifi_qrcode, 4, 0);
    lv_obj_set_style_border_color(import_wifi_qrcode, lv_color_white(), 0);
    lv_obj_align(import_wifi_qrcode, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 110);
#endif

    import_wifi_url_label = lv_label_create(scr);
    lv_obj_set_style_text_color(import_wifi_url_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(import_wifi_url_label, ui_size_22, 0);
    lv_obj_align(import_wifi_url_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 350);

    /* Deliberately skips finalize_screen_navigation()'s swipe-to-back --
     * an accidental swipe here would leave thttpd/udp_server running with
     * nothing to tear them down, same reasoning as text_entry_screen
     * skipping it for its keyboard. The back button above is the only way
     * out, and always runs import_web_stop(). */
    return scr;
}

static void open_import_wifi_screen(void) {
    import_web_start();
    populate_import_wifi_screen();
    nav_push(import_wifi_screen);
}

static void import_wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_import_wifi_screen();
}

/* ---- AirPlay screen -- WiFi analogue of Bluetooth DAC mode, see
 * airplay_control.h for the real mechanism (stock firmware's own shairport
 * binary). Same shape as build_bt_dac_screen(): a toggle plus an
 * explanation, and the same three-way mutual exclusion with local playback
 * and Bluetooth DAC. ---- */

static lv_obj_t * airplay_screen;
static lv_obj_t * airplay_list;

static void airplay_toggle_cb(lv_event_t * e);

static void populate_airplay_screen(void) {
    lv_obj_clean(airplay_list);
    add_pill_toggle_row(airplay_list, "AirPlay", current_settings.wifi_dac_mode_enabled, airplay_toggle_cb);

    lv_obj_t * explanation = lv_label_create(airplay_list);
    lv_label_set_text(explanation,
                      "When on, this device is visible to AirPlay senders on your Wi-Fi network -- stream "
                      "audio from an iPhone, iPad, or Mac to be played through this device's own output.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, ui_size_16, 0);
    lv_obj_set_style_pad_left(explanation, 24, 0);
    lv_obj_set_style_pad_top(explanation, 12, 0);
}

/* /usr/resource/hostname is the same file wifi_on.sh already reads for the
 * DHCP hostname it advertises -- reusing it here keeps the name AirPlay
 * senders see consistent with how this device already identifies itself on
 * the network, rather than inventing a second name. "HiBy_Music" matches
 * that script's own fallback default when the file is missing. */
static void get_device_name(char * out, size_t out_size) {
    snprintf(out, out_size, "HiBy_Music");
    FILE * f = fopen("/usr/resource/hostname", "r");
    if (f) {
        if (fgets(out, (int) out_size, f)) out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

static void airplay_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.wifi_dac_mode_enabled = !current_settings.wifi_dac_mode_enabled;
    settings_save(&current_settings);

    if (current_settings.wifi_dac_mode_enabled) {
        char name[64];
        get_device_name(name, sizeof(name));
        airplay_control_start(name);

        /* Same three-way mutual exclusion as bt_dac_toggle_cb() -- see its
         * own comment for why. */
        if (audio_is_playing()) {
            audio_stop();
            set_play_button_state(false);
            plugin_manager_notify_stopped();
        }
        if (current_settings.bt_dac_mode_enabled) {
            current_settings.bt_dac_mode_enabled = false;
            settings_save(&current_settings);
            start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        }
    } else {
        airplay_control_stop();
    }

    populate_airplay_screen();
}

static lv_obj_t * build_airplay_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("AirPlay", &title_label, &airplay_list);
}

static void open_airplay_screen(void) {
    populate_airplay_screen();
    nav_push(airplay_screen);
}

static void airplay_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_airplay_screen();
}

/* ---- DLNA screen -- see dlna_control.h for the real mechanism (stock
 * firmware's own dmrd binary) and its documented limitations (Pause/Mute/
 * reliable-Volume-feedback/Seek are all confirmed broken inside dmrd
 * itself on real-device testing, independent of anything built here).
 * Same toggle-plus-explanation shape as build_airplay_screen(), but no
 * mutual exclusion with Bluetooth DAC/AirPlay -- see settings.h's own
 * comment on dlna_renderer_enabled for why. ---- */

static lv_obj_t * dlna_screen;
static lv_obj_t * dlna_list;

static void dlna_toggle_cb(lv_event_t * e);

static void populate_dlna_screen(void) {
    lv_obj_clean(dlna_list);
    add_pill_toggle_row(dlna_list, "DLNA Renderer", current_settings.dlna_renderer_enabled, dlna_toggle_cb);

    lv_obj_t * explanation = lv_label_create(dlna_list);
    lv_label_set_text(explanation,
                      "When on, this device is visible to DLNA/UPnP controller apps on your Wi-Fi network -- cast "
                      "a track from one to play it here. Pause, mute, volume, and seek from the controller app "
                      "aren't supported; use this device's own controls instead once a track starts.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, ui_size_16, 0);
    lv_obj_set_style_pad_left(explanation, 24, 0);
    lv_obj_set_style_pad_top(explanation, 12, 0);
}

static void dlna_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.dlna_renderer_enabled = !current_settings.dlna_renderer_enabled;
    settings_save(&current_settings);

    if (current_settings.dlna_renderer_enabled) {
        dlna_control_start();
    } else {
        dlna_control_stop();
    }

    populate_dlna_screen();
}

static lv_obj_t * build_dlna_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("DLNA", &title_label, &dlna_list);
}

static void open_dlna_screen(void) {
    populate_dlna_screen();
    nav_push(dlna_screen);
}

/* ---- Remote Control screen (Wireless -> "Open Link") -- see
 * remote_control.h for the Phase 1 (read-only Now Playing web page, no
 * playback control yet, no auth) scope. Toggle row styled like
 * build_dlna_screen()'s own row; the IP/QR/URL display below it is styled
 * exactly like build_import_wifi_screen()'s (same colors, sizes, and even
 * the same 20px QR-to-URL-label gap) -- this is the same "here's an
 * address, scan or type it" moment for the user, just for a different
 * feature. ---- */

static lv_obj_t * remote_control_screen;
static lv_obj_t * remote_control_toggle_img;
static lv_obj_t * remote_control_status_label;
static lv_obj_t * remote_control_url_label;
#if LV_USE_QRCODE
static lv_obj_t * remote_control_qrcode;
#endif

static void remote_control_refresh_address(void) {
    if (!current_settings.remote_control_enabled) {
        lv_label_set_text(remote_control_status_label,
                           "Turn this on to see the address here.");
        lv_label_set_text(remote_control_url_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        return;
    }

    wifi_info_t info;
    if (!wifi_control_get_info(&info) || info.ip[0] == '\0') {
        lv_label_set_text(remote_control_status_label, "Connect to Wi-Fi first");
        lv_label_set_text(remote_control_url_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        return;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://%s:8899", info.ip);
    lv_label_set_text(remote_control_status_label, "Open this address on your phone or computer:");
    lv_label_set_text(remote_control_url_label, url);
#if LV_USE_QRCODE
    lv_obj_remove_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
    lv_qrcode_update(remote_control_qrcode, url, strlen(url));
#endif
}

static void remote_control_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.remote_control_enabled = !current_settings.remote_control_enabled;
    settings_save(&current_settings);

    lv_image_set_src(remote_control_toggle_img,
                      asset_path(current_settings.remote_control_enabled ? "settings/on.png" : "settings/off.png"));
    /* Keeps the LV_STATE_CHECKED selector on remote_control_toggle_img's own
     * style_accent attachment (see build_remote_control_screen()'s own
     * comment) tracking reality, so the accent recolor only ever shows on
     * the ON sprite -- see add_pill_toggle_row()'s own bug report #2 for
     * why an unconditional recolor is wrong here. */
    if (current_settings.remote_control_enabled) lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(remote_control_toggle_img, LV_STATE_CHECKED);
    if (current_settings.remote_control_enabled) {
        remote_control_start();
    } else {
        remote_control_stop();
    }
    remote_control_refresh_address();
}

static lv_obj_t * build_remote_control_screen(void) {
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
    lv_label_set_text(title, "Remote Control");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_28, 0);

    /* Same 448x124/item_bg.png pill look as add_pill_row_base(), built
     * directly here (not via that helper) since it needs to sit above the
     * absolutely-positioned Import-Wi-Fi-style fields below rather than
     * inside a flex-column list. */
    lv_obj_t * toggle_row = lv_obj_create(scr);
    lv_obj_set_size(toggle_row, 448, 124);
    lv_obj_align(toggle_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 10);
    lv_obj_add_style(toggle_row, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_image_src(toggle_row, asset_path("touch_list/item_bg.png"), 0);
    lv_obj_set_style_bg_opa(toggle_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(toggle_row, 0, 0);
    lv_obj_remove_flag(toggle_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toggle_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(toggle_row, remote_control_toggle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * toggle_label = lv_label_create(toggle_row);
    lv_label_set_text(toggle_label, "Remote Control");
    lv_obj_add_style(toggle_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(toggle_label, ui_size_16, 0);
    lv_obj_align(toggle_label, LV_ALIGN_LEFT_MID, 24, 0);

    remote_control_toggle_img = lv_image_create(toggle_row);
    lv_image_set_src(remote_control_toggle_img,
                      asset_path(current_settings.remote_control_enabled ? "settings/on.png" : "settings/off.png"));
    lv_obj_align(remote_control_toggle_img, LV_ALIGN_RIGHT_MID, -20, 0);
    if (current_settings.remote_control_enabled) lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
    /* Real-device bug report: accent color wasn't applying to this toggle
     * either. Unlike add_pill_toggle_row() (rebuilt fresh on every screen
     * repopulation, so a plain local color read is fine there), this
     * screen is built exactly once at startup (see its own call site) and
     * never rebuilt -- needs the live-updating shared style so a later
     * accent color change still reaches it.
     *
     * Real-device bug report #2: LV_STATE_CHECKED selector, not the
     * unconditional 0 this originally used -- otherwise the recolor hits
     * both the ON and OFF sprite alike, making the toggle unreadable in
     * either state. lv_obj_add_state() above (and remote_control_toggle_
     * cb()'s own add/clear) keeps this selector matching which sprite is
     * actually showing. */
    lv_obj_add_style(remote_control_toggle_img, &style_accent, LV_STATE_CHECKED);

    lv_obj_t * explanation = lv_label_create(scr);
    lv_label_set_text(explanation,
                       "Lets anyone on your Wi-Fi network who opens the address below see what's playing, "
                       "control playback, and browse your library -- no password.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(explanation, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, ui_size_16, 0);
    lv_obj_align(explanation, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 150);

    remote_control_status_label = lv_label_create(scr);
    lv_obj_set_width(remote_control_status_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(remote_control_status_label, ui_size_20, 0);
    lv_obj_align(remote_control_status_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 270);

#if LV_USE_QRCODE
    remote_control_qrcode = lv_qrcode_create(scr);
    lv_qrcode_set_size(remote_control_qrcode, 220);
    lv_qrcode_set_dark_color(remote_control_qrcode, lv_color_black());
    lv_qrcode_set_light_color(remote_control_qrcode, lv_color_white());
    lv_obj_set_style_border_width(remote_control_qrcode, 4, 0);
    lv_obj_set_style_border_color(remote_control_qrcode, lv_color_white(), 0);
    lv_obj_align(remote_control_qrcode, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 340);
#endif

    remote_control_url_label = lv_label_create(scr);
    lv_obj_set_style_text_color(remote_control_url_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(remote_control_url_label, ui_size_22, 0);
    lv_obj_align(remote_control_url_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 592);

    finalize_screen_navigation(scr);
    return scr;
}

static void open_remote_control_screen(void) {
    remote_control_refresh_address();
    nav_push(remote_control_screen);
}

static void remote_control_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_remote_control_screen();
}

static void dlna_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_dlna_screen();
}

static lv_obj_t * build_wireless_screen(void) {
    static icon_grid_item_t items[6];
    items[0] = (icon_grid_item_t){ "wireless/wifi.png", "wireless/wifi_s.png", "Wi-Fi", wifi_tile_cb, NULL };
    items[1] = (icon_grid_item_t){ "wireless/bt.png", "wireless/bt_s.png", "Bluetooth", bt_tile_cb, NULL };
    items[2] = (icon_grid_item_t){ "wireless/airplay.png", "wireless/airplay_s.png", "AirPlay", airplay_tile_cb, NULL };
    items[3] = (icon_grid_item_t){ "wireless/dlna.png", "wireless/dlna_s.png", "DLNA", dlna_tile_cb, NULL };
    /* Relabeled from the stock "HiBy Link" -- this project doesn't implement
     * that proprietary protocol (see the remote-control design doc/plan for
     * why: only partially recoverable from firmware strings, and only useful
     * against HiBy's own closed-source app). This is that plan's own Phase 1
     * (read-only Now Playing web page) now wired up -- see
     * build_remote_control_screen(). */
    items[4] =
        (icon_grid_item_t){ "wireless/hibylink.png", "wireless/hibylink_s.png", "Remote Control", remote_control_tile_cb, NULL };
    items[5] = (icon_grid_item_t){ "wireless/via.png", "wireless/via_s.png", "Import via Wi-Fi", import_wifi_tile_cb, NULL };
    lv_obj_t * scr = build_icon_grid_screen("Wireless", generic_back_cb, items, 6, 120); /* bigger than the shared 100% default -- explicit user request */
    finalize_screen_navigation(scr);
    return scr;
}

/* ---- Basic .txt reader ----
 *
 * "Books" row of the Books menu (originally "Files", folded into this one
 * row and renamed per real-device feedback -- see build_books_screen()):
 * a flat list of every .txt file under MUSIC_ROOT_DIR (there's no separate
 * books partition on this device, same root the music library itself
 * scans), sourced from the persistent book cache (metadata_db.c), tap one
 * to read it. Deliberately basic, matching what was actually asked for --
 * no per-directory navigation (file_browser.c's UI, built for browsing
 * playable audio and building playlists, doesn't fit this shape of
 * problem), no pagination, just load the whole file into one scrollable
 * label. */

static lv_obj_t * books_files_screen;
static lv_obj_t * books_files_list;
static lv_obj_t * books_files_title_label;
/* Which data source populate_books_files_screen() reads from -- set right
 * before nav_push()ing books_files_screen by whichever row (Books or
 * Favorites) opened it, see books_files_row_cb()/books_favorites_row_cb()
 * below. One shared screen/list for both, same as the player screen's
 * transport buttons being reused by the quick drawer -- these are
 * structurally identical (a flat list of book rows, tap to open), just
 * sourced differently. */
static bool books_showing_favorites = false;

static lv_obj_t * text_reader_screen;
static lv_obj_t * text_reader_title_label;
static lv_obj_t * text_reader_scroll;
static lv_obj_t * text_reader_content_label;
static lv_obj_t * text_reader_favorite_icon;
static char * text_reader_current_content = NULL; /* owned; replaced (freed) on every new file opened */
static char text_reader_current_path[600] = ""; /* the currently open book's path -- what the favorite icon below toggles */

static void refresh_text_reader_favorite_icon(void) {
    bool is_favorite = text_reader_current_path[0] != '\0' && metadata_db_book_favorite_is_set(text_reader_current_path);
    lv_image_set_src(text_reader_favorite_icon,
                     asset_path(is_favorite ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
}

static void text_reader_favorite_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (text_reader_current_path[0] == '\0') return;
    bool is_favorite = metadata_db_book_favorite_is_set(text_reader_current_path);
    metadata_db_book_favorite_set(text_reader_current_path, !is_favorite);
    refresh_text_reader_favorite_icon();
}

static void open_text_reader(const char * path) {
    free(text_reader_current_content);
    bool truncated = false;
    text_reader_current_content = text_reader_load(path, &truncated);

    if (!text_reader_current_content) {
        lv_label_set_text(text_reader_content_label, "Could not open this file.");
    } else if (truncated) {
        /* Prepend a plain-text note rather than reaching for a separate
         * toast/label widget -- simplest way to say "there's more" for a
         * feature this basic. */
        char * buf = malloc(strlen(text_reader_current_content) + 128);
        if (buf) {
            snprintf(buf, strlen(text_reader_current_content) + 128,
                     "[File truncated at %d KB -- showing the first part only]\n\n%s",
                     TEXT_READER_MAX_BYTES / 1024, text_reader_current_content);
            free(text_reader_current_content);
            text_reader_current_content = buf;
        }
        lv_label_set_text(text_reader_content_label, text_reader_current_content);
    } else {
        lv_label_set_text(text_reader_content_label, text_reader_current_content);
    }

    lv_label_set_text(text_reader_title_label, basename_of(path));
    snprintf(text_reader_current_path, sizeof(text_reader_current_path), "%s", path);
    refresh_text_reader_favorite_icon();
    lv_obj_scroll_to_y(text_reader_scroll, 0, LV_ANIM_OFF);
    nav_push(text_reader_screen);
}

static void books_file_row_cb(lv_event_t * e) {
    const char * path = (const char *) lv_event_get_user_data(e);
    open_text_reader(path);
}

/* Real-device bug report: opening the Files/Books screen visibly slowed
 * the device down and could look like a crash -- text_reader_scan_txt_files()
 * does a full, uncached recursive readdir()+stat() walk of the WHOLE
 * MUSIC_ROOT_DIR (the entire SD card/storage mount, same root the music
 * library itself scans), synchronously on the UI thread, every single time
 * this screen opens. There's no per-file tag-reading here to cache the way
 * metadata_db.c caches audio metadata (a .txt file's content is never
 * inspected ahead of time, just its name), so the walk itself -- not
 * missing caching -- is the real cost, and on a large/deep library or a
 * slow SD card that's a real multi-second block. Same fix as this
 * codebase's own scan_all_songs_with_timeout() above (a stuck read on a
 * corrupted SD card block lands the thread in an uninterruptible kernel
 * wait -- see that function's own comment for the full mechanism): run it
 * on a throwaway worker thread and stop waiting past a bound rather than
 * risk it. Shorter timeout than the music library's own 30s bound -- this
 * is just a directory walk with no per-file content reading, so it should
 * be far faster, and there's no value in blocking the UI for as long. */
#define BOOKS_SCAN_TIMEOUT_MS 8000

typedef struct {
    char root[600];
    volatile bool done;
    bool ok;
    char ** paths;
    int count;
} books_scan_work_t;

static void * books_scan_worker(void * arg) {
    books_scan_work_t * w = (books_scan_work_t *) arg;
    w->ok = text_reader_scan_txt_files(w->root, &w->paths, &w->count);
    w->done = true; /* written last -- the only field polled below */
    return NULL;
}

static bool books_scan_txt_files_with_timeout(const char * root, char *** out_paths, int * out_count) {
    books_scan_work_t * w = calloc(1, sizeof(*w));
    snprintf(w->root, sizeof(w->root), "%s", root);

    pthread_t thread;
    if (pthread_create(&thread, NULL, books_scan_worker, w) != 0) {
        free(w);
        return false;
    }
    pthread_detach(thread); /* never joined either way -- see scan_all_songs_with_timeout()'s own comment */

    for (int waited_ms = 0; waited_ms < BOOKS_SCAN_TIMEOUT_MS; waited_ms += 20) {
        if (w->done) {
            bool ok = w->ok;
            *out_paths = w->paths;
            *out_count = w->count;
            free(w);
            return ok;
        }
        usleep(20000);
    }

    fprintf(stderr, "Warning: timed out scanning %s for .txt files (possible filesystem corruption) -- treating as empty\n", root);
    return false;
}

/* Real-device feedback: the Books menu's old separate "Scanning" row was a
 * dead stub, and the actual per-visit scan (in populate_books_files_screen(),
 * before this existed) made that screen slow to open every time -- see
 * metadata_db_list_books_from_stock()'s own comment. Both problems share one
 * fix: a real persistent book cache (metadata_db.c, same shape as media's
 * own), refreshed only here -- called from library_scan_once(), i.e. folded
 * into the existing Settings > Update Music Database action, not a separate
 * menu item. Tries the fast stock-db path first (a bounded SQLite read);
 * only falls back to a real live filesystem walk if that finds nothing, so
 * an explicit user-triggered rescan can still discover books the stock
 * scanner never indexed, unlike the boot-time warm-start in metadata_db_
 * open() (fast-path only, deliberately never walks the filesystem, same
 * reasoning as library_load_from_cache_only()'s own comment on why a slow
 * scan can't run before the first frame renders). */
static void rescan_books(void) {
    char ** paths = NULL;
    int count = 0;
    if (!metadata_db_list_books_from_stock(&paths, &count)) {
        books_scan_txt_files_with_timeout(MUSIC_ROOT_DIR, &paths, &count);
    }

    metadata_db_book_replace_all(paths, count);

    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void populate_books_files_screen(void) {
    lv_obj_clean(books_files_list);
    lv_label_set_text(books_files_title_label, books_showing_favorites ? "Favorites" : "Books");

    char ** paths;
    int count;
    /* Real-device bug report: this screen was slow to open every time -- a
     * live recursive readdir()+stat() walk of the whole SD card on every
     * visit (see books_scan_txt_files_with_timeout()'s own comment). Reads
     * from the persistent book cache now instead (metadata_db.c), kept
     * fresh only by rescan_books() (folded into Settings > Update Music
     * Database) -- this screen itself never touches the filesystem or the
     * stock db at all anymore. */
    if (books_showing_favorites) {
        metadata_db_load_favorite_books(&paths, &count);
    } else {
        metadata_db_load_all_books(&paths, &count);
    }

    if (count == 0) {
        lv_obj_t * label = lv_label_create(books_files_list);
        lv_label_set_text(label, books_showing_favorites ? "No favorites yet" : "No .txt files found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        free(paths);
        return;
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t * row = lv_obj_create(books_files_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, basename_of(paths[i]));
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

        /* paths[i] itself becomes the row's user_data -- ownership passes
         * to the row's event callback closure for as long as this screen
         * exists (the array is only ever rebuilt by lv_obj_clean() above,
         * which destroys these rows and their callbacks together, so
         * there's no dangling-pointer window). The char** array holding
         * them is freed here since each element's ownership already moved
         * to its row. */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, books_file_row_cb, LV_EVENT_CLICKED, paths[i]);
    }
    free(paths);
}

static void books_files_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    books_showing_favorites = false;
    populate_books_files_screen();
    nav_push(books_files_screen);
}

static void books_favorites_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    books_showing_favorites = true;
    populate_books_files_screen();
    nav_push(books_files_screen);
}

static lv_obj_t * build_books_files_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen("Books", &books_files_title_label, &books_files_list);
    return scr;
}

static lv_obj_t * build_text_reader_screen(void) {
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

    /* Favorite toggle, top-right -- same collect_in/collect_out asset pair
     * and click-to-toggle shape as the player screen's own favorite_icon,
     * just persisted for real here (metadata_db_book_favorite_set()) rather
     * than that one's purely in-memory, never-saved toggle. */
    text_reader_favorite_icon = lv_image_create(scr);
    lv_image_set_src(text_reader_favorite_icon, asset_path("playing_plane/collect_out.png"));
    lv_obj_align(text_reader_favorite_icon, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(text_reader_favorite_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(text_reader_favorite_icon, 16);
    lv_obj_add_event_cb(text_reader_favorite_icon, text_reader_favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);

    text_reader_title_label = lv_label_create(scr);
    lv_label_set_text(text_reader_title_label, "");
    lv_obj_align(text_reader_title_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(text_reader_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(text_reader_title_label, &app_font_28, 0); /* shows the (possibly non-Latin) file's own name -- see fallback_font.h */
    /* Narrower than build_subsonic_list_screen()'s equivalent titles (70%
     * -- this one has both the back button AND the favorite icon eating
     * into its available width) -- same width-capped dot-scroll treatment
     * as the player screen's own title label either way. */
    lv_obj_set_width(text_reader_title_label, lv_pct(55));
    lv_label_set_long_mode(text_reader_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(text_reader_title_label, LV_TEXT_ALIGN_CENTER, 0);

    text_reader_scroll = lv_obj_create(scr);
    lv_obj_set_size(text_reader_scroll, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(text_reader_scroll, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(text_reader_scroll, 0, 0);
    lv_obj_set_style_border_width(text_reader_scroll, 0, 0);
    lv_obj_set_scroll_dir(text_reader_scroll, LV_DIR_VER);
    lv_obj_set_style_pad_all(text_reader_scroll, 16, 0);

    text_reader_content_label = lv_label_create(text_reader_scroll);
    lv_label_set_long_mode(text_reader_content_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(text_reader_content_label, lv_pct(100));
    lv_obj_add_style(text_reader_content_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(text_reader_content_label, ui_size_20, 0);
    lv_label_set_text(text_reader_content_label, "");

    finalize_screen_navigation(scr);
    /* Same reasoning as every other scrollable-content screen in this
     * file: a drag inside the text itself should scroll the text, not
     * bubble up as an app-wide swipe. */
    lv_obj_remove_flag(text_reader_scroll, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return scr;
}

/* Shared click handler for every plugin-registered Books list row below --
 * user_data is the row's index into plugin_manager's own plugin_books_
 * list_items[] (not an LVGL object), same index-not-object shape
 * plugin_stream_tile_click_cb() already uses for Stream Media tiles. */
static void plugin_books_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_books_list_item_clicked(index);
}

/* "Books" and "Favorites" are this screen's only native rows -- anything
 * else (an "Audio Books" row, or any other plugin-driven entry) comes from
 * plugin.register_list_item("books", ...) (src/plugins/plugin_manager.c),
 * the Audiobooks example plugin (plugins_examples/Audiobooks.lua) being the
 * intended one. This used to be a single hardcoded "Audio Books" row
 * routed to whichever plugin happened to register first (plugin.
 * register_tile(), only one slot, real Home-screen tiles ruled out by the
 * icon grid's own inability to scroll -- see PLUGIN_MAX_STREAM_TILES's own
 * comment in plugin_manager.h for that same constraint elsewhere). Unlike
 * that icon grid, build_pill_list_screen()'s rows genuinely scroll
 * (screen_builders.h's own comment), so there's no reason every plugin
 * that wants a Books row can't have its own -- if none are installed, this
 * screen now just shows its 2 native rows, no dead placeholder toast. */
static lv_obj_t * build_books_screen(void) {
    static pill_list_item_t items[2 + PLUGIN_MAX_BOOKS_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Books", PILL_ACCESSORY_CHEVRON, false, books_files_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Favorites", PILL_ACCESSORY_CHEVRON, false, books_favorites_row_cb, NULL, NULL };

    int count = 2;
    int plugin_count = plugin_manager_get_books_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_BOOKS_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_books_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_books_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_books_list_item_options(i, &item.icon_asset, &item.row_height, &text_size);
        item.text_size = text_size ? text_size : "medium"; /* see pill_row_resolve_text_size()'s own comment on why plugin rows always supply a non-NULL default */
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Books", generic_back_cb, items, count, &style_accent);
    finalize_screen_navigation(scr);
    return scr;
}

/* ---- Firmware update confirmation popup -- same hand-built top-layer
 * overlay shape as eq_reset_popup/bt_dac_leave_popup (this codebase doesn't
 * use LVGL's lv_msgbox anywhere). Confirmation is required since this
 * reboots the device into its own recovery partition -- see
 * firmware_update.h for how that reuses the exact same "*.upt on the SD
 * card + /usr/bin/bootmode.sh Recovery" mechanism the stock closed-source
 * hiby_player's own "Firmware Update" menu item uses, so no custom flashing
 * logic lives here at all. ---- */
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
    lv_obj_t * top = lv_layer_top();

    firmware_update_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(firmware_update_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(firmware_update_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(firmware_update_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(firmware_update_popup_backdrop, 0, 0);
    lv_obj_remove_flag(firmware_update_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(firmware_update_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(firmware_update_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(firmware_update_popup_backdrop, firmware_update_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    firmware_update_popup = lv_obj_create(top);
    lv_obj_set_size(firmware_update_popup, 420, 260);
    lv_obj_align(firmware_update_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(firmware_update_popup, 16, 0);
    lv_obj_add_style(firmware_update_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(firmware_update_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(firmware_update_popup, 0, 0);
    lv_obj_remove_flag(firmware_update_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(firmware_update_popup, LV_OBJ_FLAG_HIDDEN);

    firmware_update_popup_title = lv_label_create(firmware_update_popup);
    lv_obj_set_width(firmware_update_popup_title, lv_pct(90));
    lv_label_set_long_mode(firmware_update_popup_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(firmware_update_popup_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(firmware_update_popup_title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(firmware_update_popup_title, ui_size_22, 0);
    lv_obj_align(firmware_update_popup_title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * update_row = lv_obj_create(firmware_update_popup);
    lv_obj_set_size(update_row, lv_pct(90), 56);
    lv_obj_align(update_row, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_radius(update_row, 12, 0);
    lv_obj_set_style_bg_opa(update_row, 0, 0);
    lv_obj_set_style_border_width(update_row, 0, 0);
    lv_obj_remove_flag(update_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(update_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(update_row, firmware_update_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * update_label = lv_label_create(update_row);
    lv_label_set_text(update_label, "Update & Reboot");
    lv_obj_set_style_text_color(update_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(update_label, ui_size_20, 0);
    lv_obj_center(update_label);

    lv_obj_t * cancel_row = lv_obj_create(firmware_update_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_TOP_MID, 0, 176);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, firmware_update_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, ui_size_20, 0);
    lv_obj_center(cancel_label);
}

static void firmware_update_row_cb(lv_event_t * e) {
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
    snprintf(version_line, sizeof(version_line), "Alpha 2 (%s)", TEST_BUILD_TAG);
#elif defined(BUILD_STAMP)
    snprintf(version_line, sizeof(version_line), "Alpha 2 (%s)", BUILD_STAMP);
#else
    snprintf(version_line, sizeof(version_line), "Alpha 2");
#endif
    items[0] = (pill_list_item_t){ "Open Source Player for HiBy OS", PILL_ACCESSORY_NONE, false, NULL, NULL, NULL };
    items[1] = (pill_list_item_t){ version_line, PILL_ACCESSORY_NONE, false, NULL, NULL, NULL };
    items[2] =
        (pill_list_item_t){ "Firmware Update", PILL_ACCESSORY_CHEVRON, false, firmware_update_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen("About", generic_back_cb, items, 3, &style_accent);
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
    lv_obj_set_style_text_font(title, ui_size_28, 0);

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
        accent_swatches[i] = swatch;
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

/* Index into SCREEN_TIMEOUT_STEPS closest to `seconds` -- settings_load()
 * already snaps stored values onto a step, so this normally finds an exact
 * match; the nearest-match fallback just keeps the slider from landing on a
 * bogus index if it's ever called before that snapping has happened. */
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
 * SCREEN_TIMEOUT_STEP_COUNT-1), not raw seconds, so it can only ever land on
 * one of the fixed presets (30s/1m/2m/5m/10m/30m) rather than an arbitrary
 * linear value. */
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
    lv_obj_set_style_text_font(title, ui_size_28, 0);

    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_size(enable_row, lv_pct(90), 50);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Turn off screen automatically");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, ui_size_20, 0);
    lv_obj_align(enable_label, LV_ALIGN_LEFT_MID, 0, 0);

    screen_timeout_switch = lv_switch_create(enable_row);
    lv_obj_align(screen_timeout_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(screen_timeout_switch, &style_accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
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
    lv_obj_align(screen_timeout_slider_card, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 90);
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
    lv_obj_add_style(screen_timeout_slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(screen_timeout_slider, &style_accent, LV_PART_KNOB);
    lv_obj_add_event_cb(screen_timeout_slider, screen_timeout_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(screen_timeout_slider, 20);

    screen_timeout_value_label = lv_label_create(screen_timeout_slider_card);
    lv_obj_add_style(screen_timeout_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(screen_timeout_value_label, ui_size_28, 0);
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
    lv_obj_set_style_text_font(title, ui_size_28, 0);

    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_size(enable_row, lv_pct(90), 50);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Launch at a fixed volume");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, ui_size_20, 0);
    lv_obj_align(enable_label, LV_ALIGN_LEFT_MID, 0, 0);

    startup_volume_switch = lv_switch_create(enable_row);
    lv_obj_align(startup_volume_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(startup_volume_switch, &style_accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.startup_volume_fixed_enabled) lv_obj_add_state(startup_volume_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(startup_volume_switch, startup_volume_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    startup_volume_slider_card = lv_obj_create(scr);
    lv_obj_set_size(startup_volume_slider_card, lv_pct(90), 170);
    lv_obj_align(startup_volume_slider_card, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 90);
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
    lv_obj_add_style(startup_volume_slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(startup_volume_slider, &style_accent, LV_PART_KNOB);
    lv_obj_add_event_cb(startup_volume_slider, startup_volume_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(startup_volume_slider, 20);

    startup_volume_value_label = lv_label_create(startup_volume_slider_card);
    lv_obj_add_style(startup_volume_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(startup_volume_value_label, ui_size_28, 0);
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
    lv_obj_set_style_text_font(title, ui_size_28, 0);

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
    lv_obj_add_style(sleep_timer_slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(sleep_timer_slider, &style_accent, LV_PART_KNOB);
    lv_obj_add_event_cb(sleep_timer_slider, sleep_timer_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(sleep_timer_slider, 20);

    sleep_timer_value_label = lv_label_create(slider_card);
    lv_obj_add_style(sleep_timer_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(sleep_timer_value_label, ui_size_28, 0);
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
    lv_obj_set_style_text_font(title, ui_size_28, 0);

    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_size(enable_row, lv_pct(90), 50);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Automatically go idle");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, ui_size_20, 0);
    lv_obj_align(enable_label, LV_ALIGN_LEFT_MID, 0, 0);

    idle_shutdown_switch = lv_switch_create(enable_row);
    lv_obj_align(idle_shutdown_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(idle_shutdown_switch, &style_accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
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
    lv_obj_align(idle_action_section, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 90);
    lv_obj_set_style_bg_opa(idle_action_section, 0, 0);
    lv_obj_set_style_border_width(idle_action_section, 0, 0);
    lv_obj_set_style_pad_top(idle_action_section, 0, 0);
    lv_obj_set_style_pad_bottom(idle_action_section, 0, 0);
    lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_SCROLLABLE);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_action_explain_label = lv_label_create(idle_action_section);
    lv_label_set_text(idle_action_explain_label, "Choose what happens when idle:");
    lv_obj_add_style(idle_action_explain_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_action_explain_label, ui_size_16, 0);
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
    lv_obj_align(idle_shutdown_slider_card, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 90 + 292 + 20);
    lv_obj_add_style(idle_shutdown_slider_card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(idle_shutdown_slider_card, 0, 0);
    lv_obj_set_style_radius(idle_shutdown_slider_card, 10, 0);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_shutdown_slider_caption = lv_label_create(idle_shutdown_slider_card);
    lv_label_set_text(idle_shutdown_slider_caption, "Idle timeout:");
    lv_obj_add_style(idle_shutdown_slider_caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_shutdown_slider_caption, ui_size_16, 0);
    lv_obj_align(idle_shutdown_slider_caption, LV_ALIGN_TOP_LEFT, 24, 14);

    idle_shutdown_slider = lv_slider_create(idle_shutdown_slider_card);
    lv_obj_set_width(idle_shutdown_slider, lv_pct(94));
    lv_obj_set_height(idle_shutdown_slider, 36);
    lv_obj_align(idle_shutdown_slider, LV_ALIGN_TOP_MID, 0, 48);
    lv_slider_set_range(idle_shutdown_slider, 0, IDLE_SHUTDOWN_STEP_COUNT - 1);
    lv_slider_set_value(idle_shutdown_slider, idle_shutdown_minutes_to_step_index(current_settings.idle_shutdown_minutes), LV_ANIM_OFF);
    lv_obj_add_style(idle_shutdown_slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(idle_shutdown_slider, &style_accent, LV_PART_KNOB);
    lv_obj_add_event_cb(idle_shutdown_slider, idle_shutdown_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_ext_click_area(idle_shutdown_slider, 20);

    idle_shutdown_value_label = lv_label_create(idle_shutdown_slider_card);
    lv_obj_add_style(idle_shutdown_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(idle_shutdown_value_label, ui_size_28, 0);
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

/* Rebuilt (delete + rebuild, same idiom as all_songs_screen after a library
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
    lv_obj_t * scr = build_compact_list_screen(region, generic_back_cb, items, timezone_city_count, timezone_city_row_click_cb, NULL, NULL, LIST_ROW_WIDTH, false, lv_color_black());
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
    lv_obj_t * scr = build_pill_list_screen("Time Zone", generic_back_cb, items, (int) TIMEZONE_REGION_COUNT, &style_accent);
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
    static pill_list_item_t items[6 + PLUGIN_MAX_PLAYBACK_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Car Mode", PILL_ACCESSORY_TOGGLE,
                                    current_settings.car_mode_enabled, NULL, car_mode_switch_event_cb, NULL };
    items[1] = (pill_list_item_t){ "Crossfade", PILL_ACCESSORY_TOGGLE,
                                    current_settings.crossfade_enabled, NULL, crossfade_switch_event_cb, NULL,
                                    &settings_crossfade_toggle_img };
    items[2] = (pill_list_item_t){ "Equalizer", PILL_ACCESSORY_CHEVRON, false, eq_screen_btn_event_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Resume Last Track", PILL_ACCESSORY_CHEVRON, false, resume_mode_settings_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "Sleep Timer", PILL_ACCESSORY_CHEVRON, false, sleep_timer_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ "Startup Volume", PILL_ACCESSORY_CHEVRON, false, startup_volume_row_cb, NULL, NULL };

    int count = 6;
    int plugin_count = plugin_manager_get_playback_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_PLAYBACK_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_playback_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_playback_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_playback_list_item_options(i, &item.icon_asset, &item.row_height, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Playback", generic_back_cb, items, count, &style_accent);
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
    static pill_list_item_t items[4 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Accent Color", PILL_ACCESSORY_CHEVRON, false, accent_color_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Font Size", PILL_ACCESSORY_CHEVRON, false, font_size_settings_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Screen Timeout", PILL_ACCESSORY_CHEVRON, false, screen_timeout_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Swipe Up for Home", PILL_ACCESSORY_TOGGLE,
                                    current_settings.swipe_up_home_enabled, NULL, swipe_up_home_switch_event_cb, NULL };

    int count = 4;
    int plugin_count = plugin_manager_get_display_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_DISPLAY_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_display_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_display_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_display_list_item_options(i, &item.icon_asset, &item.row_height, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Display", generic_back_cb, items, count, &style_accent);
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
    static pill_list_item_t items[3 + PLUGIN_MAX_POWER_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Charge Limiter (85%)", PILL_ACCESSORY_TOGGLE,
                                    current_settings.charge_limiter_enabled, NULL, charge_limiter_switch_event_cb, NULL };
    items[1] = (pill_list_item_t){ "Idle Shutdown", PILL_ACCESSORY_CHEVRON, false, idle_shutdown_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "LED charge indicator", PILL_ACCESSORY_TOGGLE,
                                    current_settings.led_indicator_enabled, NULL, led_indicator_switch_event_cb, NULL };

    int count = 3;
    int plugin_count = plugin_manager_get_power_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_POWER_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_power_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_power_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_power_list_item_options(i, &item.icon_asset, &item.row_height, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Power", generic_back_cb, items, count, &style_accent);
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

static lv_obj_t * build_settings_system_screen(void) {
    static pill_list_item_t items[4 + PLUGIN_MAX_SYSTEM_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Time Zone", PILL_ACCESSORY_CHEVRON, false, timezone_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "USB Mode", PILL_ACCESSORY_CHEVRON, false, usb_mode_settings_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Update Music Database", PILL_ACCESSORY_NONE, false, update_music_database_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Factory Reset", PILL_ACCESSORY_NONE, false, factory_reset_btn_cb, NULL, NULL };

    int count = 4;
    int plugin_count = plugin_manager_get_system_list_item_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_SYSTEM_LIST_ITEMS; i++) {
        pill_list_item_t item = {
            plugin_manager_get_system_list_item_label(i), PILL_ACCESSORY_CHEVRON, false,
            plugin_system_list_item_click_cb, NULL, (void *) (intptr_t) i
        };
        const char * text_size = NULL;
        plugin_manager_get_system_list_item_options(i, &item.icon_asset, &item.row_height, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("System", generic_back_cb, items, count, &style_accent);
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
        plugin_manager_get_settings_list_item_options(i, &item.icon_asset, &item.row_height, &text_size);
        item.text_size = text_size ? text_size : "medium";
        items[count++] = item;
    }

    lv_obj_t * scr = build_pill_list_screen("Settings", generic_back_cb, items, count, &style_accent);
    finalize_screen_navigation(scr);
    return scr;
}

static void music_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(music_screen);
}

static void stream_media_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(stream_media_screen);
}

static void wireless_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(wireless_screen);
}

static void books_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(books_screen);
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
        nav_push(usb_dac_overlay_screen);
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

static lv_obj_t * build_dac_home_screen(void) {
    static pill_list_item_t items[2];
    items[0] = (pill_list_item_t){ "USB DAC", PILL_ACCESSORY_CHEVRON, false, dac_home_usb_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Bluetooth DAC", PILL_ACCESSORY_CHEVRON, false, bt_dac_settings_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen("DAC", generic_back_cb, items, 2, &style_accent);
    finalize_screen_navigation(scr);
    return scr;
}

static void dac_home_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(dac_home_screen);
}

static lv_obj_t * build_home_screen(void) {
    static icon_grid_item_t items[6];
    items[0] = (icon_grid_item_t){ "launcher/music.png", "launcher/music_s.png", "Music", music_tile_cb, NULL };
    items[1] = (icon_grid_item_t){ "launcher/stream_media.png", "launcher/stream_media_s.png", "Stream Media", stream_media_tile_cb, NULL };
    items[2] = (icon_grid_item_t){ "launcher/wireless.png", "launcher/wireless_s.png", "Wireless", wireless_tile_cb, NULL };
    items[3] = (icon_grid_item_t){ "launcher/book.png", "launcher/book_s.png", "Books", books_tile_cb, NULL };
    items[4] = (icon_grid_item_t){ "launcher/sys_set.png", "launcher/sys_set_s.png", "System", system_tile_cb, NULL };
    items[5] = (icon_grid_item_t){ "launcher/dac.png", "launcher/dac_s.png", "DAC", dac_home_tile_cb, NULL };
    /* No back_btn_cb -- this is the true root, nothing to go back to. No
     * title either -- matches the real stock launcher, which has no header
     * text above its icon grid. */
    lv_obj_t * scr = build_icon_grid_screen(NULL, NULL, items, 6, 100);
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
    lv_obj_set_size(card, lv_pct(92), 132);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Clickable (tap-to-edit, see eq_field_label_click_cb()) -- the whole
     * label area is the tap target, not just the text glyphs, via
     * lv_obj_set_ext_click_area() below. */
    lv_obj_t * value_label = lv_label_create(card);
    lv_obj_add_style(value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value_label, ui_size_22, 0);
    /* y=6, not the original 14 -- real-device feedback: the value text
     * needed more separation from the slider below it, so it moves up a
     * little rather than the slider moving down (slider stays anchored to
     * the card bottom). */
    lv_obj_align(value_label, LV_ALIGN_TOP_LEFT, 14, 6);
    lv_obj_add_flag(value_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(value_label, 16);
    lv_obj_add_event_cb(value_label, eq_field_label_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) field);

    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_width(slider, lv_pct(90));
    lv_obj_set_height(slider, 34);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_slider_set_range(slider, range_min, range_max);
    lv_obj_add_style(slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(slider, &style_accent, LV_PART_KNOB);
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

static lv_obj_t * eq_profiles_screen;
static lv_obj_t * eq_profiles_list;

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
    lv_obj_t * top = lv_layer_top();

    eq_reset_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(eq_reset_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(eq_reset_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(eq_reset_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(eq_reset_popup_backdrop, 0, 0);
    lv_obj_remove_flag(eq_reset_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(eq_reset_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(eq_reset_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(eq_reset_popup_backdrop, eq_reset_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    eq_reset_popup = lv_obj_create(top);
    lv_obj_set_size(eq_reset_popup, 400, 220);
    lv_obj_align(eq_reset_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(eq_reset_popup, 16, 0);
    lv_obj_add_style(eq_reset_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(eq_reset_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eq_reset_popup, 0, 0);
    lv_obj_remove_flag(eq_reset_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(eq_reset_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(eq_reset_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "Reset PEQ to defaults?");

    lv_obj_t * reset_row = lv_obj_create(eq_reset_popup);
    lv_obj_set_size(reset_row, lv_pct(90), 56);
    lv_obj_align(reset_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(reset_row, 12, 0);
    lv_obj_set_style_bg_opa(reset_row, 0, 0);
    lv_obj_set_style_border_width(reset_row, 0, 0);
    lv_obj_remove_flag(reset_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(reset_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(reset_row, eq_reset_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * reset_label = lv_label_create(reset_row);
    lv_label_set_text(reset_label, "Reset");
    lv_obj_set_style_text_color(reset_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(reset_label, ui_size_20, 0);
    lv_obj_center(reset_label);

    lv_obj_t * cancel_row = lv_obj_create(eq_reset_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, eq_reset_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, ui_size_20, 0);
    lv_obj_center(cancel_label);
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
    lv_obj_t * top = lv_layer_top();

    factory_reset_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(factory_reset_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(factory_reset_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(factory_reset_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(factory_reset_popup_backdrop, 0, 0);
    lv_obj_remove_flag(factory_reset_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(factory_reset_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(factory_reset_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(factory_reset_popup_backdrop, factory_reset_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    factory_reset_popup = lv_obj_create(top);
    lv_obj_set_size(factory_reset_popup, 400, 220);
    lv_obj_align(factory_reset_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(factory_reset_popup, 16, 0);
    lv_obj_add_style(factory_reset_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(factory_reset_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(factory_reset_popup, 0, 0);
    lv_obj_remove_flag(factory_reset_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(factory_reset_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(factory_reset_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "Reset all settings and reboot?");

    lv_obj_t * reset_row = lv_obj_create(factory_reset_popup);
    lv_obj_set_size(reset_row, lv_pct(90), 56);
    lv_obj_align(reset_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(reset_row, 12, 0);
    lv_obj_set_style_bg_opa(reset_row, 0, 0);
    lv_obj_set_style_border_width(reset_row, 0, 0);
    lv_obj_remove_flag(reset_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(reset_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(reset_row, factory_reset_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * reset_label = lv_label_create(reset_row);
    lv_label_set_text(reset_label, "Reset");
    lv_obj_set_style_text_color(reset_label, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(reset_label, ui_size_20, 0);
    lv_obj_center(reset_label);

    lv_obj_t * cancel_row = lv_obj_create(factory_reset_popup);
    lv_obj_set_size(cancel_row, lv_pct(90), 56);
    lv_obj_align(cancel_row, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_radius(cancel_row, 12, 0);
    lv_obj_set_style_bg_opa(cancel_row, 0, 0);
    lv_obj_set_style_border_width(cancel_row, 0, 0);
    lv_obj_remove_flag(cancel_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_row, factory_reset_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * cancel_label = lv_label_create(cancel_row);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(cancel_label, ui_size_20, 0);
    lv_obj_center(cancel_label);
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

static void show_font_size_reboot_popup(void) {
    lv_obj_remove_flag(font_size_reboot_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(font_size_reboot_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(font_size_reboot_popup_backdrop);
    lv_obj_move_foreground(font_size_reboot_popup);
}

static void build_font_size_reboot_popup(void) {
    lv_obj_t * top = lv_layer_top();

    font_size_reboot_popup_backdrop = lv_obj_create(top);
    lv_obj_set_size(font_size_reboot_popup_backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(font_size_reboot_popup_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(font_size_reboot_popup_backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(font_size_reboot_popup_backdrop, 0, 0);
    lv_obj_remove_flag(font_size_reboot_popup_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(font_size_reboot_popup_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(font_size_reboot_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(font_size_reboot_popup_backdrop, font_size_reboot_popup_backdrop_cb, LV_EVENT_CLICKED, NULL);

    font_size_reboot_popup = lv_obj_create(top);
    lv_obj_set_size(font_size_reboot_popup, 400, 220);
    lv_obj_align(font_size_reboot_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(font_size_reboot_popup, 16, 0);
    lv_obj_add_style(font_size_reboot_popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(font_size_reboot_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(font_size_reboot_popup, 0, 0);
    lv_obj_remove_flag(font_size_reboot_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(font_size_reboot_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = lv_label_create(font_size_reboot_popup);
    lv_obj_set_width(title, lv_pct(90));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, ui_size_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title, "Restart now to apply the new font size?");

    lv_obj_t * now_row = lv_obj_create(font_size_reboot_popup);
    lv_obj_set_size(now_row, lv_pct(90), 56);
    lv_obj_align(now_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(now_row, 12, 0);
    lv_obj_set_style_bg_opa(now_row, 0, 0);
    lv_obj_set_style_border_width(now_row, 0, 0);
    lv_obj_remove_flag(now_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(now_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(now_row, font_size_reboot_now_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * now_label = lv_label_create(now_row);
    lv_label_set_text(now_label, "Restart Now");
    lv_obj_set_style_text_color(now_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(now_label, ui_size_20, 0);
    lv_obj_center(now_label);

    lv_obj_t * later_row = lv_obj_create(font_size_reboot_popup);
    lv_obj_set_size(later_row, lv_pct(90), 56);
    lv_obj_align(later_row, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_style_radius(later_row, 12, 0);
    lv_obj_set_style_bg_opa(later_row, 0, 0);
    lv_obj_set_style_border_width(later_row, 0, 0);
    lv_obj_remove_flag(later_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(later_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(later_row, font_size_reboot_later_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * later_label = lv_label_create(later_row);
    lv_label_set_text(later_label, "Later");
    lv_obj_add_style(later_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(later_label, ui_size_20, 0);
    lv_obj_center(later_label);
}

static void eq_profile_row_cb(lv_event_t * e) {
    const char * path = (const char *) lv_event_get_user_data(e);
    if (!peq_load_from_path(path)) {
        show_error_toast("Failed to load profile");
        return;
    }
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
    show_error_toast(peq_save_to_path(path) ? "Profile saved" : "Failed to save profile");
}

static void eq_save_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Profile Name", "", false, false, eq_save_profile_name_done_cb, NULL);
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
    lv_obj_set_style_text_font(title, ui_size_28, 0);

    /* "EQ Enabled" switch lives in the title row itself (top-right), same
     * spot other screens put a title-row action (e.g. the Wi-Fi screen's
     * "Rescan"). */
    eq_bypass_switch = lv_switch_create(scr);
    lv_obj_align(eq_bypass_switch, LV_ALIGN_TOP_RIGHT, -16, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_style(eq_bypass_switch, &style_accent, LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* Reset to defaults -- sits right next to the enable switch per the
     * bug report ask, aligned relative to the switch itself (not a fixed
     * x offset) so it stays correctly placed regardless of the switch's
     * actual rendered width. Confirmation required (build_eq_reset_popup())
     * since this clobbers every band's freq/gain/Q/type plus the preamp. */
    lv_obj_t * reset_btn = lv_label_create(scr);
    lv_label_set_text(reset_btn, "Reset");
    lv_obj_set_style_text_color(reset_btn, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(reset_btn, ui_size_16, 0);
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

    /* Band picker: a 5x2 grid of tappable band numbers -- edit one band's
     * freq/Q/gain/type at a time rather than showing 10 bands' worth of
     * controls simultaneously. Bigger buttons than before (80x44 vs
     * 60x28) for the same "bigger, not overlapping" reasoning as the
     * slider cards; LV_SIZE_CONTENT height so it's never a guess at
     * exactly how tall two rows of these need to be. */
    lv_obj_t * band_grid = lv_obj_create(content);
    lv_obj_set_width(band_grid, lv_pct(92));
    lv_obj_set_height(band_grid, LV_SIZE_CONTENT);
    lv_obj_add_style(band_grid, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(band_grid, 0, 0);
    lv_obj_set_style_radius(band_grid, 12, 0);
    lv_obj_set_style_pad_all(band_grid, 10, 0);
    lv_obj_remove_flag(band_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(band_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(band_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < PEQ_NUM_BANDS; i++) {
        lv_obj_t * btn = lv_obj_create(band_grid);
        lv_obj_set_size(btn, 80, 44);
        lv_obj_set_style_bg_opa(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, eq_band_button_event_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);

        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text_fmt(label, "Band%d", i);
        lv_obj_set_style_text_font(label, ui_size_20, 0);
        lv_obj_add_style(label, &style_muted_text, 0);
        lv_obj_center(label);

        eq_band_buttons[i] = btn;
        eq_band_button_labels[i] = label;
    }

    lv_obj_t * eq_freq_card = create_eq_slider_card(content, EQ_FIELD_FREQ, &eq_freq_value_label, &eq_freq_slider, 0, EQ_FREQ_SLIDER_MAX);
    lv_obj_t * eq_gain_card = create_eq_slider_card(content, EQ_FIELD_GAIN, &eq_gain_value_label, &eq_gain_slider, -120, 120);
    lv_obj_t * eq_q_card = create_eq_slider_card(content, EQ_FIELD_Q, &eq_q_value_label, &eq_q_slider, 1, 100);

    lv_obj_t * type_row = lv_obj_create(content);
    lv_obj_set_size(type_row, lv_pct(92), 64);
    lv_obj_add_style(type_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(type_row, 0, 0);
    lv_obj_set_style_radius(type_row, 12, 0);
    lv_obj_remove_flag(type_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * type_label = lv_label_create(type_row);
    lv_label_set_text(type_label, "Type");
    lv_obj_add_style(type_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(type_label, ui_size_20, 0);
    lv_obj_align(type_label, LV_ALIGN_LEFT_MID, 16, 0);

    eq_type_dropdown = lv_dropdown_create(type_row);
    lv_dropdown_set_options(eq_type_dropdown, "Peaking\nLow Shelf\nHigh Shelf");
    lv_obj_set_width(eq_type_dropdown, 170);
    lv_obj_align(eq_type_dropdown, LV_ALIGN_RIGHT_MID, -14, 0);

    lv_obj_t * enable_row = lv_obj_create(content);
    lv_obj_set_size(enable_row, lv_pct(92), 64);
    lv_obj_add_style(enable_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_set_style_radius(enable_row, 12, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * enabled_label = lv_label_create(enable_row);
    lv_label_set_text(enabled_label, "Enable");
    lv_obj_add_style(enabled_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enabled_label, ui_size_20, 0);
    lv_obj_align(enabled_label, LV_ALIGN_LEFT_MID, 16, 0);

    eq_band_enabled_switch = lv_switch_create(enable_row);
    lv_obj_align(eq_band_enabled_switch, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_add_style(eq_band_enabled_switch, &style_accent, LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* Save/Load Profile row -- the new SD card persistence ask, sitting at
     * the end of the same scrollable list as everything else rather than
     * a separate screen of its own. */
    lv_obj_t * profile_row = lv_obj_create(content);
    lv_obj_set_size(profile_row, lv_pct(92), 64);
    lv_obj_set_style_bg_opa(profile_row, 0, 0);
    lv_obj_set_style_border_width(profile_row, 0, 0);
    lv_obj_remove_flag(profile_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(profile_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(profile_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(profile_row, 12, 0);

    lv_obj_t * save_btn = lv_obj_create(profile_row);
    lv_obj_set_size(save_btn, lv_pct(46), 56);
    lv_obj_add_style(save_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_radius(save_btn, 12, 0);
    lv_obj_remove_flag(save_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(save_btn, eq_save_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save Profile");
    lv_obj_set_style_text_font(save_label, ui_size_20, 0);
    lv_obj_add_style(save_label, &style_accent, 0);
    lv_obj_center(save_label);

    lv_obj_t * load_btn = lv_obj_create(profile_row);
    lv_obj_set_size(load_btn, lv_pct(46), 56);
    lv_obj_add_style(load_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(load_btn, 0, 0);
    lv_obj_set_style_radius(load_btn, 12, 0);
    lv_obj_remove_flag(load_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(load_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(load_btn, eq_load_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * load_label = lv_label_create(load_btn);
    lv_label_set_text(load_label, "Load Profile");
    lv_obj_set_style_text_font(load_label, ui_size_20, 0);
    lv_obj_add_style(load_label, &style_accent, 0);
    lv_obj_center(load_label);

    /* Establish correct initial visual state for every widget BEFORE
     * binding any event callbacks -- lv_obj_add_state()/clear_state() can
     * itself fire LV_EVENT_VALUE_CHANGED, which would otherwise trigger a
     * spurious peq_save() during screen construction (observed empirically:
     * band 0 ended up saved as enabled=1 on a fresh run with no user
     * interaction at all). */
    refresh_all_eq_widgets();

    lv_obj_add_event_cb(eq_bypass_switch, eq_bypass_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
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

void gui_init(uint32_t screen_width, uint32_t screen_height) {
#ifndef HOST_BUILD
    boot_checkpoint("gui_init entered");
    bt_boot_suppress_start_tick = lv_tick_get(); /* see bt_boot_suppress_active()'s own comment */
#endif
    settings_load(&current_settings);
#ifndef HOST_BUILD
    boot_checkpoint("settings_load done");
#endif

    /* Must run before any screen below captures a ui_size_16/20/22/28
     * pointer into its own style -- see this function's own doc comment. */
    apply_font_size_tier(current_settings.font_size_tier);

    /* Correct the persisted USB mode against live gadget state before
     * anything else reads it (external_dac_block_reason() in particular --
     * a stale usb_mode==USB_MODE_DAC left over from a previous run would
     * otherwise wrongly block local playback from the moment the app
     * starts). Hardware USB gadget state doesn't survive a reboot, but this
     * binary can also be killed and relaunched without a reboot (as it was
     * repeatedly during development), which is exactly when the persisted
     * value and reality can disagree. */
    usb_mode_t detected_usb_mode;
    if (usb_mode_control_detect_current(&detected_usb_mode)) current_settings.usb_mode = (int) detected_usb_mode;

    /* current_settings.volume itself is left untouched here even when the
     * fixed-startup path below is taken -- it keeps tracking "last used"
     * independently (still updated normally by volume_slider_event_cb's
     * RELEASED case during the session), so turning startup_volume_fixed_enabled
     * back off later resumes from wherever the slider was really last left,
     * not from stale fixed-mode state. */
    audio_set_volume(current_settings.startup_volume_fixed_enabled
                          ? (float) current_settings.startup_volume_fixed_percent / 100.0f
                          : current_settings.volume); /* picked up below when the volume slider reads audio_get_volume() */
    audio_set_crossfade_enabled(current_settings.crossfade_enabled);

    /* Real-device bug report: brightness jumped to an arbitrary value after
     * a real power-down/power-up -- root cause, nothing in this app applied
     * ANY brightness at startup before this, so the screen just came up at
     * whatever raw value the kernel/bootloader itself left the backlight
     * sysfs attribute at. See settings.h's own comment on brightness_percent. */
    backlight_set_percent(current_settings.brightness_percent);

    led_control_apply(current_settings.led_indicator_enabled);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    if (current_settings.timezone[0] != '\0') timezone_apply(current_settings.timezone);

    /* Reapply persisted external-DAC state -- see
     * start_bt_dac_startup_reapply_if_needed()'s own comment for the real
     * incident this fixes. AirPlay's equivalent (wifi_dac_mode_enabled) has
     * the same shape of bug but no slow chip-init step, so it's cheap enough
     * to just call directly here rather than needing its own background
     * thread. */
    if (current_settings.wifi_dac_mode_enabled) {
        char name[64];
        get_device_name(name, sizeof(name));
        airplay_control_start(name);
    }
    start_bt_dac_startup_reapply_if_needed();
    if (current_settings.dlna_renderer_enabled) dlna_control_start();
    if (current_settings.remote_control_enabled) remote_control_start();

    /* The quick drawer's own open/close drag tracking is polled from
     * update_timer_cb instead (poll_quick_drawer_drag()) -- see its comment
     * for why event-based approaches (both indev-wide LV_EVENT_PRESSED/
     * PRESSING and per-object LV_EVENT_GESTURE bubbling) proved unreliable
     * here specifically. */

    lv_style_init(&style_accent);
    lv_style_set_bg_color(&style_accent, accent_lv_color());
    lv_style_set_text_color(&style_accent, accent_lv_color());
    lv_style_set_bg_image_recolor(&style_accent, accent_lv_color());
    lv_style_set_bg_image_recolor_opa(&style_accent, LV_OPA_COVER);
    lv_style_set_image_recolor(&style_accent, accent_lv_color());
    /* LV_OPA_80, not LV_OPA_COVER -- see apply_accent_color()'s own comment
     * on this same property for why COVER flattens on.png's handle and
     * track into one indistinguishable solid color. Mirrored here since
     * this is the initial setup at boot, before apply_accent_color() might
     * ever run again. */
    lv_style_set_image_recolor_opa(&style_accent, LV_OPA_80);

    lv_style_init(&style_muted_text);
    lv_style_set_text_color(&style_muted_text, lv_color_make(220, 220, 220));

    fallback_font_init_early(current_settings.font_size_tier); /* must run before any style/screen captures &app_font_16/&app_font_22 -- see fallback_font.h */
    screen_builders_init_list_row_style();

    /* Discovers plugin rows/tiles by loading and running every .lua file
     * under <SD card>/.plugins/ -- run early, well before Books, Settings,
     * or Stream Media (the current plugin entry points, see build_books_
     * screen()/build_settings_screen()/build_stream_media_screen()) could
     * plausibly be reached. */
    plugin_manager_init();
#ifndef HOST_BUILD
    boot_checkpoint("pre-screen-build setup done");
#endif

    player_screen = build_player_screen(screen_width, screen_height);
#ifndef HOST_BUILD
    boot_checkpoint("build_player_screen done");
#endif
    /* Converts any pre-existing absolute-path playlist entries (everything
     * written before playlist_files_append()/_create() started writing
     * relative ones) to relative -- see playlist_files_migrate_to_relative()'s
     * own comment. Runs before the first read of any playlist below. Host-only
     * exclusion matches migrate_old_db_if_needed()'s own -- ./music/Playlists
     * locally is dev test fixtures, not real user data. */
#ifndef HOST_BUILD
    playlist_files_migrate_to_relative(PLAYLISTS_DIR);
#endif
    library_load_from_cache_only();
#ifndef HOST_BUILD
    boot_checkpoint("library_load_from_cache_only done");
#endif
    files_screen = build_files_screen();
    all_songs_screen = build_all_songs_screen();
    sync_remote_control_library();
    artists_screen = build_artists_screen();
    albums_screen = build_albums_screen();
    album_artist_screen = build_album_artist_screen();
    register_az_index(artists_screen, artists_list, artist_group_name_of, &artist_group_count);
    register_az_index(albums_screen, albums_list, album_group_name_of, &album_group_count);
    register_az_index(album_artist_screen, album_artist_list, album_artist_group_name_of, &album_artist_group_count);
    register_az_index(all_songs_screen, all_songs_list, all_songs_sorted_name_of, &all_songs_count);

    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, artist_group_name_of, &artist_group_count, false);
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, album_group_name_of, &album_group_count, false);
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, album_artist_group_name_of,
                     &album_artist_group_count, false);
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, all_songs_sorted_name_of, &all_songs_count, false);

    /* Files' search-results overlay: a compact-list widget layered on top
     * of file_browser.c's own folder-browsing UI (already built by
     * build_files_screen() above, via file_browser_init()), hidden until
     * search opens -- unlike the other four, files_screen is never rebuilt
     * on a library rescan (file_browser.c manages its own directory
     * listing independently), so this is only ever registered here, not
     * at the post-rescan rebuild site too. */
    files_search_list = build_compact_list_widget(files_screen, NULL, 0, files_search_row_click_cb, NULL, LIST_ROW_WIDTH_WIDE, false, lv_color_black());
    /* build_compact_list_widget() leaves its list transparent by default --
     * fine for the other four screens (it's the only content there), but
     * this one needs to actually COVER file_browser.c's own folder-browsing
     * UI sitting behind it, or it's invisible (indistinguishable from file
     * browsing) whenever there happen to be zero matching rows, i.e. the
     * moment search opens before anything's been typed. */
    lv_obj_set_style_bg_opa(files_search_list, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(files_search_list, lv_color_black(), 0);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_HIDDEN);
    /* Unlike the other four screens' lists (built before their own
     * finalize_screen_navigation() call, so already covered by its
     * one-time recursive pass), this is built well after files_screen's
     * own -- needs the same explicit gesture-bubble treatment. */
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    enable_gesture_bubble_recursive(files_search_list);
    register_search(SEARCH_BINDING_FILES, files_screen, files_search_list, all_songs_sorted_name_of, &all_songs_count, true);

    playlists_screen = build_playlists_screen();
    group_songs_screen = build_group_songs_screen();
    artist_albums_screen = build_subsonic_list_screen("Albums", &artist_albums_title_label, &artist_albums_list);
    text_entry_screen = build_text_entry_screen();
    music_screen = build_music_screen();
    stream_media_screen = build_stream_media_screen();
    subsonic_saved_servers_screen = build_subsonic_saved_servers_screen();
    subsonic_new_connection_screen = build_subsonic_new_connection_screen();
    subsonic_entry_screen = build_subsonic_entry_screen();
    subsonic_downloading_screen = build_subsonic_downloading_screen();

    /* Subsonic screen redesign: the menu (Artists/Playlists/Albums) that's
     * now the first screen after connecting -- see poll_subsonic_connect().
     * Rows built once here, not repopulated per visit, since this list
     * never changes. */
    subsonic_menu_screen = build_subsonic_list_screen("Subsonic", &subsonic_menu_title_label, &subsonic_menu_list);
    {
        lv_obj_t * artists_row = add_pill_row_base(subsonic_menu_list, "Artists");
        lv_obj_add_flag(artists_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(artists_row, subsonic_menu_artists_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * playlists_row = add_pill_row_base(subsonic_menu_list, "Playlists");
        lv_obj_add_flag(playlists_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(playlists_row, subsonic_menu_playlists_row_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t * albums_row = add_pill_row_base(subsonic_menu_list, "Albums");
        lv_obj_add_flag(albums_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(albums_row, subsonic_menu_albums_row_cb, LV_EVENT_CLICKED, NULL);
    }

    subsonic_artists_screen = build_subsonic_list_screen("Artists", &subsonic_artists_title_label, &subsonic_artists_list);
    subsonic_albums_screen = build_subsonic_list_screen("Albums", &subsonic_albums_title_label, &subsonic_albums_list);
    subsonic_songs_screen = build_subsonic_list_screen("Songs", &subsonic_songs_title_label, &subsonic_songs_list);
    subsonic_playlists_screen = build_subsonic_list_screen("Playlists", &subsonic_playlists_title_label, &subsonic_playlists_list);

    /* Top-of-screen Download buttons -- see subsonic_download_artist_btn_cb()/
     * subsonic_download_songs_btn_cb() for what each actually downloads.
     * Added directly onto the already-built screens (same pattern as
     * build_wifi_screen()'s Rescan button) rather than threading a new
     * parameter through build_subsonic_list_screen() itself, which ~20
     * other, unrelated screens also share. Hidden by default -- shown only
     * when the screen's own click handler determines it's actually
     * applicable (an Artist page for the albums one; always for the songs
     * one, whether it's an album or a playlist). */
    subsonic_albums_download_btn = lv_label_create(subsonic_albums_screen);
    lv_label_set_text(subsonic_albums_download_btn, "Download");
    lv_obj_set_style_text_color(subsonic_albums_download_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(subsonic_albums_download_btn, ui_size_20, 0);
    /* -20-51-16 (not just -20, the plain top-right corner offset the songs
     * button below still uses) -- subsonic_albums_screen also gets a search
     * icon (registered below), which sits at the plain -20 corner; shifted
     * left here to clear it rather than overlapping when both are visible
     * (an Artist page, mid-search). */
    lv_obj_align(subsonic_albums_download_btn, LV_ALIGN_TOP_RIGHT, -87, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(subsonic_albums_download_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(subsonic_albums_download_btn, subsonic_download_artist_btn_cb, LV_EVENT_CLICKED, NULL);
    /* subsonic_albums_download_btn (see its own comment above) is already
     * the leftmost of the two right-side buttons this screen can show, so
     * reserving up to it also clears the search icon further right. */
    reserve_title_width_before(subsonic_albums_title_label, subsonic_albums_download_btn);

    subsonic_songs_download_btn = lv_label_create(subsonic_songs_screen);
    lv_label_set_text(subsonic_songs_download_btn, "Download");
    lv_obj_set_style_text_color(subsonic_songs_download_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(subsonic_songs_download_btn, ui_size_20, 0);
    lv_obj_align(subsonic_songs_download_btn, LV_ALIGN_TOP_RIGHT, -20, STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - 28) / 2);
    lv_obj_add_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(subsonic_songs_download_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(subsonic_songs_download_btn, subsonic_download_songs_btn_cb, LV_EVENT_CLICKED, NULL);
    reserve_title_width_before(subsonic_songs_title_label, subsonic_songs_download_btn);

    /* Same live search as Artists/Albums/Album Artist/All Songs (see the
     * search_binding_t infra above), extended to Subsonic's own Artists and
     * Albums lists -- these are non-virtualized build_subsonic_list_screen()
     * flex lists (populate_indexed_list()'s one-object-per-row shape), not
     * the virtualized compact_list_widget the other bindings assume, hence
     * flex_click_cb: search_apply_filter()/search_close() detect it and
     * repopulate via populate_indexed_list_from_items() instead of
     * compact_list_set_items(). No A-Z index -- same reasoning as Files
     * (search-only), and unlike Files this isn't even alphabetically sorted
     * (server order). Registered once here only -- unlike the local-library
     * bindings, these screens are never rebuilt (no equivalent of a library
     * rescan), so there's no second registration site to mirror this at. */
    register_search(SEARCH_BINDING_SUBSONIC_ARTISTS, subsonic_artists_screen, subsonic_artists_list, subsonic_artist_label_of,
                     &subsonic_artists_count, false);
    search_bindings[SEARCH_BINDING_SUBSONIC_ARTISTS].flex_click_cb = subsonic_artist_row_click_cb;
    register_search(SEARCH_BINDING_SUBSONIC_ALBUMS, subsonic_albums_screen, subsonic_albums_list, subsonic_album_label_of,
                     &subsonic_albums_count, false);
    search_bindings[SEARCH_BINDING_SUBSONIC_ALBUMS].flex_click_cb = subsonic_album_row_click_cb;
    wifi_screen = build_wifi_screen();
    wifi_info_screen = build_wifi_info_screen();
    wifi_dns_screen = build_wifi_dns_screen();
    bt_screen = build_bluetooth_screen();
    bt_dac_screen = build_bt_dac_screen();
    bt_dac_overlay_screen = build_bt_dac_overlay_screen();
    bt_codec_screen = build_bt_codec_screen();
    font_size_screen = build_font_size_screen();
    resume_mode_screen = build_resume_mode_screen();
    usb_mode_screen = build_usb_mode_screen();
    usb_dac_overlay_screen = build_usb_dac_overlay_screen();
    build_import_rescan_popup(); /* must exist before import_wifi_back_cb (wired below) can show it */
    import_wifi_screen = build_import_wifi_screen();
    airplay_screen = build_airplay_screen();
    dlna_screen = build_dlna_screen();
    remote_control_screen = build_remote_control_screen();
    wireless_screen = build_wireless_screen();
    books_screen = build_books_screen();
    books_files_screen = build_books_files_screen();
    text_reader_screen = build_text_reader_screen();
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
    build_eq_reset_popup();
    build_factory_reset_popup();
    build_font_size_reboot_popup();
    build_sd_mount_failed_popup();
    build_sd_format_confirm_popup();
    build_power_off_countdown_popup();
    build_firmware_update_popup();
    add_to_playlist_screen = build_add_to_playlist_screen();
    queue_screen = build_queue_screen();
    for (int i = 0; i < PLUGIN_LIST_SCREEN_POOL_SIZE; i++) {
        plugin_list_screens[i] = build_subsonic_list_screen("Plugin", &plugin_list_title_labels[i], &plugin_list_lists[i]);
    }
    for (int i = 0; i < PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE; i++) {
        plugin_settings_list_screens[i] =
            build_subsonic_list_screen("Settings", &plugin_settings_list_title_labels[i], &plugin_settings_list_lists[i]);
    }
    build_delete_song_popup();
    build_more_menu_popup();
    build_song_context_menu_popup();
    dac_home_screen = build_dac_home_screen();
    home_screen = build_home_screen();
#ifndef HOST_BUILD
    boot_checkpoint("all screens built");
#endif

    /* See static_snapshot_screen's comment -- these are pure fixed content
     * (icon grid / pill list, no toggles or per-item state) and never
     * change after this point, so their transition bitmap is worth
     * rendering once now rather than on every single visit. settings_screen
     * (the new category menu) and settings_system_screen now qualify too --
     * unlike the old flat System list, neither has any toggle rows. */
    register_static_snapshot(0, home_screen);
    register_static_snapshot(1, music_screen);
    register_static_snapshot(2, stream_media_screen);
    register_static_snapshot(3, wireless_screen);
    register_static_snapshot(4, books_screen);
    register_static_snapshot(5, about_screen);
    register_static_snapshot(6, settings_screen);
    register_static_snapshot(7, settings_system_screen);
    register_static_snapshot(8, dac_home_screen);

    build_status_bar();
    build_volume_popup();
    build_home_indicator_bar();
    build_error_toast();
    build_info_toast();
    build_bt_action_popup();
    build_wifi_action_popup();
    build_usb_dac_leave_popup();
    build_bt_dac_leave_popup();
    build_quick_drawer();
    refresh_clock_label();
    refresh_battery_topbar();
    refresh_wifi_icon();
    refresh_play_pause_topbar();
#ifndef HOST_BUILD
    boot_checkpoint("start_refresh_bt_icon about to be called");
#endif
    start_refresh_bt_icon();
#ifndef HOST_BUILD
    boot_checkpoint("start_refresh_bt_icon done");
#endif
    refresh_headphone_icon();
    poll_usb_audio_output(); /* same reasoning as refresh_headphone_icon() above -- detect an already-connected USB DAC immediately rather than waiting for the first timer tick */
    /* Read once at startup, not per-refresh -- this is a static device
     * config file, not something that changes while the app is running
     * (the stock player itself only writes it from its own Settings
     * screen, which isn't reachable from here). Must happen before this
     * first refresh_volume_topbar() call so the initial color is correct. */
    if (!device_config_get_volume_warn_threshold(&volume_warn_threshold_percent)) {
        volume_warn_threshold_percent = -1;
    }
    refresh_volume_topbar((int32_t) (audio_get_volume() * 100.0f));
    lv_timer_create(update_timer_cb, 500, NULL);
    /* Its own fast timer -- see poll_quick_drawer_drag()'s comment for why
     * update_timer_cb's 500ms period is too slow to track a swipe with. */
    lv_timer_create(poll_quick_drawer_drag, LV_DEF_REFR_PERIOD, NULL);
    lv_timer_create(poll_az_index_drag, LV_DEF_REFR_PERIOD, NULL);

#ifndef HOST_BUILD
    /* Holds the boot splash (gui_show_boot_splash(), called from main.c
     * before any of this function's own work) on screen for at least
     * BOOT_SPLASH_MIN_DISPLAY_MS of real boot time, piggybacking on
     * whatever this function's own setup (library load, screen building)
     * already consumed -- only adds *additional* wait if that finished
     * faster, so this isn't always a flat tax on boot time. This also
     * delays start_refresh_bt_icon() (called above) by the same margin.
     *
     * Deliberately NOT waiting here for bt_init to finish -- see
     * BOOT_SPLASH_MIN_DISPLAY_MS's own comment, further up, for what was
     * tried and reverted, and why Bluetooth is left alone (off, and
     * staying off) rather than this app touching it at all during boot. */
    while (boot_splash_start_tick != 0 &&
           lv_tick_get() - boot_splash_start_tick < BOOT_SPLASH_MIN_DISPLAY_MS) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms > 50) wait_ms = 50;
        usleep(wait_ms * 1000);
    }
    boot_checkpoint("boot splash settle wait done");

    /* Reveals the status bar/popups/quick drawer -- see gui_show_boot_
     * splash()'s own comment for why these were hidden in the first place
     * (they all live on lv_layer_top(), which paints above any screen
     * unconditionally, splash included). */
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
#endif

    /* home_screen is the permanent root of the nav stack -- nav_pop() never
     * goes past it. Load it first so there's always something valid on
     * screen even before any auto-resume logic below runs. */
    nav_stack[0] = home_screen;
    nav_depth = 1;
    lv_screen_load(home_screen);
#ifndef HOST_BUILD
    boot_checkpoint("lv_screen_load(home_screen) done");
#endif

    /* Real-device incident (2026-08-08): auto-resuming into a Subsonic
     * track (cached locally at SUBSONIC_STREAM_CACHE_DIR/stream.<suffix>,
     * see that macro's own comment further down -- last_track points at
     * that cache file, not a live stream URL) crashed and rebooted the
     * device on a cold boot with no Wi-Fi connected yet. Root cause was
     * never fully pinned down -- candidates included the cache file being
     * a leftover partial/corrupt download from a session killed mid-stream,
     * or something else on this path unexpectedly touching network state
     * -- so this whole path was disabled outright rather than resuming
     * into a file of unknown safety on every single cold boot.
     *
     * Car Mode's rework back to poweroff+auto-resume (see its own comment
     * above, in the update_timer_cb Car Mode block) needs this path
     * working again, so it's re-enabled here with the guard that was
     * actually missing before: last_track is skipped if it's inside
     * SUBSONIC_STREAM_CACHE_DIR rather than a real library file. This can't
     * rule out every possible cause of the original crash, but it removes
     * the one concretely different thing about a Subsonic-cache resume
     * versus a normal library-file resume -- this same path already worked
     * fine for ordinary local files before that incident -- so it's the
     * correct first fix to try rather than leaving auto-resume disabled
     * for everyone over one still-unexplained edge case.
     *
     * This block was, for a long time, gated on car_mode_enabled only, NOT
     * the separate general "resume on every launch" setting -- that one had
     * no UI toggle at all since this incident (its Settings row was removed
     * entirely, not just left inert -- a live-looking toggle for a feature
     * that silently did nothing read as a regression) and defaulted to true,
     * so wiring it back in here unconditionally would have silently
     * auto-resumed on EVERY cold boot for every user, not just those who
     * deliberately opted into Car Mode's specific docking routine.
     *
     * Now reintroduced as Settings -> Playback -> Resume Last Track
     * (player_settings_t.resume_mode), opt-in and defaulting to off (0),
     * handled in the separate `else if` below so it can share this same
     * Subsonic-cache guard without duplicating it, while staying fully
     * independent of Car Mode's own always-on, headphone-gated resume. */
    if (current_settings.car_mode_enabled && current_settings.last_track[0] != '\0' &&
        strncmp(current_settings.last_track, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) != 0) {
#ifndef HOST_BUILD
        /* Real-device incident: a device Car Mode shut down (unplugged
         * while playing) that was later found with its wired headphones
         * ALSO disconnected, then manually powered on with no external
         * power connected at all, boot-looped on every attempt -- only
         * recoverable by pulling the SD card (so this whole block's
         * file_browser_build_playlist_for_path() call below fails to find
         * last_track and the resume is skipped entirely) and then
         * disabling Car Mode by hand. This is the same general class of
         * problem as the 2026-08-08 Subsonic-cache incident above (an
         * auto-resume at boot crashing/hanging for a reason never fully
         * pinned down) -- rather than resume blind into a state already
         * twice confirmed dangerous, refuse outright whenever there's no
         * headphone jack connected (this app's own docking routine's own
         * assumption: Car Mode expects to resume into a car's own wired
         * aux/dock connection, not open air) and disable Car Mode so a
         * user who hits this doesn't land back in the exact same trap on
         * their very next boot too, before they've had any chance to
         * investigate. headphone_is_connected() is a cheap synchronous
         * sysfs read (no D-Bus/Bluetooth involved) -- safe to call this
         * early in boot, unlike a Bluetooth connectivity check (see the
         * "no startup-time Bluetooth cleanup" comment in main.c). */
        if (!headphone_is_connected()) {
            current_settings.car_mode_enabled = false;
            settings_save(&current_settings);
            show_info_toast("Car Mode disabled: no headphone detected at boot");
        } else
#endif
        {
            char ** resume_playlist;
            int resume_count, resume_index;
            if (file_browser_build_playlist_for_path(current_settings.last_track, &resume_playlist, &resume_count, &resume_index)) {
                free_playlist();
                playlist = resume_playlist;
                playlist_count = resume_count;
                /* play_track_at_from() itself nav_push()es player_screen on top
                 * of the seeded root, so a back-swipe from the resumed player
                 * correctly lands back on the home screen. */
                play_track_at_from(resume_index, current_settings.last_position);
            }
        }
    } else if (current_settings.resume_mode != 0 && current_settings.last_track[0] != '\0' &&
               strncmp(current_settings.last_track, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) != 0) {
        /* General-purpose "Resume Last Track" (Settings -> Playback),
         * independent of Car Mode's own dedicated docking-routine resume
         * above -- reuses the exact same Subsonic-cache guard (the one
         * concretely identified cause of the 2026-08-08 crash-reboot-loop
         * incident) but deliberately has none of Car Mode's own
         * headphone-presence requirement, which is specific to its docking
         * assumption and doesn't apply to a normal user just picking this
         * setting up in Settings. */
        char ** resume_playlist;
        int resume_count, resume_index;
        if (file_browser_build_playlist_for_path(current_settings.last_track, &resume_playlist, &resume_count, &resume_index)) {
            free_playlist();
            playlist = resume_playlist;
            playlist_count = resume_count;
            play_track_at_from(resume_index, current_settings.last_position);
            if (current_settings.resume_mode == 2) {
                /* "Resume, but Paused" -- load/seek exactly as resume_mode 1
                 * does, then immediately pause, same as a user tapping Play
                 * then Pause right after. No audible auto-play at boot; the
                 * track is ready the instant the user hits play themselves. */
                toggle_play_pause();
            }
        }
    }
#ifndef HOST_BUILD
    boot_checkpoint("gui_init returning");
#endif

    /* Deliberately the very last thing gui_init() does -- see
     * fallback_font_schedule_deferred_load()'s own doc comment for why this
     * can't just load the font directly here or anywhere earlier in this
     * function (the auto-resume path just above is exactly the kind of
     * pre-first-frame call site that hung boot on the previous attempt at
     * this). */
    fallback_font_schedule_deferred_load();
}
