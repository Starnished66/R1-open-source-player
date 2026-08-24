#include "gui.h"
#include "gui_library.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_settings.h"
#include "gui_network.h"
#include "gui_books.h"
#include "gui_text_input.h"
#include "gui_lyrics.h"
#include "gui_subsonic.h"
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
#include "albumart.h"
#include "lyrics.h"
#include "lyrics_layout.h"
#include "remote_track.h"
#include "transition_compositor.h"
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
#include "cue_parser.h"
#include "subprocess.h"
#include "timezone_data.h"
#include "timezone_apply.h"
#include "hostname_apply.h"

/* --- Theme API (gui_theme.h) --- */



/* --- Notification/Modal API (gui_notifications.h) --- */


gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
void gui_busy_set_progress(gui_busy_handle_t handle, int percent);
void gui_busy_hide(gui_busy_handle_t handle);

gui_busy_handle_t wifi_connect_token = 0;
gui_busy_handle_t wifi_connect_saved_token = 0;
gui_busy_handle_t import_web_stop_token = 0;

#include "src/core/lv_obj.h"
#include "src/core/lv_obj_pos.h"
#include "src/core/lv_obj_style.h"
#include "src/core/lv_obj_style_gen.h"
#include "src/layouts/flex/lv_flex.h"
#include "src/misc/lv_area.h"
#include "src/draw/lv_draw_buf.h"
#include "src/others/snapshot/lv_snapshot.h"
#include "src/core/lv_refr.h"
#include "src/widgets/image/lv_image.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef UI_PERF_TRACE
static uint64_t ui_perf_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}
#endif

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

/* Books are deliberately isolated from the music-library root. A recursive
 * .txt search across an entire large SD card is both unexpectedly broad and
 * slow; only this dedicated directory participates in book discovery. */
#define BOOKS_ROOT_DIR MUSIC_ROOT_DIR "/Books"
#define AUDIOBOOKS_LIBRARY_DIR_NAME "Audiobooks"

/* Music browsing benefits from roomier touch targets and artwork, while
 * Settings and the rest of the app retain the denser shared 84px rows. */

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
/* ui_size_* and apply_font_size_tier moved to gui_theme.c */

static lv_obj_t * home_screen;
lv_obj_t * stream_media_screen;

static lv_obj_t * player_screen;
static lv_obj_t * player_dismiss_btn;
void sync_player_topbar_visibility(lv_obj_t * screen);
lv_obj_t * dac_home_screen;







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
uint8_t * current_cover_bytes;
static lv_image_dsc_t current_cover_dsc;
int current_cover_for_index = -1;
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
/* Settings -> System -> "24-Hour Clock" (current_settings.clock_24h). Extra
 * flex-row child after the 5 digit slots, hidden entirely in 24h mode --
 * topbar/am.png and topbar/pm.png are pre-existing theme assets, unused by
 * any code before this setting. clock_topbar_group is LV_SIZE_CONTENT and
 * center-aligned to the status bar band, so hiding/showing this slot
 * reflows and re-centers the whole clock automatically, same as any other
 * flex child visibility change in this file. */
static lv_obj_t * clock_topbar_ampm;
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
static lv_obj_t * quick_drawer_motion_image;
static lv_draw_buf_t * quick_drawer_motion_buf;
static bool quick_drawer_bitmap_motion;
static bool quick_drawer_snapshot_dirty = true;
static void quick_drawer_mark_snapshot_dirty(void);

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
    quick_drawer_mark_snapshot_dirty();
}

static char ** playlist = NULL;
static int playlist_count = 0;
int playlist_index = -1;

/* Non-NULL only while playing from the whole, unfiltered All Songs list OR
 * the whole Recently Added list -- see on_file_selected_lazy_all_songs()'s/
 * on_file_selected_lazy_recently_added()'s own comments for why. playlist[i]
 * == NULL means "not resolved yet"; playlist_path_at() (forward-declared
 * below) resolves playlist_lazy_sort_order[i] to a real path via a single-
 * row DB query (the DB's own title-sorted order by default, or first_seen-
 * DESC order when playlist_lazy_order_is_recency is set, offset by that
 * value), strdup()s it into playlist[i], and from then on that slot is a
 * completely ordinary owned entry. Kept the same length as
 * playlist[] itself by queue_add_song()/queue_remove_song_at_offset()/
 * delete_song_confirm_cb() -- the only three places that resize or shift
 * playlist[] after creation -- so every still-unresolved slot keeps
 * pointing at the right song after a splice. Real-device cost this exists
 * to avoid: tapping any song in a 32K-song library used to strdup() all
 * 32,000 paths just to play one of them, the same O(library)-per-action
 * failure class as this whole session's boot-scale incidents. */
static int * playlist_lazy_sort_order = NULL;
/* Which DB order playlist_lazy_sort_order[]'s identity mapping refers to --
 * see playlist_path_at()'s own comment. Only meaningful while playlist_
 * lazy_sort_order is non-NULL. */
static bool playlist_lazy_order_is_recency = false;

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

/* Path of whichever song is currently playing, or an empty string if
 * nothing is (or the current track isn't part of the local library, e.g.
 * an Airplay/DLNA source). Set once per real track-start in apply_track_
 * metadata_to_ui(), from that function's own local `path`. This is the
 * single source of truth every now-playing indicator (Artists/Albums/All
 * Songs/group-songs rows) reads from -- each resolves it to a display
 * position via its own DB query (metadata_db_get_group_offset()/
 * metadata_db_get_song_title_offset()/a direct path comparison), rather
 * than an in-memory array index -- see refresh_now_playing_indicators()
 * below. */
char now_playing_path[600] = "";

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
/* player_source_kind_t defined in gui.h */

player_source_kind_t player_source_kind = PLAYER_SOURCE_NONE;

int player_source_all_songs_index = -1; /* row index into all_songs_list -- the DB's own title-sorted order */
int player_source_recently_added_index = -1; /* row index into recently_added_list -- the DB's own first_seen-DESC order */

/* Own copy of the group's song entries at the moment playback started --
 * group_songs_entries/count/title_label themselves just describe
 * whichever group group_songs_screen CURRENTLY shows, which can change
 * (browsing to a different artist/album, or a library rescan) before the
 * user ever opens "List". group_song_entry_t (gui.c further down) is
 * declared after this point in the file -- forward-declared here since
 * this struct only needs a pointer to it, not its layout. */
/* group_song_entry_t defined in gui_library.h */
char * player_source_group_title = NULL;
group_song_entry_t * player_source_group_entries = NULL;
int player_source_group_count = 0;
int player_source_group_pos = -1; /* row index within the group */

char player_source_file_browser_dir[PATH_MAX];
int player_source_file_browser_row = -1;
/* Set while the shared Group Songs screen represents an album.  The screen
 * is reused for artists, favorites and playlists too, so its title alone
 * cannot identify the source type for persistence. */

/* Resume-but-paused is a true deferred start.  Starting audio and then
 * immediately pausing races the output open on a headphone-less boot; an
 * ALSA failure can consume the queue before the pause lands. */
static bool deferred_resume_pending = false;
static double deferred_resume_position = 0.0;

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

/* Set only when compute_auto_advance_index() has to precompute a reshuffled
 * continuation bag for the shuffle-wrap case -- see both that function's
 * and commit_auto_advance()'s own comments. Declared here (rather than
 * just above compute_auto_advance_index(), where it used to live) since
 * ensure_shuffle_order_current() -- defined earlier in this file -- also
 * needs to invalidate it on a playlist change. */
static int * pending_shuffle_order = NULL;

static bool user_seeking = false;
/* A short debounce keeps a seek followed immediately by next/previous from
 * entering a slow synchronous decoder seek for a track that is about to be
 * discarded.  It is imperceptible compared with the 500 ms UI refresh and
 * is cancelled by play_track_at_from(). */
static lv_timer_t * pending_progress_seek_timer;
static double pending_progress_seek_seconds;

static void cancel_pending_progress_seek(void) {
    if (pending_progress_seek_timer) {
        lv_timer_delete(pending_progress_seek_timer);
        pending_progress_seek_timer = NULL;
    }
}
static int32_t displayed_progress_percent = -1;
static int displayed_position_second = -1;
static int displayed_duration_second = -1;

player_settings_t current_settings;

/* Shared style object driving the app-wide accent color (sliders, checked
 * switches, the selected EQ band) -- one lv_style_t whose bg_color gets
 * updated in place whenever the user picks a new color, so every widget
 * that has this style attached re-renders automatically without having to
 * walk and restyle each one individually. */
/* style_accent defined in gui_theme.c */
/* Plain light-gray text style for the *unselected* EQ band labels -- kept
 * as its own shared style (rather than a one-off lv_obj_set_style_text_color)
 * so toggling selection is just swapping which shared style is attached,
 * with no risk of a local per-object style override taking priority over
 * style_accent and silently defeating the highlight. */
/* accent colors and styles moved to gui_theme.c */

/* Generic back-stack, replacing the old pairwise hardcoded back targets
 * (settings always -> browser, eq always -> settings). Every screen's back
 * button and the left-to-right swipe gesture just call nav_pop(); forward
 * navigation calls nav_push(). Root (home_screen) is seeded once in
 * gui_init() and is never popped past. */
#define NAV_STACK_MAX 16
lv_obj_t * nav_stack[NAV_STACK_MAX];
int nav_depth = 0;

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

/* Forward declarations -- real (first) tentative definitions further down
 * this file, alongside the code that actually builds/positions each of
 * these. A plain `static T * x;` with no initializer is a tentative
 * definition in C; declaring the same one twice in one translation unit is
 * fine and merges into a single variable, so this is just an ordering fix,
 * not a second/shadow copy -- build_flattened_transition_frame() below
 * needs all three, and is used by register_static_snapshot() just below
 * it, which is itself defined earlier in the file than any of the real
 * declarations. */
static lv_obj_t * status_bar_band;
static lv_obj_t * home_indicator_band;

/* Alpha-blends one row of an ARGB8888 source over an RGB565 destination
 * row, `count` pixels wide -- shared by build_flattened_transition_frame()
 * below for compositing a persistent lv_layer_top() band (status bar,
 * home indicator) onto a screen-only RGB565 base. LVGL's ARGB8888 memory
 * layout is B,G,R,A per pixel (see lv_color32_t in lv_color.h), not the
 * A,R,G,B its name suggests. */
static void blend_argb8888_row_over_rgb565(uint16_t * dst, const uint8_t * src_argb, int32_t count) {
    for (int32_t x = 0; x < count; x++) {
        uint8_t b = src_argb[x * 4 + 0];
        uint8_t g = src_argb[x * 4 + 1];
        uint8_t r = src_argb[x * 4 + 2];
        uint8_t a = src_argb[x * 4 + 3];
        if (a == 0) continue; /* fully transparent -- leave the destination pixel untouched */
        if (a == 255) {
            dst[x] = (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            continue;
        }
        uint16_t old = dst[x];
        uint8_t old_r = (uint8_t) (((old >> 11) & 0x1F) << 3);
        uint8_t old_g = (uint8_t) (((old >> 5) & 0x3F) << 2);
        uint8_t old_b = (uint8_t) ((old & 0x1F) << 3);
        uint8_t nr = (uint8_t) (((uint32_t) r * a + (uint32_t) old_r * (255 - a)) / 255);
        uint8_t ng = (uint8_t) (((uint32_t) g * a + (uint32_t) old_g * (255 - a)) / 255);
        uint8_t nb = (uint8_t) (((uint32_t) b * a + (uint32_t) old_b * (255 - a)) / 255);
        dst[x] = (uint16_t) (((nr >> 3) << 11) | ((ng >> 2) << 5) | (nb >> 3));
    }
}

/* Snapshots `overlay_obj` (an lv_layer_top() child -- status_bar_band or
 * home_indicator_band, never a whole layer, see build_flattened_transition_
 * frame()'s own comment on why) as ARGB8888 and alpha-blends it onto
 * `base` at overlay_obj's own real on-screen coordinates. No-op (leaves
 * base untouched) if the snapshot itself fails -- caller still gets a
 * usable, if incomplete, frame rather than none at all. */
static void blend_overlay_onto_base(lv_draw_buf_t * base, lv_obj_t * overlay_obj) {
    lv_area_t coords;
    lv_obj_get_coords(overlay_obj, &coords);
    int32_t ow = lv_area_get_width(&coords);
    int32_t oh = lv_area_get_height(&coords);
    if (ow <= 0 || oh <= 0) return;

    lv_draw_buf_t * overlay = lv_snapshot_take(overlay_obj, LV_COLOR_FORMAT_ARGB8888);
    if (!overlay) return;

    int32_t base_w = (int32_t) base->header.w;
    int32_t base_h = (int32_t) base->header.h;
    for (int32_t y = 0; y < oh; y++) {
        int32_t dy = coords.y1 + y;
        if (dy < 0 || dy >= base_h) continue;
        int32_t dx0 = coords.x1;
        int32_t count = ow;
        int32_t sx0 = 0;
        if (dx0 < 0) { sx0 = -dx0; count += dx0; dx0 = 0; }
        if (dx0 + count > base_w) count = base_w - dx0;
        if (count <= 0) continue;
        const uint8_t * srow = (const uint8_t *) lv_draw_buf_goto_xy(overlay, (uint32_t) sx0, (uint32_t) y);
        uint16_t * drow = (uint16_t *) lv_draw_buf_goto_xy(base, (uint32_t) dx0, (uint32_t) dy);
        blend_argb8888_row_over_rgb565(drow, srow, count);
    }
    lv_draw_buf_destroy(overlay);
}

/* Screen-only RGB565 base for target_screen -- player_dismiss_btn's hidden
 * flag (Player's own standalone back arrow, part of player_screen's own
 * subtree, not a separate layer) is temporarily forced to its TARGET-state
 * value (based on current_settings.hide_player_topbar, never on whichever
 * live screen happens to be active right now) before snapshotting, then
 * restored immediately after -- safe because nothing here ever yields to a
 * real lv_timer_handler()/lv_refr_now() refresh pass in between, so the
 * live UI never actually renders the temporary state to the real screen.
 * Deliberately does NOT include the persistent status bar / home-indicator
 * content -- see blend_persistent_bars() below for why that's kept
 * separate rather than baked in here. Caller owns the returned buffer;
 * returns NULL on snapshot failure. */
static lv_draw_buf_t * snapshot_screen_base(lv_obj_t * target_screen) {
    if (!target_screen) return NULL;

    bool dismiss_target_hidden = current_settings.hide_player_topbar && target_screen == player_screen;
    bool dismiss_touched = (player_dismiss_btn != NULL && target_screen == player_screen);
    bool dismiss_was_hidden = false;
    if (dismiss_touched) {
        dismiss_was_hidden = lv_obj_has_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        if (dismiss_target_hidden) lv_obj_add_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_draw_buf_t * base = lv_snapshot_take(target_screen, LV_COLOR_FORMAT_RGB565);

    if (dismiss_touched) {
        if (dismiss_was_hidden) lv_obj_add_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
    }
    return base;
}

/* Blends whichever persistent lv_layer_top() bands would actually be
 * visible on target_screen (status_bar_band per Settings > Display > "Hide
 * Player/Lyrics Top Bar", home_indicator_band per Settings > Display >
 * "Swipe Up for Home") onto an existing owned RGB565 base, mutating it in
 * place. TRANSITION_PERFORMANCE_PLAN.md Phase 3 target architecture,
 * section B -- kept as a separate, cheap, always-fresh step from the base
 * snapshot itself (snapshot_screen_base() above) specifically so a CACHED
 * base (the static-screen cache or the Phase 2 player cache, both below)
 * never bakes in stale clock/battery/wifi/home-indicator content: real-
 * device review finding -- baking bars into a snapshot taken once (the
 * static cache, built at startup) or infrequently (the player cache, only
 * rebuilt on track/art/play-state changes, not every second) meant a
 * transition could show a visibly wrong/outdated clock or a home-indicator
 * bar whose visibility had since been toggled off. Blending fresh at
 * transition time instead means the persistent-bar content is never more
 * than a few lines of code away from what a real, current LVGL render of
 * lv_layer_top() would show. Deliberately does NOT snapshot lv_layer_top()
 * as a whole: that would also capture transient overlays (the volume
 * popup, quick-drawer motion image, a DAC overlay) that may happen to be
 * present at the wrong moment -- only these two named, permanent bands are
 * ever composited in. status_bar_band's hidden flag is temporarily forced
 * to its target-state value (same reasoning/safety as snapshot_screen_
 * base()'s own comment on player_dismiss_btn); home_indicator_band's
 * current live flag already IS its correct target-state value (its
 * visibility is unrelated to which screen is active), so it needs no such
 * forcing. */
static void blend_persistent_bars(lv_draw_buf_t * base, lv_obj_t * target_screen) {
    bool topbar_target_hidden = current_settings.hide_player_topbar &&
                                 (target_screen == player_screen || target_screen == gui_lyrics_get_screen());
    if (status_bar_band) {
        bool status_was_hidden = lv_obj_has_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);
        if (!topbar_target_hidden) {
            if (status_was_hidden) lv_obj_remove_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);
            blend_overlay_onto_base(base, status_bar_band);
        }
        if (status_was_hidden) lv_obj_add_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);
    }
    if (home_indicator_band && !lv_obj_has_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN)) {
        blend_overlay_onto_base(base, home_indicator_band);
    }
}

/* Builds a complete, opaque, full-screen RGB565 frame representing exactly
 * what the display should look like once target_screen is the active
 * screen -- a fresh screen-only base (snapshot_screen_base()) with the
 * persistent bars blended fresh on top (blend_persistent_bars()). Used as
 * begin_slide_transition()'s synchronous fallback when neither the static-
 * screen cache nor the Phase 2 player cache has a base available -- the
 * cached-base cases dup their own cached base and call
 * blend_persistent_bars() directly instead of going through this, so the
 * bars are still always fresh even when the (comparatively expensive)
 * screen content itself comes from a cache. Caller owns the returned
 * buffer (lv_draw_buf_destroy() it when done); returns NULL on allocation/
 * snapshot failure. */
static lv_draw_buf_t * build_flattened_transition_frame(lv_obj_t * target_screen) {
    lv_draw_buf_t * base = snapshot_screen_base(target_screen);
    if (!base) return NULL;
    blend_persistent_bars(base, target_screen);
    return base;
}

/* A deliberately narrow set of screens are pure static content -- fixed
 * icon-grid/pill-list items baked in at build time, no toggles, no
 * per-item state, nothing a timer ever touches. Their screen-only content
 * is rendered ONCE at startup and reused forever, since re-rendering it
 * before every single visit was the last remaining cost in an otherwise-
 * cheap transition -- the persistent status bar/home-indicator content is
 * deliberately NOT part of this cached base (see blend_persistent_bars()'s
 * own comment on why baking bars into a cache built once at startup goes
 * stale); begin_slide_transition() blends them in fresh every time it uses
 * this cache. Anything with actual dynamic content (player screen, file/
 * song lists, Settings' toggles, the accent-color picker's selection ring,
 * Subsonic status screens) is deliberately left out and always rendered
 * fresh. */
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
    static_snapshot_buf[index] = snapshot_screen_base(scr);
}

/* Player-screen transition-frame cache -- TRANSITION_PERFORMANCE_PLAN.md
 * Phase 2. player_screen is dynamic (track metadata/art/play-state, so it
 * can't just be baked in once like the STATIC_SNAPSHOT_SCREEN_COUNT screens
 * above), but it's also the single most common transition target (every
 * player-swipe and most Home/Now-Playing navigations) and the one whose
 * on-demand lv_snapshot_take() cost is actually visible: confirmed via
 * on-device UI_PERF_TRACE profiling, that snapshot alone typically costs
 * 10-14ms and spiked to ~84ms on a cold cache the first time it ran. This
 * mirrors quick_drawer_mark_snapshot_dirty()/quick_drawer_rebuild_snapshot()
 * above -- the exact same "own an independent bitmap, invalidate lazily,
 * rebuild once via lv_async_call() during the next idle pass instead of on
 * the touch-down path" shape, just for a full-screen target instead of the
 * drawer panel. Rebuilt off the gesture path entirely: at track start, once
 * cover art actually finishes decoding, on play/pause icon changes, on
 * accent-color changes, and when the hide-topbar setting changes -- NOT on
 * every per-second progress-bar tick (deliberately -- see the plan's own
 * Phase 2 requirement 2), so the cached frame's progress fill can be up to
 * a few seconds stale. Always safe to use regardless of staleness: this is
 * an independent owned copy, never aliased to any live widget's own memory,
 * so nothing about "how stale" it is can make it unsafe to display, only
 * slightly out of date -- see begin_slide_transition()'s own use of it.
 * Screen-only base -- deliberately does NOT include the persistent status
 * bar/home-indicator content, for the same staleness reason
 * blend_persistent_bars() explains: this cache is only rebuilt on the
 * dirty triggers above, not every second, so baking in the clock/battery/
 * wifi here would go visibly stale between rebuilds. begin_slide_
 * transition() blends those bars in fresh from this cached base every
 * time it's used. */
static lv_draw_buf_t * player_transition_cache_buf = NULL;
static bool player_transition_cache_dirty = true;

static void player_transition_rebuild_cache(void) {
    /* Nothing to gain while player_screen is already the live screen --
     * no transition can ever target the screen already on display, and
     * re-snapshotting it here would just be wasted work on every one of
     * its own dynamic updates (track change, progress tick via the other
     * dirty triggers, etc.) while the user is actually looking at it. */
    if (!player_screen || lv_screen_active() == player_screen) return;
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    lv_draw_buf_t * fresh = snapshot_screen_base(player_screen);
    if (!fresh) return; /* OOM -- keep whatever's cached (stale beats nothing); still tried again on the next dirty notification */
    if (player_transition_cache_buf) lv_draw_buf_destroy(player_transition_cache_buf);
    player_transition_cache_buf = fresh;
    player_transition_cache_dirty = false;
#ifdef UI_PERF_TRACE
    printf("PERF player_cache rebuild_us=%llu\n", (unsigned long long) (ui_perf_now_us() - perf_start_us));
#endif
}

static void player_transition_cache_async_cb(void * unused) {
    (void) unused;
    /* Re-checked here, not just at the mark_dirty() call site: several
     * dirty notifications can each schedule their own async callback
     * before the first one runs (lv_async_call() doesn't dedupe by
     * callback/data), so every call after the first one that actually
     * rebuilds is a cheap no-op instead of a redundant re-snapshot. */
    if (player_transition_cache_dirty) player_transition_rebuild_cache();
}

/* Called from every site where something visible on player_screen's own
 * subtree changes -- see this function's own doc comment on the cache
 * above for the current full list of call sites. Deliberately NOT called
 * from the routine per-second progress-bar update. */
void player_transition_mark_dirty(void) {
    player_transition_cache_dirty = true;
    if (player_screen) lv_async_call(player_transition_cache_async_cb, NULL);
}

typedef struct {
    lv_obj_t * overlay;
    lv_obj_t * img_from;
    lv_obj_t * img_to;
    lv_draw_buf_t * buf_from;
    lv_draw_buf_t * buf_to;
    bool buf_from_owned;
    bool buf_to_owned;
    bool fallback_bands_suppressed;
    bool home_indicator_was_hidden;
    lv_obj_t * from_scr;
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

/* Forward declarations -- real (first) tentative/actual definitions
 * further down this file, alongside poll_quick_drawer_drag()'s own
 * player-swipe state. slide_transition_anim_x_cb()'s compositor-failure
 * abort path below needs to clear these directly: a hard compositor
 * failure can happen mid-drag, and by the time it's detected, transition_
 * compositor_frame() has already torn down the compositor session itself
 * (LVGL invalidation restored, buf_act re-synced) -- what's left is purely
 * this file's OWN gesture-tracking bookkeeping, which only these three
 * variables (plus the ctx/anim cleanup already shared with the normal
 * commit/cancel path) actually hold. */
static bool player_swipe_candidate;
static bool player_swipe_tracking;
static slide_transition_ctx_t * player_swipe_ctx;

/* Shared by two unrelated callers -- both need "the next real
 * lv_timer_handler() pass redraws literally everything" without calling
 * lv_refr_now() synchronously from inside a callback that's itself already
 * running from within an lv_timer_handler() pass (a real reentrancy risk,
 * not just a style preference): slide_transition_anim_x_cb()'s compositor-
 * failure recovery (see its own comment), and update_timer_cb()'s screen-
 * wake handling (NEXT_TODO_IMPLEMENTATION_PROMPT.md Task 1) -- the async
 * call runs within that same lv_timer_handler() invocation, just after all
 * timers finish, which is still effectively immediate either way. */
static void full_redraw_async_cb(void * unused) {
    (void) unused;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
}

static void slide_transition_anim_x_cb(void * var, int32_t v) {
    slide_transition_ctx_t * ctx = (slide_transition_ctx_t *) var;
    /* TRANSITION_PERFORMANCE_PLAN.md Phase 3 -- when begin_slide_transition()
     * below handed this transition off to the direct-framebuffer compositor,
     * img_from/img_to are never drawn at all (LVGL invalidation is disabled
     * for the whole session), so driving them with lv_obj_set_x() here would
     * be pure wasted work: skip straight to compositing this frame instead.
     * Every caller of this function (screen_transition_slide()'s own
     * lv_anim_t, poll_quick_drawer_drag()'s per-tick interactive drag, and
     * its release settle animation) goes through this one shared path, so
     * all three get compositor support with no changes of their own. */
    if (transition_compositor_is_active()) {
        if (!transition_compositor_frame(v)) {
            /* Hard presentation failure. transition_compositor_frame()
             * already restored normal fbdev/LVGL ownership. Recover to a
             * deterministic logical screen: a fixed transition or a
             * released/committed gesture keeps its destination (the nav
             * stack has already been updated); an in-progress or cancelled
             * gesture returns to the source (and has made no stack change).
             * The deferred invalidation avoids a re-entrant lv_refr_now()
             * from inside an animation/timer callback. */
            lv_anim_delete(ctx, slide_transition_anim_x_cb); /* safe no-op if ctx isn't driven by a real lv_anim_t (the raw per-tick interactive-drag path isn't) */
            lv_obj_t * recovery_scr = ctx->commit ? ctx->to_scr : ctx->from_scr;
            lv_screen_load(recovery_scr);
            sync_player_topbar_visibility(recovery_scr);
            lv_async_call(full_redraw_async_cb, NULL);
            if (ctx->overlay) lv_obj_delete(ctx->overlay);
            if (ctx->buf_from_owned) lv_draw_buf_destroy(ctx->buf_from);
            if (ctx->buf_to_owned) lv_draw_buf_destroy(ctx->buf_to);
            if (player_swipe_ctx == ctx) player_swipe_ctx = NULL;
            player_swipe_tracking = false;
            player_swipe_candidate = false;
            lv_free(ctx);
            slide_transition_active = false;
        }
        return;
    }
    lv_obj_set_x(ctx->img_from, v);
    lv_obj_set_x(ctx->img_to, v + ctx->to_offset);
}

static void slide_transition_done_cb(lv_anim_t * a) {
    slide_transition_ctx_t * ctx = (slide_transition_ctx_t *) lv_anim_get_user_data(a);
    lv_obj_t * final_scr = ctx->commit ? ctx->to_scr : ctx->from_scr;
    if (ctx->commit) {
        lv_screen_load(ctx->to_scr);
    }
    /* The fallback owns flattened full-screen images, including both
     * persistent bands, so their live layer objects stay hidden throughout
     * the slide. Restore the status bar for the screen actually selected
     * and restore the screen-independent home-indicator setting now. */
    sync_player_topbar_visibility(final_scr);
    if (ctx->fallback_bands_suppressed && home_indicator_band) {
        if (ctx->home_indicator_was_hidden)
            lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->overlay) lv_obj_delete(ctx->overlay); /* deletes img_from/img_to too -- NULL when this transition was handed to the compositor instead (see begin_slide_transition()'s own comment on why the overlay is skipped entirely there, not just left undrawn) */
    if (ctx->buf_from_owned) lv_draw_buf_destroy(ctx->buf_from);
    if (ctx->buf_to_owned) lv_draw_buf_destroy(ctx->buf_to);
    transition_compositor_end(); /* no-op if the compositor was never activated for this transition */
    lv_free(ctx);
    slide_transition_active = false;
}

/* Moved up from its own neighborhood further down (build_home_indicator_bar())
 * -- a plain #define has no location dependency on where home_indicator_band
 * itself is built, and blend_overlay_onto_base() (used by
 * build_flattened_transition_frame() above) reads that object's own real
 * on-screen coordinates directly rather than this constant, so nothing
 * below actually requires this specific placement anymore; left here since
 * moving it back offers no benefit either. */
#define HOME_INDICATOR_BAND_HEIGHT 34

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
 * Both transition sources are always OWNED, independent copies now
 * (TRANSITION_PERFORMANCE_PLAN.md Phase 3) -- never a live alias of real
 * framebuffer memory, and never a direct reference to a cache buffer that
 * something else could destroy out from under this transition. See this
 * function's own body for the two separate real-device reasons: (1) a
 * live-aliased outgoing frame bled through live redraws happening on
 * from_scr during a held drag ("swiping to enter the player causes the
 * main menu to flicker"); (2) once the direct-framebuffer compositor is
 * involved, an aliased source becomes the compositor's OWN write target
 * again a couple of frames later (the two physical pages ping-pong every
 * frame), an overlapping-memcpy hazard, not just a staleness question. */
static slide_transition_ctx_t * begin_slide_transition(lv_obj_t * to_scr, bool forward) {
#ifdef UI_PERF_TRACE
    uint64_t perf_begin_us = ui_perf_now_us();
    uint64_t perf_to_done_us;
    uint64_t perf_drain_done_us;
    uint64_t perf_from_done_us;
#endif
    lv_obj_t * from_scr = lv_screen_active();
    if (from_scr == to_scr || slide_transition_active) return NULL;

    lv_display_t * disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    slide_transition_ctx_t * ctx = lv_malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->commit = true;
    ctx->buf_from_owned = true;
    ctx->buf_to_owned = true;
    ctx->fallback_bands_suppressed = false;
    ctx->home_indicator_was_hidden = true;

    /* Incoming source prepared FIRST, before the outgoing physical-page
     * capture below -- TRANSITION_PERFORMANCE_PLAN.md Phase 3 real-device
     * review finding. Always a COMPLETE flattened frame -- screen content
     * plus whichever persistent bars would really be visible there, never
     * a bare screen-only snapshot. The (comparatively expensive) screen
     * content itself is pre-baked for the fixed STATIC_SNAPSHOT_SCREEN_COUNT
     * screens or the Phase 2 player-transition cache when available (each
     * DUPLICATED here, never referenced directly -- the player cache can
     * be destroyed and replaced by an lv_async_call() rebuild firing mid-
     * gesture, a real use-after-free risk if referenced instead of
     * copied), else captured fresh synchronously; the persistent bars
     * (status bar/home indicator) are always blended in fresh right here,
     * regardless of which base was used -- see blend_persistent_bars()'s
     * own comment on why baking them into either cache would go stale. */
    lv_draw_buf_t * buf_to;
#ifdef UI_PERF_TRACE
    bool used_player_cache = false;
#endif
    lv_draw_buf_t * cached_base = get_static_snapshot(to_scr);
    if (!cached_base && to_scr == player_screen && player_transition_cache_buf) {
        cached_base = player_transition_cache_buf;
#ifdef UI_PERF_TRACE
        used_player_cache = true;
#endif
    }
    if (cached_base) {
        buf_to = lv_draw_buf_dup(cached_base);
        if (buf_to) blend_persistent_bars(buf_to, to_scr);
    } else {
        buf_to = build_flattened_transition_frame(to_scr);
    }
#ifdef UI_PERF_TRACE
    perf_to_done_us = ui_perf_now_us();
#endif

    /* Drain any already-queued LVGL rendering NOW, before capturing the
     * outgoing physical page -- TRANSITION_PERFORMANCE_PLAN.md Phase 3
     * real-device review finding: capturing the physical page first and
     * draining afterward (the earlier design, inside transition_
     * compositor_begin()) meant a pending redraw could still pan to a
     * DIFFERENT physical page after the capture, leaving frame zero of the
     * animation showing an already-stale image that visibly jumped
     * backward once the real (post-drain) state caught up. This driver's
     * flush_cb() is fully synchronous (no deferred/async flush
     * completion), so one lv_refr_now() call is guaranteed to fully
     * render AND flush everything pending before returning -- nothing can
     * still be "in flight" by the time the capture below runs.
     * transition_compositor_begin() itself no longer does this drain --
     * doing it here, before capture, is what actually matters; doing it
     * again inside begin() (after capture) would be too late. */
    lv_refr_now(disp);
#ifdef UI_PERF_TRACE
    perf_drain_done_us = ui_perf_now_us();
#endif

    /* Outgoing source: an owned copy of the REAL physical scanout page,
     * captured AFTER the drain above -- TRANSITION_PERFORMANCE_PLAN.md
     * Phase 3 fix. lv_linux_fbdev_get_active_page() is the fbdev driver's
     * own accessor for whichever physical half is actually being scanned
     * out right now; LVGL's generic lv_display_get_buf_active() (disp->
     * buf_act) is simply whichever buffer LVGL itself last rendered into
     * -- normally the same thing, but not a driver-level guarantee in
     * DIRECT double-buffered mode, so it's only the fallback here (host/
     * SDL builds, or if fbdev pan-based double buffering isn't active on
     * this display). */
    lv_draw_buf_t * buf_from = NULL;
#if LV_USE_LINUX_FBDEV
    {
        const void * phys_active = lv_linux_fbdev_get_active_page(disp);
        uint32_t fb_stride = lv_linux_fbdev_get_stride(disp);
        if (phys_active && fb_stride != 0) {
            lv_draw_buf_t phys_desc;
            memset(&phys_desc, 0, sizeof(phys_desc));
            phys_desc.header.magic = LV_IMAGE_HEADER_MAGIC;
            phys_desc.header.cf = LV_COLOR_FORMAT_RGB565;
            phys_desc.header.w = (uint32_t) w;
            phys_desc.header.h = (uint32_t) h;
            phys_desc.header.stride = fb_stride;
            phys_desc.data = (uint8_t *) phys_active; /* lv_draw_buf_dup() below only reads this -- never written through phys_desc itself */
            phys_desc.data_size = fb_stride * (uint32_t) h;
            buf_from = lv_draw_buf_dup(&phys_desc);
        }
    }
#endif
    if (!buf_from) {
        lv_draw_buf_t * active_buf = lv_display_get_buf_active(disp);
        buf_from = active_buf ? lv_draw_buf_dup(active_buf) : lv_snapshot_take(from_scr, LV_COLOR_FORMAT_RGB565);
    }
#ifdef UI_PERF_TRACE
    perf_from_done_us = ui_perf_now_us();
#endif
    if (!buf_from || !buf_to) {
        /* Snapshot failed (e.g. OOM) -- caller falls back to an instant cut
         * rather than crash or get stuck mid-navigation/mid-drag. */
        if (buf_from && ctx->buf_from_owned) lv_draw_buf_destroy(buf_from);
        if (buf_to && ctx->buf_to_owned) lv_draw_buf_destroy(buf_to);
        lv_free(ctx);
        return NULL;
    }

    int32_t to_offset = forward ? w : -w;
    ctx->buf_from = buf_from;
    ctx->buf_to = buf_to;
    ctx->from_scr = from_scr;
    ctx->to_scr = to_scr;
    ctx->to_offset = to_offset;

    slide_transition_active = true;

    /* TRANSITION_PERFORMANCE_PLAN.md Phase 3 -- try to hand this transition
     * off to the direct-framebuffer compositor BEFORE creating any LVGL
     * overlay/image objects, not after. Real-device bug (earlier design):
     * creating those objects first queued their own initial-draw
     * invalidation via the normal, still-enabled path, which then got
     * rendered and flushed for real on the next lv_timer_handler() tick
     * regardless of disabling invalidation moments later -- visible as a
     * leftover static image flashing on top of the compositor's own
     * correctly-sliding frames. Skipping the overlay entirely when the
     * compositor takes over removes the problem at its root; transition_
     * compositor_begin() itself also drains any OTHER already-queued LVGL
     * rendering (lv_refr_now()) before disabling invalidation, so nothing
     * unrelated is left stranded in the queue either. */
    if (transition_compositor_begin(buf_from, buf_to, to_offset)) {
        ctx->overlay = NULL;
        ctx->img_from = NULL;
        ctx->img_to = NULL;
    } else {
        /* buf_from/buf_to already contain the persistent bands. Hide the
         * live layer copies for the whole fallback animation so they move
         * exactly once as part of those flattened frames instead of being
         * drawn a second time, stationary, above the sliding images. */
        ctx->fallback_bands_suppressed = true;
        if (home_indicator_band) {
            ctx->home_indicator_was_hidden =
                lv_obj_has_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_bar_band)
            lv_obj_add_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t * overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(overlay);
        lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_pos(overlay, 0, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE); /* swallow touches while the slide is in flight */
        /* Keep transient top-layer UI such as the volume popup above the
         * transition. The persistent bands themselves are hidden above. */
        lv_obj_move_to_index(overlay, 0);

        lv_obj_t * img_from = lv_image_create(overlay);
        lv_image_set_src(img_from, buf_from);
        lv_obj_set_pos(img_from, 0, 0);

        lv_obj_t * img_to = lv_image_create(overlay);
        lv_image_set_src(img_to, buf_to);
        lv_obj_set_pos(img_to, to_offset, 0);

        ctx->overlay = overlay;
        ctx->img_from = img_from;
        ctx->img_to = img_to;
    }
#ifdef UI_PERF_TRACE
    uint64_t perf_end_us = ui_perf_now_us();
    printf("PERF transition begin_us=%llu to_us=%llu drain_us=%llu from_us=%llu setup_us=%llu from_owned=%d to_owned=%d player_cache=%d cache_dirty=%d compositor=%d\n",
           (unsigned long long) (perf_end_us - perf_begin_us),
           (unsigned long long) (perf_to_done_us - perf_begin_us),
           (unsigned long long) (perf_drain_done_us - perf_to_done_us),
           (unsigned long long) (perf_from_done_us - perf_drain_done_us),
           (unsigned long long) (perf_end_us - perf_from_done_us),
           ctx->buf_from_owned, ctx->buf_to_owned,
           used_player_cache, player_transition_cache_dirty, transition_compositor_is_active());
#endif
    return ctx;
}

static void screen_transition_slide(lv_obj_t * to_scr, bool forward) {
    slide_transition_ctx_t * ctx = begin_slide_transition(to_scr, forward);
    if (!ctx) {
        lv_screen_load(to_scr);
        sync_player_topbar_visibility(to_scr);
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

void nav_push(lv_obj_t * scr) {
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    if (nav_depth > 0 && nav_stack[nav_depth - 1] == scr) {
        lv_screen_load(scr); /* already the active screen -- nothing to push */
        sync_player_topbar_visibility(scr);
        return;
    }
    if (nav_depth < NAV_STACK_MAX) {
        nav_stack[nav_depth++] = scr;
    }
    /* Live A/B test: forward navigation (entering a submenu) now cuts
     * instantly instead of sliding -- screen_transition_slide() is still
     * used by nav_pop() below, so backing out still animates. */
    lv_screen_load(scr);
    sync_player_topbar_visibility(scr);
#ifdef UI_PERF_TRACE
    printf("PERF nav_push load_us=%llu depth=%d\n",
           (unsigned long long) (ui_perf_now_us() - perf_start_us), nav_depth);
#endif
}

void nav_pop(void) {
    if (nav_depth > 1) nav_depth--;
    /* Keep the outgoing screen's bars untouched until its physical frame
     * has been captured. The transition completion/cut-fallback path
     * applies the destination state at the actual screen handoff. */
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
void nav_remove_stack_slot(int index) {
    for (int i = index; i < nav_depth - 1; i++) nav_stack[i] = nav_stack[i + 1];
    if (nav_depth > 0) nav_depth--;
}

/* Collapses the whole nav stack back to Home -- used after a library rescan,
 * since any deeper screen (Artists/Albums/group songs/...) may be showing
 * rows built from the pre-rescan data and would otherwise still be reachable
 * via back-navigation. */
void nav_reset_to_home(void) {
    nav_depth = 1;
    nav_stack[0] = home_screen;
    lv_screen_load(home_screen);
    sync_player_topbar_visibility(home_screen);
}

/* Shared back-button handler for every screen built via the reusable
 * icon-grid/pill-list builders -- passed directly as their back_btn_cb. */
void generic_back_cb(lv_event_t * e) {
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
void enable_gesture_bubble_recursive(lv_obj_t * obj) {
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
/* Defined later alongside player_swipe_press_excluded()'s own raw-polling
 * dead-zone machinery -- needed here too, by screen_gesture_event_cb()
 * below, see its own comment. */
static bool active_press_is_over_drag_adjust_widget(void);
static bool point_in_swipe_dead_zone(lv_point_t p);
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

static void screen_gesture_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) return;
    lv_indev_t * indev = lv_indev_active();
    if (!indev) return;

    /* Real-device bug report: dragging the player screen's own seek bar
     * (progress_slider) still triggered the back-swipe. Widening its
     * ext_click_area (see progress_slider's own comment) wasn't enough on
     * its own -- LVGL's gesture recognition runs throughout a slider drag
     * regardless (sliders clear LV_OBJ_FLAG_SCROLLABLE in their
     * constructor, so the generic scroll_obj early-exit indev_gesture()
     * relies on elsewhere never applies to one), and real-device testing
     * showed this still reaching here rather than staying resolved to the
     * slider itself the way GESTURE_BUBBLE exclusion (enable_gesture_
     * bubble_recursive()) was expected to guarantee. Reusing the same two
     * checks player_swipe_press_excluded() already combines for the
     * separate raw-polling player-swipe path: active_press_is_over_drag_
     * adjust_widget() (hit-tested object identity -- covers a press that
     * lands squarely on progress_slider) plus point_in_swipe_dead_zone()
     * (raw point-in-rect against the registered dead-zone list --
     * progress_slider is registered there too, see its own
     * register_swipe_dead_zone() call, covering a press that lands just
     * off it) closes the gap regardless of exactly which part of LVGL's
     * own gesture-bubbling chain let it through. */
    lv_point_t gesture_press_point;
    lv_indev_get_point(indev, &gesture_press_point);
    if (active_press_is_over_drag_adjust_widget() || point_in_swipe_dead_zone(gesture_press_point)) return;

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
void finalize_screen_navigation(lv_obj_t * scr) {
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

void sync_player_topbar_visibility(lv_obj_t * screen) {
    /* Settings > Display > "Hide Player/Lyrics Top Bar" -- hides the global
     * status bar while the Player or its fullscreen Lyrics view is active;
     * every other screen keeps its status bar as normal regardless of this
     * setting. player_dismiss_btn (Player's own standalone back arrow) is
     * additionally tied to the same setting, Player-only -- when the
     * status bar is hidden there's no other visible way back short of the
     * swipe/hardware-button gesture, matching the immersive intent; Lyrics
     * has no equivalent standalone back button of its own. Real, live
     * object state here is allowed to reflect "whatever the user last
     * navigated to" -- correctness for the Phase 2 transition CACHE (built
     * while Player is inactive, so this function's own object-flag state
     * can't be trusted for it) is handled independently by
     * build_flattened_transition_frame()'s own temporary-flag-then-restore
     * approach, not by this function. */
    bool hide = current_settings.hide_player_topbar && (screen == player_screen || screen == gui_lyrics_get_screen());
    if (status_bar_band) {
        if (hide) lv_obj_add_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN);
    }
    if (player_dismiss_btn) {
        if (current_settings.hide_player_topbar && screen == player_screen)
            lv_obj_add_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* player_transition_rebuild_cache() (see its own doc comment) refuses to
     * run while player_screen is still the active one -- which, since
     * whatever marked the cache dirty (track change, cover art, play/pause,
     * accent color) almost always happens WHILE the user is looking at the
     * Player screen, is exactly the state the cache is usually dirtied in.
     * Its own lv_async_call() only ever fires once, right after being
     * scheduled, so without this it would stay permanently dirty from that
     * point on -- confirmed on-device (every "PERF transition" line showing
     * player_cache=0 cache_dirty=1, never once actually using the cache).
     * This function already runs as the last step of every real navigation
     * (nav_push/nav_pop/screen_transition_slide's cut fallback/
     * slide_transition_done_cb's commit/nav_reset_to_home), i.e. exactly
     * "after the Player has settled" -- so retrying here, once per actual
     * screen change away from Player, is the natural moment. */
    if (screen != player_screen && player_transition_cache_dirty)
        lv_async_call(player_transition_cache_async_cb, NULL);
}

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
    clock_topbar_ampm = lv_image_create(clock_topbar_group);
    lv_image_set_src(clock_topbar_ampm, asset_path("topbar/am.png"));
    lv_image_set_scale(clock_topbar_ampm, LV_SCALE_NONE);
    lv_obj_add_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN); /* refresh_clock_label() unhides this if clock_24h is off */

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
    lv_obj_align_to(battery_topbar_group, battery_icon_frame, LV_ALIGN_OUT_LEFT_MID, -5, 0);

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

/* Defined further down (near wifi_icon/bt_status_icon's own setup) --
 * forward-declared here so refresh_battery_topbar() below can re-run it
 * whenever battery_topbar_group's own visibility might have changed
 * (unknown percent, or Settings > Power > "Battery Percentage" toggling),
 * since that group is one of the two anchors that logic positions the
 * wifi/bt topbar icons against. */
static void sync_topbar_status_icon_positions(void);

static void refresh_battery_topbar(void) {
    int percent = battery_get_display_percent();

    /* The fuel gauge can recalibrate upward after charging is stopped, and
     * the kernel's preferred battery/status node remains stale at
     * "Charging" even while AXP2101 REG18 has chg_en cleared and REG01 says
     * not_charging. Without this limiter-aware presentation, battery.c's
     * direction filter walks the visible number from 85 toward that stale
     * raw value, making a working electrical cutoff look broken. Keep the
     * raw percentage untouched for charge_limiter_poll()'s hysteresis; only
     * cap what the 85%-limit UI promises to show while a hold is active. */
    bool charge_limiter_holding = charge_limiter_is_holding();
    if (charge_limiter_holding && percent > 85) percent = 85;

    /* battery_icon_frame (the outline + fill gauge) is always shown --
     * current_settings.show_battery_percent (Settings > Power > "Battery
     * Percentage") only ever hides the "NN%" digit readout below, never the
     * icon itself. Edge-triggered (compares against the group's own current
     * hidden-flag rather than setting it unconditionally every call) since
     * this whole function runs every tick the screen is on -- re-syncing
     * the wifi/bt icon positions that often, on every tick, for a flag that
     * only ever changes on a battery-unplugged/replugged edge or a Settings
     * toggle, would be pure churn. */
    bool percent_should_show = percent >= 0 && current_settings.show_battery_percent;
    bool percent_was_shown = !lv_obj_has_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
    if (percent_should_show != percent_was_shown) {
        if (percent_should_show) lv_obj_remove_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
        sync_topbar_status_icon_positions();
    }

    if (percent < 0) {
        lv_obj_add_flag(battery_icon_fill_clip, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(battery_icon_frame, asset_path("topbar/battery_bg.png"));
        return;
    }
    if (percent > 100) percent = 100;

    /* Real-device bug report: the charging bolt disappeared as soon as the
     * 85% limiter was enabled, well before the battery actually got there.
     * Root cause: charge_limiter_holding tracks charge_limiter.c's own
     * hysteresis state, which flips true one point EARLY (at 84%, see
     * CHARGE_LIMITER_TRIGGER_PERCENT's own comment -- deliberate, to absorb
     * fuel-gauge lag before the real 85% cutoff) and, being sticky
     * hysteresis, can stay true from a previous session even once the
     * displayed percent has since dropped a little without crossing the
     * lower CHARGE_LIMITER_RESUME_PERCENT reset point. Neither case means
     * the battery has actually reached the 85% this UI promises -- only
     * suppress the charging icon once the displayed percent has genuinely
     * gotten there too, matching what's on screen rather than the internal
     * hysteresis flag alone. */
    bool limiter_capped_now = charge_limiter_holding && percent >= 85;
    bool charging = !limiter_capped_now && battery_is_charging();
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

/* Real-device bug report: wifi_icon and bt_status_icon originally each sat
 * at their own hand-tuned fixed offset from battery_icon_frame -- fine when
 * both or neither were showing, but with only one of the two radios on, the
 * other's now-hidden slot was left as a dead gap between the visible icon
 * and the battery percentage instead of the visible one sliding over to sit
 * right next to it. Fix: track which of the two is CURRENTLY closer to the
 * battery (order[0], the inner slot) vs. one slot further out (order[1]),
 * and re-derive it from scratch on every call rather than mutating an
 * existing arrangement in place -- simpler and can't drift out of sync with
 * the two icons' own hidden-flag state, the actual source of truth, which
 * is all this ever reads. Whichever of the two is currently visible AND was
 * already occupying a slot keeps it; a newly-visible icon takes whichever
 * slot (if any) is still free. This is what gives "closer to the battery"
 * its "whichever appeared first" ordering from the bug report: the icon
 * that was already on when the second one turns on keeps the inner slot
 * instead of being displaced, and the moment either disappears the survivor
 * (if any) is pulled into the inner slot so there's never a gap. The two
 * slots are positioned relative to battery_topbar_group/battery_icon_frame
 * (not a fixed offset), further down -- see that comment for how Settings >
 * Power > "Battery Percentage" folds into the same anchor logic. */
typedef enum {
    TOPBAR_STATUS_ICON_NONE = 0,
    TOPBAR_STATUS_ICON_WIFI,
    TOPBAR_STATUS_ICON_BT,
} topbar_status_icon_t;

static topbar_status_icon_t topbar_status_icon_order[2] = { TOPBAR_STATUS_ICON_NONE, TOPBAR_STATUS_ICON_NONE };

static void sync_topbar_status_icon_positions(void) {
    bool wifi_visible = !lv_obj_has_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    bool bt_visible = !lv_obj_has_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);

    topbar_status_icon_t new_order[2] = { TOPBAR_STATUS_ICON_NONE, TOPBAR_STATUS_ICON_NONE };
    int slot = 0;
    /* Existing occupants first, in their current order, so an icon that's
     * still visible never moves slots just because the other one's
     * visibility also happened to change on this same call. */
    for (int i = 0; i < 2 && slot < 2; i++) {
        topbar_status_icon_t icon = topbar_status_icon_order[i];
        if ((icon == TOPBAR_STATUS_ICON_WIFI && wifi_visible) || (icon == TOPBAR_STATUS_ICON_BT && bt_visible)) {
            new_order[slot++] = icon;
        }
    }
    /* Then any newly-visible icon not already placed above, oldest-checked
     * (wifi) first -- only matters when both go from hidden to visible on
     * the exact same call, an arbitrary but stable tiebreak. */
    if (wifi_visible && new_order[0] != TOPBAR_STATUS_ICON_WIFI && new_order[1] != TOPBAR_STATUS_ICON_WIFI && slot < 2) {
        new_order[slot++] = TOPBAR_STATUS_ICON_WIFI;
    }
    if (bt_visible && new_order[0] != TOPBAR_STATUS_ICON_BT && new_order[1] != TOPBAR_STATUS_ICON_BT && slot < 2) {
        new_order[slot++] = TOPBAR_STATUS_ICON_BT;
    }
    topbar_status_icon_order[0] = new_order[0];
    topbar_status_icon_order[1] = new_order[1];

    /* Chained anchoring, not fixed offsets -- Settings > Power > "Battery
     * Percentage" (current_settings.show_battery_percent) lets the "NN%"
     * readout be turned off entirely (battery_topbar_group hidden by
     * refresh_battery_topbar() in that case, battery_icon_frame itself
     * always stays visible -- see its own comment). When the percentage is
     * showing, the inner slot sits left of battery_topbar_group, same gap
     * that group's own anchor to battery_icon_frame already uses; when it's
     * off, the inner slot moves in to sit left of battery_icon_frame
     * directly, closing the gap the percentage would otherwise have left. */
    lv_obj_t * anchor = (current_settings.show_battery_percent && !lv_obj_has_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN))
                             ? battery_topbar_group
                             : battery_icon_frame;
    for (int i = 0; i < 2; i++) {
        lv_obj_t * widget = topbar_status_icon_order[i] == TOPBAR_STATUS_ICON_WIFI  ? wifi_icon
                            : topbar_status_icon_order[i] == TOPBAR_STATUS_ICON_BT ? bt_status_icon
                                                                                    : NULL;
        if (!widget) continue;
        lv_obj_align_to(widget, anchor, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        anchor = widget;
    }
}

/* The drawer's own wifi icon just reflects radio-on/off (blue as soon as
 * enabled, real-device feedback: the connected-vs-just-enabled distinction
 * is a top-bar-only thing) -- the top bar icon keeps the finer-grained
 * enabled-vs-actually-associated-to-an-AP distinction below. */
static void refresh_wifi_icon(void) {
    bool enabled = wifi_control_is_enabled();
    if (quick_drawer_wifi_icon) {
        lv_image_set_src(quick_drawer_wifi_icon, asset_path(enabled ? "pull_down/wifi_s.png" : "pull_down/wifi.png"));
        quick_drawer_mark_snapshot_dirty();
    }

    if (!enabled) {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        sync_topbar_status_icon_positions();
        return;
    }
    lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    sync_topbar_status_icon_positions();

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
bool bt_is_powered_cached = false;

/* The connected A2DP accessory's own MAC + live-negotiated codec, kept
 * fresh by the same background refresh_bt_icon_thread_func() poll as
 * bt_is_powered_cached above -- add_bt_device_row() (Bluetooth screen) uses
 * these to know which paired-device row is the actual A2DP-audio one (not
 * just "connected" -- a non-audio BLE peripheral could be connected too)
 * and what to print on its second line. Empty when nothing's A2DP-connected. */
char bt_connected_mac_cached[18] = "";
char bt_connected_codec_cached[32] = "";

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
 * The mask is now state-based rather than time-based: it remains active
 * until bt_init has written its tmpfs completion marker AND two subsequent
 * polls agree the adapter is off. A manual toggle ends it immediately, so
 * explicit user intent always wins. Starting the player later in the same
 * boot does not enable the mask because bt_init_ok already exists; resume
 * likewise never re-enters gui_init(). */
static bool bt_boot_suppress_enabled = false;
static unsigned int bt_boot_off_observations = 0;
#define BT_BOOT_OFF_OBSERVATIONS_REQUIRED 2

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
    return bt_boot_suppress_enabled;
}

/* Moved up from the Bluetooth settings screen section further down (still
 * used there, see populate_bt_screen()) -- refresh_bt_icon_thread_func()
 * below needs to reference bt_scan_results directly, before its own
 * definition down there, to fix a real-device bug: the settings screen's
 * device list only ever reflected paired/connected state as of the last
 * explicit scan, never refreshed afterward unlike the top-bar icon (see
 * poll_refresh_bt_icon()'s own comment on the merge step below). */
#define BT_MAX_RESULTS 32
bt_device_t bt_scan_results[BT_MAX_RESULTS];
int bt_scan_result_count = 0;

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
static atomic_bool refresh_bt_icon_done_flag = false;
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

    atomic_store_explicit(&refresh_bt_icon_done_flag, true, memory_order_release); /* written last -- poll_refresh_bt_icon only checks this flag */
    return NULL;
}

static void start_refresh_bt_icon(void) {
    if (refresh_bt_icon_active) return; /* previous check still in flight -- same "ignore taps until it lands" pattern as everything else here */
    refresh_bt_icon_active = true;
    atomic_store_explicit(&refresh_bt_icon_done_flag, false, memory_order_relaxed);
        if (pthread_create(&refresh_bt_icon_thread, NULL, refresh_bt_icon_thread_func, NULL) != 0) {
        refresh_bt_icon_active = false;
    }
}

static bool bt_toggle_active; /* defined with the rest of the tap-to-toggle mechanism, below -- see poll_refresh_bt_icon()'s own use of it */

static void poll_refresh_bt_icon(void) {
    if (!refresh_bt_icon_active || !atomic_load_explicit(&refresh_bt_icon_done_flag, memory_order_acquire)) return;
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

    /* Do not graduate based on elapsed time. A worker can start its
     * bluetoothctl query during bt_init's transient powered interval and
     * deliver that stale "on" result after a timer expires, which is the
     * exact cold-boot flash this mask exists to prevent. Instead require
     * bt_init's tmpfs completion marker plus two settled "off" results.
     * Resume never enters this state, and a deliberate user tap clears it
     * immediately in quick_drawer_bt_event_cb(). */
    if (bt_boot_suppress_active()) {
        if (access(BT_INIT_OK_FLAG_PATH, F_OK) == 0 && !refresh_bt_icon_result_powered) {
            bt_boot_off_observations++;
            if (bt_boot_off_observations >= BT_BOOT_OFF_OBSERVATIONS_REQUIRED) {
                bt_boot_suppress_enabled = false;
            }
        } else {
            bt_boot_off_observations = 0;
        }
    }

    bool display_powered = refresh_bt_icon_result_powered;
    if (bt_boot_suppress_active()) display_powered = false;

    bt_is_powered_cached = display_powered;
    snprintf(bt_connected_mac_cached, sizeof(bt_connected_mac_cached), "%s", refresh_bt_icon_result_mac);
    snprintf(bt_connected_codec_cached, sizeof(bt_connected_codec_cached), "%s", refresh_bt_icon_result_codec);
    if (quick_drawer_bt_icon) {
        lv_image_set_src(quick_drawer_bt_icon, asset_path(display_powered ? "pull_down/bt_s.png" : "pull_down/bt.png"));
        quick_drawer_mark_snapshot_dirty();
    }

    if (!display_powered) {
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
        sync_topbar_status_icon_positions();
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
    sync_topbar_status_icon_positions();
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

/* The stock rail sprites are 360 px wide. LVGL centers a background image
 * at its native size instead of scaling it to the part, so they protrude
 * from the 340 px volume rail and the dynamically narrower brightness rail.
 * Paint those two rails natively; keep cursor.png only for the knob. */
static void configure_native_slider_rail(lv_obj_t * slider) {
    lv_obj_set_style_bg_image_src(slider, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(slider, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_make(132, 134, 132), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
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
    lv_obj_set_style_bg_image_src(volume_popup_track, asset_path("volume/cursor.png"), LV_PART_KNOB);
    /* Real-device bug report: accent color didn't apply here -- see
     * apply_accent_color()'s own comment on why an image-art slider needs
     * bg_image_recolor, not just bg_color. */
    lv_obj_add_style(volume_popup_track, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(volume_popup_track, &style_accent, LV_PART_KNOB);
    /* Flat (0), not this app's usual default, on the theory a rounded
     * corner might be receding from the indicator's own true edge -- kept
     * as a reasonable belt-and-suspenders choice even though real-device
     * testing (and a debug build with each part painted a distinct solid
     * color) showed this alone does NOT explain the actual gray-sliver bug
     * here. The bounded native rail below fixes the actual image-width
     * mismatch. */
    configure_native_slider_rail(volume_popup_track);
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
/* error_toast moved to gui_notifications.c */

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
/* info_toast moved to gui_notifications.c */

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
    quick_drawer_mark_snapshot_dirty();
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
lv_obj_t * settings_crossfade_toggle_img;
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
    quick_drawer_mark_snapshot_dirty();
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
        quick_drawer_mark_snapshot_dirty();
        return;
    }

    /* Round up so the label never shows "0m" for the last, still-live
     * sub-minute stretch -- counts down 15,14,...,1 then disarms above
     * rather than ever displaying a misleading zero. */
    int remaining_min = (int) ((total_ms - elapsed_ms + 59999) / 60000);
    lv_label_set_text_fmt(quick_drawer_sleep_label, "%dm", remaining_min);
    quick_drawer_mark_snapshot_dirty();
}

static void quick_drawer_anim_y_cb(void * var, int32_t v) {
    (void) var;
    lv_obj_set_y(quick_drawer_bitmap_motion ? quick_drawer_motion_image : quick_drawer, v);
}

static int32_t quick_drawer_motion_y(void) {
    return lv_obj_get_y(quick_drawer_bitmap_motion ? quick_drawer_motion_image : quick_drawer);
}

static void quick_drawer_rebuild_snapshot(void) {
    if (!quick_drawer || quick_drawer_bitmap_motion) return;
    lv_draw_buf_t * fresh = lv_snapshot_take(quick_drawer, LV_COLOR_FORMAT_RGB565);
    if (!fresh) return;
    if (!quick_drawer_motion_image) {
        quick_drawer_motion_image = lv_image_create(lv_layer_top());
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(quick_drawer_motion_image, NULL);
    }
    if (quick_drawer_motion_buf) lv_draw_buf_destroy(quick_drawer_motion_buf);
    quick_drawer_motion_buf = fresh;
    lv_image_set_src(quick_drawer_motion_image, quick_drawer_motion_buf);
    quick_drawer_snapshot_dirty = false;
}

static void quick_drawer_snapshot_async_cb(void * unused) {
    (void) unused;
    if (quick_drawer_snapshot_dirty && !quick_drawer_bitmap_motion)
        quick_drawer_rebuild_snapshot();
}

static void quick_drawer_mark_snapshot_dirty(void) {
    quick_drawer_snapshot_dirty = true;
    if (quick_drawer && !quick_drawer_bitmap_motion)
        lv_async_call(quick_drawer_snapshot_async_cb, NULL);
}

static bool quick_drawer_begin_bitmap_motion(void) {
    if (quick_drawer_bitmap_motion) return true;
    /* Never lv_snapshot_take() on the drag/animation tick: a full-panel
     * RGB565 snapshot is a multi-millisecond hitch on this SoC and was
     * the "dragging the drawer feels slow" report. Use a buffer already
     * built while idle, or follow the live panel. */
    if (quick_drawer_snapshot_dirty || !quick_drawer_motion_buf || !quick_drawer_motion_image)
        return false;
    lv_obj_set_y(quick_drawer_motion_image, lv_obj_get_y(quick_drawer));
    lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(quick_drawer_motion_image);
    lv_obj_move_foreground(status_bar_band);
    lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
    quick_drawer_bitmap_motion = true;
    return true;
}

static void quick_drawer_finish_bitmap_motion(void) {
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_set_y(quick_drawer, quick_drawer_open ? 0 : -h);
    lv_obj_remove_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
    if (quick_drawer_motion_image) {
        lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_bitmap_motion = false;
    if (quick_drawer_snapshot_dirty || quick_drawer_open)
        lv_async_call(quick_drawer_snapshot_async_cb, NULL);
}

static void quick_drawer_anim_done_cb(lv_anim_t * a) {
    (void) a;
    quick_drawer_finish_bitmap_motion();
}

static void open_quick_drawer(void) {
    if (quick_drawer_open) return;
    quick_drawer_open = true;
    refresh_quick_drawer_brightness(); /* see its own comment -- keeps the slider from showing a stale pre-screen-off value */
    quick_drawer_begin_bitmap_motion();
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
    lv_anim_set_values(&a, quick_drawer_motion_y(), 0);
    lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
    lv_anim_set_exec_cb(&a, quick_drawer_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, quick_drawer_anim_done_cb);
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
    quick_drawer_begin_bitmap_motion();
    lv_anim_set_values(&a, quick_drawer_motion_y(), -h);
    lv_anim_set_duration(&a, QUICK_DRAWER_ANIM_MS);
    lv_anim_set_exec_cb(&a, quick_drawer_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, quick_drawer_anim_done_cb);
    lv_anim_start(&a);
}

static bool library_rescan_active; /* defined with the rest of the Update Music Database rescan, below -- see poll_quick_drawer_drag()'s own use of it */

/* Handle stashed by gui_init() at creation time -- see poll_quick_drawer_
 * drag()'s own comment on why this timer runs at LV_DEF_REFR_PERIOD (~60fps)
 * instead of update_timer_cb's shared 500ms one. Paused by poll_quick_
 * drawer_drag() itself the instant nothing's pressed (so a ~60fps timer
 * doesn't sit registered forever, capping how long main()'s own idle
 * usleep() between lv_timer_handler() calls can ever be -- real cost even
 * though each individual idle tick barely does anything) and resumed by
 * resume_fast_gesture_timers_cb() (registered on the pointer indev, next to
 * this timer's own creation in gui_init()) the instant a new press begins
 * anywhere -- LV_EVENT_PRESSED is the one indev event LVGL dispatches
 * regardless of hit target (see poll_quick_drawer_drag()'s own doc comment
 * on why that's reliable here but LV_EVENT_PRESSING isn't). */
static lv_timer_t * quick_drawer_drag_timer = NULL;
static bool quick_drawer_drag_tracking = false;
static bool quick_drawer_drag_claimed = false;
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
lv_indev_t * find_pointer_indev(void) {
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

void register_swipe_dead_zone(lv_obj_t * obj) {
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
            quick_drawer_drag_panel_start_y = quick_drawer_motion_y();
        } else if (p.y <= QUICK_DRAWER_TRIGGER_ZONE && !library_rescan_active) {
            /* !library_rescan_active -- same exclusion as the other rescan-
             * time guards below (search this file for library_rescan_active
             * for the rest of them); only blocks starting a NEW open-drag,
             * if the drawer somehow got dragged open right as a rescan
             * started, the quick_drawer_open branch above still lets it be
             * dragged closed again. */
            quick_drawer_drag_tracking = true;
            quick_drawer_drag_panel_start_y = quick_drawer_motion_y();
            lv_obj_move_foreground(quick_drawer); /* above regular screens/volume popup while dragging into view */
            lv_obj_move_foreground(status_bar_band); /* but the status bar stays above THAT -- see open_quick_drawer()'s comment */
        } else {
            quick_drawer_drag_tracking = false;
        }
        quick_drawer_drag_claimed = false;
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
                               lv_screen_active() != gui_lyrics_get_screen() &&
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
         * gesture that could navigate off the rescan's busy screen. Also
         * excluded on gui_lyrics_get_screen() -- real-device feedback: triggering
         * this gesture from a screen that's already reached FROM the
         * player screen looked like a broken, looping transition; lyrics_
         * gesture_event_cb() is the only swipe this screen responds to. */
        player_swipe_candidate = !quick_drawer_drag_tracking && !quick_drawer_open &&
                                  lv_screen_active() != player_screen &&
                                  lv_screen_active() != gui_lyrics_get_screen() &&
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
                player_swipe_ctx = begin_slide_transition(player_screen, true); /* see begin_slide_transition()'s own comment -- both sources are always owned copies now */
                if (player_swipe_ctx) {
                    /* No navigation decision exists until release. A
                     * compositor failure during the live drag therefore
                     * recovers to from_scr and leaves the stack untouched. */
                    player_swipe_ctx->commit = false;
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
            /* Past the deadzone this is a drag, not a tap. The live drawer
             * does not cover the list while opening (it starts off-screen),
             * and bitmap motion hides the real panel behind a snapshot --
             * without wait_release(), LVGL re-hit-tests the still-down
             * finger onto whatever row is now underneath and fires CLICKED
             * on release. Same tool as the player-swipe path above. */
            if (!quick_drawer_drag_claimed) {
                quick_drawer_drag_claimed = true;
                lv_indev_wait_release(indev);
            }
            /* Per-tick velocity, in case the finger lifts mid-flick (see the
             * release branch below) -- a plain position delta rather than
             * lv_indev_get_vect() so it's driven by the exact same samples
             * this function already reads, not a second/possibly-
             * differently-timed source. */
            if (!quick_drawer_bitmap_motion) quick_drawer_begin_bitmap_motion();
            quick_drawer_last_velocity = new_y - quick_drawer_motion_y();
            quick_drawer_anim_y_cb(quick_drawer, new_y);
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
            snap_open = quick_drawer_motion_y() > -h / 2;
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
        /* player_swipe_last_v already holds exactly this value -- it's set
         * to the same v applied via slide_transition_anim_x_cb() every tick
         * just above, before either the LVGL-object or compositor path
         * consumes it. Reading it back here directly (instead of
         * lv_obj_get_x(player_swipe_ctx->img_from)) is required, not just
         * simpler, now that img_from can be NULL -- TRANSITION_PERFORMANCE_
         * PLAN.md Phase 3's compositor path skips creating it entirely (see
         * begin_slide_transition()'s own comment); real-device crash log
         * confirmed lv_obj_get_x(NULL) -> SIGSEGV (invalid read from 0x14,
         * lv_obj_get_x()'s own offset into a null lv_obj_t) the first time
         * this was reached under compositor mode. */
        int32_t current_v = player_swipe_last_v;
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

    /* Every release-handling branch above (drawer snap, player-swipe
     * settle) has already run by this point in the same call that observed
     * the release -- nothing left to track until resume_fast_gesture_
     * timers_cb() wakes this again on the next press-down. See this
     * timer's own handle comment for why pausing (not just letting the
     * ~60fps tick keep firing and no-op) is what actually matters here. */
    if (!pressed) lv_timer_pause(timer);
}

/* Forward declarations -- defined later in this file (with the player
 * screen's own transport buttons, which they were originally written for),
 * but the quick drawer's mini now-playing card reuses them verbatim. */
static void favorite_icon_event_cb(lv_event_t * e);
static void prev_btn_event_cb(lv_event_t * e);
static void play_btn_event_cb(lv_event_t * e);
static void next_btn_event_cb(lv_event_t * e);
static const char * play_mode_icon_asset(play_mode_t mode);
const char * basename_of(const char * path);
/* Defined much later, alongside the rest of the new Wi-Fi/Bluetooth
 * screens -- long-pressing the drawer's wifi/bt icons opens the real
 * settings screen for that radio, matching Android's quick-settings
 * convention (tap toggles, long-press opens the full screen). */

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
    quick_drawer_finish_bitmap_motion();
    open_wifi_screen();
}

static void quick_drawer_bt_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_bt_long_press_fired = true;
    quick_drawer_open = false;
    quick_drawer_finish_bitmap_motion();
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
static atomic_bool wifi_toggle_done_flag = false;
static bool wifi_toggle_target_enabled = false;

static void * wifi_toggle_thread_func(void * arg) {
    (void) arg;
    bool turning_on = wifi_toggle_target_enabled;
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

    atomic_store_explicit(&wifi_toggle_done_flag, true, memory_order_release); /* written last -- poll_wifi_toggle only checks this flag */
    return NULL;
}

/* populate_wifi_screen declared in gui.h */ /* defined with the rest of the Wi-Fi settings screen, below */

void quick_drawer_wifi_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (quick_drawer_wifi_long_press_fired) { /* see quick_drawer_wifi_long_press_cb()'s own comment */
        quick_drawer_wifi_long_press_fired = false;
        return;
    }
    if (wifi_toggle_active) return; /* already toggling -- ignore taps until it lands */
    bool wifi_will_be_enabled = !wifi_control_is_enabled();
    wifi_toggle_active = true;
    atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
    wifi_toggle_target_enabled = wifi_will_be_enabled;

    /* Optimistic sprite flip -- wifi_control_is_enabled() is a plain
     * access() check (see its own comment), not a subprocess spawn, so
     * it's cheap enough to call synchronously right here. The actual
     * radio toggle below can take a couple seconds; flipping the icon
     * immediately instead of waiting for poll_wifi_toggle() to confirm it
     * is what makes the tap read as instant. poll_wifi_toggle() still
     * re-reads the real state once the thread lands and corrects this if
     * the toggle unexpectedly failed. */
    lv_image_set_src(quick_drawer_wifi_icon, asset_path(wifi_will_be_enabled ? "pull_down/wifi_s.png" : "pull_down/wifi.png"));
    quick_drawer_mark_snapshot_dirty();

    /* Real-device bug report: the topbar Wi-Fi icon stayed frozen (hidden,
     * or showing whatever signal-strength sprite it had before toggling
     * off) until poll_wifi_toggle()'s own refresh_wifi_icon() call landed --
     * same delay-to-first-feedback bug already fixed for Bluetooth's own
     * topbar icon in quick_drawer_bt_event_cb(), same fix here: flip it
     * optimistically alongside the drawer icon just above. ON shows the
     * disconnected sprite (a fresh toggle-on can't be associated to an AP
     * yet); OFF hides it outright. refresh_wifi_icon() overwrites this with
     * the real, settled state (association status included) once
     * wifi_control_enable()/disable() actually finishes. */
    if (wifi_will_be_enabled) {
        lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(wifi_icon, asset_path("topbar/wifi_unconnect.png"));
    } else {
        lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
    }
    sync_topbar_status_icon_positions();

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
    /* Do not clean/rebuild wifi_list from inside the clicked row's own
     * event callback: doing so deletes the event target while LVGL is still
     * dispatching through it. poll_wifi_toggle() performs the authoritative
     * rebuild as soon as the worker settles. */
    if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0) {
        wifi_toggle_active = false;
        refresh_wifi_icon();
        if (nav_depth > 0 && nav_stack[nav_depth - 1] == wifi_screen)
            populate_wifi_screen(wifi_control_is_enabled());
    }
}

static void poll_wifi_toggle(void) {
    if (!wifi_toggle_active || !atomic_load_explicit(&wifi_toggle_done_flag, memory_order_acquire)) return;
    wifi_toggle_active = false;
    pthread_join(wifi_toggle_thread, NULL);
    bool enabled = wifi_control_is_enabled();
    refresh_wifi_icon(); /* re-reads the real state -- updates both the status bar and drawer icons */
    populate_wifi_screen(enabled); /* the Wi-Fi screen's own toggle row + everything gated on it needs the same refresh */
    if (enabled != wifi_toggle_target_enabled) show_error_toast("Wi-Fi failed to change state");
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
static atomic_bool bt_toggle_done_flag = false;


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

    atomic_store_explicit(&bt_toggle_done_flag, true, memory_order_release); /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

/* Real-device incident: a tap that landed while bt_init's own chip flash
 * was still genuinely in progress used to just be refused outright (see
 * BT_INIT_OK_FLAG_PATH's own comment for the actual chip-wedging risk that
 * guards against) -- functionally safe, but made the user re-tap
 * themselves once it settled, AND gave no visual feedback at all that
 * anything had registered (the drawer's own icon never flipped, unlike a
 * normal toggle). This queues the intent properly instead: waits for
 * BT_INIT_OK_FLAG_PATH, then waits for two real powered-off observations
 * before performing the exact same turn-on sequence bt_toggle_thread_func()'s
 * own turning-on path uses, automatically, no second tap needed. Waiting
 * for the settled-off state matters: bt_init_ok can become visible just
 * before the boot sequence's final disable propagates through bluetoothd,
 * and enabling in that gap lets the final disable erase the user's intent.
 * Deliberately
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
#define BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED 2

static void * bt_pending_enable_thread_func(void * arg) {
    (void) arg;
    uint32_t waited_ms = 0;
    unsigned int off_observations = 0;
    bool init_finished = false;
    while (waited_ms < BT_BOOT_ENABLE_MAX_WAIT_MS) {
        if (access(BT_INIT_OK_FLAG_PATH, F_OK) == 0) {
            init_finished = true;
            if (!bt_control_is_powered()) {
                off_observations++;
                if (off_observations >= BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED) {
                    if (bt_control_init_chip()) bt_control_enable();
                    break;
                }
            } else {
                off_observations = 0;
            }
        }
        usleep(BT_BOOT_ENABLE_POLL_INTERVAL_MS * 1000);
        waited_ms += BT_BOOT_ENABLE_POLL_INTERVAL_MS;
    }
    /* If initialization finished but its state never produced two clean
     * off samples before the bounded wait elapsed, assert the requested
     * final state once anyway. Never do this without bt_init_ok: that would
     * reintroduce the unsafe concurrent UART initialization race. */
    if (init_finished && off_observations < BT_BOOT_ENABLE_OFF_OBSERVATIONS_REQUIRED) {
        if (bt_control_init_chip()) bt_control_enable();
    }
    atomic_store_explicit(&bt_toggle_done_flag, true, memory_order_release); /* written last -- poll_bt_toggle only checks this flag */
    return NULL;
}

void quick_drawer_bt_event_cb(lv_event_t * e) {
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
    bt_boot_suppress_enabled = false;
    bt_boot_off_observations = 0;

    bt_toggle_active = true;
    atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);

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
    quick_drawer_mark_snapshot_dirty();

    /* Real-device bug report: the topbar Bluetooth icon stayed frozen on
     * whatever it showed pre-toggle (e.g. still the "connected" sprite
     * after manually turning Bluetooth off) until poll_bt_toggle()'s own
     * follow-up start_refresh_bt_icon() subprocess round trip finally
     * landed -- a real, user-visible delay, not just the ~10-13s cold-boot
     * chip-init case: even turning OFF (bt_control_disable() is D-Bus-only,
     * no chip op, so the toggle thread itself finishes fast) still waited
     * on that separate re-check. Flip it here too, same as the drawer icon
     * just above: OFF hides it outright (nothing to be connected to), ON
     * shows the disconnected sprite since a fresh toggle-on can't have an
     * active connection yet -- poll_refresh_bt_icon() overwrites both with
     * the real, settled state once its own check lands (it already skips
     * doing so while bt_toggle_active, see its own comment). */
    if (bt_will_be_powered) {
        lv_obj_remove_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(bt_status_icon, asset_path("topbar/bluetooth_unconnect.png"));
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN);
    }
    sync_topbar_status_icon_positions();

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
    if (!bt_toggle_active || !atomic_load_explicit(&bt_toggle_done_flag, memory_order_acquire)) return;
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
static atomic_bool bt_dac_startup_reapply_done_flag = false;

static void * bt_dac_startup_reapply_thread_func(void * arg) {
    (void) arg;
    bt_control_init_chip();
    bt_control_enable();
    bt_control_apply_output_settings(true, current_settings.bt_volume_sync_enabled);
    atomic_store_explicit(&bt_dac_startup_reapply_done_flag, true, memory_order_release); /* written last -- poll_bt_dac_startup_reapply only checks this flag */
    return NULL;
}

/* Called once from gui_init(), only if bt_dac_mode_enabled was already true
 * at load time (a fresh toggle-on tap already goes through
 * bt_dac_toggle_cb() directly and doesn't need this). */
static void start_bt_dac_startup_reapply_if_needed(void) {
    if (!current_settings.bt_dac_mode_enabled) return;
    bt_dac_startup_reapply_active = true;
    atomic_store_explicit(&bt_dac_startup_reapply_done_flag, false, memory_order_relaxed);
        if (pthread_create(&bt_dac_startup_reapply_thread, NULL, bt_dac_startup_reapply_thread_func, NULL) != 0) {
        bt_dac_startup_reapply_active = false;
    }
}


static void poll_bt_dac_startup_reapply(void) {
    if (!bt_dac_startup_reapply_active || !atomic_load_explicit(&bt_dac_startup_reapply_done_flag, memory_order_acquire)) return;
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
static atomic_bool bt_apply_output_settings_done_flag = false;

typedef struct {
    bool dac_mode_enabled;
    bool volume_sync_enabled;
} bt_apply_output_settings_request_t;

static void * bt_apply_output_settings_thread_func(void * arg) {
    bt_apply_output_settings_request_t * req = (bt_apply_output_settings_request_t *) arg;
    bt_control_apply_output_settings(req->dac_mode_enabled, req->volume_sync_enabled);
    free(req);
    atomic_store_explicit(&bt_apply_output_settings_done_flag, true, memory_order_release); /* written last -- poll_bt_apply_output_settings only checks this flag */
    return NULL;
}

/* Silently ignores overlap (another apply already in flight) rather than
 * queuing -- same "ignore taps until it lands" treatment as
 * quick_drawer_bt_event_cb()/quick_drawer_wifi_event_cb() use for their own
 * slow operations, and the current_settings values the caller already wrote
 * before calling this are what the eventually-scheduled apply would use
 * anyway once the in-flight one finishes and the screen is re-populated. */
void start_bt_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    if (bt_apply_output_settings_active) return;
    bt_apply_output_settings_request_t * req = malloc(sizeof(*req));
    req->dac_mode_enabled = dac_mode_enabled;
    req->volume_sync_enabled = volume_sync_enabled;
    atomic_store_explicit(&bt_apply_output_settings_done_flag, false, memory_order_relaxed);
    bt_apply_output_settings_active = true;
        if (pthread_create(&bt_apply_output_settings_thread, NULL, bt_apply_output_settings_thread_func, req) != 0) {
        bt_apply_output_settings_active = false;
        free(req);
    }
}


static void poll_bt_apply_output_settings(void) {
    if (!bt_apply_output_settings_active || !atomic_load_explicit(&bt_apply_output_settings_done_flag, memory_order_acquire)) return;
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
        backlight_set_normal_percent((int) percent); /* live feedback while dragging; also exits inactivity dim */
        lv_label_set_text_fmt(label, "%d%%", (int) percent);
        quick_drawer_mark_snapshot_dirty();
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
    lv_obj_set_style_text_font(quick_drawer_sleep_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
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
    lv_obj_set_style_text_font(quick_drawer_brightness_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(quick_drawer_brightness_label, LV_ALIGN_TOP_RIGHT, -20, QUICK_DRAWER_PANEL1_TOP + 177);

    /* Real-device bug report: the "NN%" label overlapped the slider --
     * first attempted by pushing the slider's own Y down to clear the
     * label's line height, but real-device feedback rejected that ("the
     * slider can't be lower than the icon, restore it to its default
     * position") -- brightness_icon (row 2's own visual anchor) sits at a
     * fixed Y, and the slider is meant to sit at a fixed, small offset
     * below it, not drift down with text size. The actual overlap was
     * horizontal, not vertical: the label is right-anchored and grows
     * LEFTWARD as its rendered text widens at bigger font tiers (BlindMF),
     * eventually reaching past the slider's fixed 300px-wide right edge.
     * Shrinking the slider's own WIDTH -- explicitly OK per that same
     * feedback ("it's ok to make the slider smaller") -- to always leave
     * room for the widest this label could ever render ("100%") fixes the
     * real, horizontal overlap while leaving both elements' Y positions
     * exactly as originally designed. */
    int32_t brightness_label_max_w = lv_text_get_width("100%", 4, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    int32_t brightness_track_w = (w - 20 - brightness_label_max_w - 20) - 90;
    if (brightness_track_w > 300) brightness_track_w = 300; /* never wider than the original design */
    if (brightness_track_w < 120) brightness_track_w = 120; /* sane floor so the track never collapses to nothing */

    quick_drawer_brightness_track = lv_slider_create(quick_drawer);
    lv_obj_set_size(quick_drawer_brightness_track, brightness_track_w, 12);
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
    lv_obj_set_style_bg_color(quick_drawer_brightness_track, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_bg_image_src(quick_drawer_brightness_track, asset_path("volume/cursor.png"), LV_PART_KNOB);
    /* Real-device bug report: accent color didn't apply here -- see
     * apply_accent_color()'s own comment on why an image-art slider needs
     * bg_image_recolor, not just bg_color. */
    lv_obj_add_style(quick_drawer_brightness_track, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(quick_drawer_brightness_track, &style_accent, LV_PART_KNOB);
    /* Real-device bug report: same left-edge gray sliver/root cause as
     * volume_popup_track's own fix -- see its comment. */
    configure_native_slider_rail(quick_drawer_brightness_track);
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

    /* Render the complex live control tree once while still under the boot
     * splash. Drag/snap motion uses this one opaque RGB565 image; controls
     * become live again the instant the motion settles. */
    quick_drawer_rebuild_snapshot();
}

void refresh_clock_label(void) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char buf[8];
    /* %I (12h) zero-pads to 2 digits just like %H (24h) does -- "01".."12",
     * never a single digit -- so buf is always "HH:MM" (5 chars) either
     * way, mapping 1:1 onto the 5 fixed slots with no leading-slot-hiding
     * needed here, unlike the volume/battery readouts. */
    strftime(buf, sizeof(buf), current_settings.clock_24h ? "%H:%M" : "%I:%M", &tm_info);

    for (int i = 0; i < 5; i++) {
        char asset[24];
        if (buf[i] == ':') {
            snprintf(asset, sizeof(asset), "topbar/colon.png");
        } else {
            snprintf(asset, sizeof(asset), "topbar/%c.png", buf[i]);
        }
        lv_image_set_src(clock_topbar_digit[i], asset_path(asset));
    }

    if (current_settings.clock_24h) {
        lv_obj_add_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(clock_topbar_ampm, asset_path(tm_info.tm_hour < 12 ? "topbar/am.png" : "topbar/pm.png"));
        lv_obj_remove_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    }
}

static void free_playlist(void) {
    for (int i = 0; i < playlist_count; i++) free(playlist[i]); /* free(NULL) (an unresolved lazy slot) is a safe no-op */
    free(playlist);
    playlist = NULL;
    playlist_count = 0;
    playlist_index = -1;
    free(playlist_lazy_sort_order);
    playlist_lazy_sort_order = NULL;
}

/* Defined further down, with the rest of the lazy-All-Songs-queue
 * machinery -- forward-declared here since every reader of playlist[]'s
 * actual STRING CONTENT
 * between here and there (favorite toggle, gapless preload, play_track_at_
 * from, ...) must resolve through it rather than indexing playlist[]
 * directly, or a lazy All-Songs queue's unresolved NULL slots would crash
 * them. See playlist_lazy_sort_order's own comment for the full picture. */

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

    /* A pending wrap-continuation order (see compute_auto_advance_index()'s
     * own comment) sized for whatever playlist_count was true when it was
     * generated is no longer valid once the playlist itself has changed --
     * without this, a stale pending_shuffle_order could later get promoted
     * by commit_auto_advance() and paired with the NEW (different)
     * playlist_count, a real out-of-bounds read the first time shuffle_pos
     * advances past the old, smaller array's real size. */
    free(pending_shuffle_order);
    pending_shuffle_order = NULL;

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
             * later confirms are guaranteed to be the same track.
             *
             * Audit finding: this used to regenerate pending_shuffle_order
             * unconditionally on every call, directly violating this
             * function's own documented contract just above ("must be safe
             * to call more than once for the same index and always get the
             * same answer") -- arm_next_track_for_audio() calls this
             * speculatively and can call it again for the same pending
             * transition (e.g. a crossfade/ReplayGain setting change
             * re-arming before the track actually finishes), and each call
             * was drawing a brand-new random order, silently discarding
             * whichever track audio.c had already been armed with. Only
             * generate once per wrap; commit_auto_advance() consumes and
             * NULLs this when the wrap is actually confirmed, so the next
             * genuinely new wrap still gets a fresh shuffle. */
            if (!pending_shuffle_order) {
                pending_shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
                memcpy(pending_shuffle_order, shuffle_order, sizeof(int) * (size_t) playlist_count);
                fisher_yates_shuffle(pending_shuffle_order, playlist_count);
            }
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
        remote_control_sync_queue(queued_pending_count > 0 ? (const char * const *) &playlist[playlist_index + 2] : NULL,
                                  queued_pending_count);
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
        remote_control_sync_queue(queued_pending_count > 0 ? (const char * const *) &playlist[index + 2] : NULL,
                                  queued_pending_count);
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
        if (folder_size > 0) {
            strncpy(folder_out, folder_name, folder_size - 1);
            folder_out[folder_size - 1] = '\0';
        }
    }
}

static void format_time(double seconds, char * buf, size_t buf_size) {
    if (seconds < 0) seconds = 0;
    int total = (int) seconds;
    snprintf(buf, buf_size, "%d:%02d", total / 60, total % 60);
}

void set_play_button_state(bool is_playing) {
    lv_image_set_src(play_btn, asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png"));
    player_transition_mark_dirty(); /* play_btn lives on player_screen -- see the cache's own doc comment */
    if (quick_drawer_play_btn) {
        lv_image_set_src(quick_drawer_play_btn,
                         asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png"));
        quick_drawer_mark_snapshot_dirty();
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

    const char * path = playlist_path_at(playlist_index);

    /* A remote track's synthetic "remote://" key has no real extension to
     * parse (see decoder_open()'s own comment) -- the plugin declares the
     * codec up front instead. */
    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    char ext[16] = "";
    if (is_remote_track && remote_meta.codec[0]) {
        size_t i = 0;
        for (const char * p = remote_meta.codec; *p && i < sizeof(ext) - 1; p++, i++) {
            ext[i] = (char) toupper((unsigned char) *p);
        }
        ext[i] = '\0';
    } else if (!is_remote_track) {
        const char * dot = strrchr(path, '.');
        if (dot) {
            size_t i = 0;
            for (const char * p = dot + 1; *p && i < sizeof(ext) - 1; p++, i++) {
                ext[i] = (char) toupper((unsigned char) *p);
            }
            ext[i] = '\0';
        }
    }

    unsigned int sample_rate = audio_get_sample_rate();
    if (sample_rate > 0) {
        lv_label_set_text_fmt(format_badge_label, "%s  %.1fkHz", ext, sample_rate / 1000.0);
    } else {
        lv_label_set_text(format_badge_label, ext);
    }
}


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
#define REFLECTION_BLUR_RADIUS 32
#define REFLECTION_BLUR_PASSES 5
/* Kept as an integer ratio (channel * NUM / DEN) rather than a float --
 * matches this codebase's general preference for integer arithmetic on the
 * embedded target, and there's no accuracy need here that would justify a
 * float. 1/4 reads as "mostly faded into black" without the panel going
 * fully flat. */
#define REFLECTION_DARKEN_NUM 1
#define REFLECTION_DARKEN_DEN 2

/* 1D box blur via a sliding running sum -- O(length) regardless of radius,
 * rather than O(length * radius) from re-summing the whole window at every
 * pixel. `stride` is measured in elements, not bytes, so the same function
 * serves both passes of the separable 2D blur generate_reflection() below
 * does: 1 for a horizontal pass along a contiguous row, `width` for a
 * vertical pass down a column. Edges are clamped (repeats the edge pixel)
 * rather than wrapping or zero-padding, so the blur doesn't darken/lighten
 * near the image boundary. */
void box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius) {
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

/* RGB565 has only 32 red/blue and 64 green levels, so a strong blur exposes
 * broad contour bands even though all filtering above is done in 8-bit
 * planes. Ordered 8x8 dithering trades those coherent bands for a tiny,
 * stable sub-pixel texture. Stable (not random) matters because a changing
 * noise field would shimmer on every redraw. */
uint16_t rgb888_to_565_dithered(int r, int g, int b, int x, int y) {
    static const uint8_t bayer8[8][8] = {
        { 0,48,12,60, 3,51,15,63 }, { 32,16,44,28,35,19,47,31 },
        { 8,56, 4,52,11,59, 7,55 }, { 40,24,36,20,43,27,39,23 },
        { 2,50,14,62, 1,49,13,61 }, { 34,18,46,30,33,17,45,29 },
        { 10,58, 6,54, 9,57, 5,53 }, { 42,26,38,22,41,25,37,21 }
    };
    int threshold = bayer8[y & 7][x & 7];
    r += threshold / 8 - 4;
    g += threshold / 16 - 2;
    b += threshold / 8 - 4;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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

    /* Center-crop the square cover to this wide panel. The previous bottom-
     * strip mirror preserved recognizable hard shapes even after blurring,
     * which looked like a smeared reflection rather than frosted glass. */
    for (int y = 0; y < h; y++) {
        int src_y = (COVER_ART_HEIGHT - h) / 2 + y;
        const uint16_t * src_row = (const uint16_t *) (cover_bytes + (size_t) src_y * COVER_ART_WIDTH * 2);
        for (int x = 0; x < w; x++) {
            uint16_t px = src_row[x];
            r[y * w + x] = (uint8_t) (((px >> 11) & 0x1F) * 255 / 31);
            g[y * w + x] = (uint8_t) (((px >> 5) & 0x3F) * 255 / 63);
            b[y * w + x] = (uint8_t) ((px & 0x1F) * 255 / 31);
        }
    }

    /* Separable box blur: one horizontal pass (per row) then one vertical
     * pass (per column), on each channel independently -- a full 2D blur
     * at a fraction of the cost of a real 2D kernel. Heavy (radius 16)
     * deliberately, per the "frosted glass" look this is going for rather
     * than a sharp mirror image. */
    for (int pass = 0; pass < REFLECTION_BLUR_PASSES; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(r + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(r, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(r + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
        memcpy(r, tmp, (size_t) w * h);
    }
    for (int pass = 0; pass < REFLECTION_BLUR_PASSES; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(g + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(g, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(g + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
        memcpy(g, tmp, (size_t) w * h);
    }
    for (int pass = 0; pass < REFLECTION_BLUR_PASSES; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(b + y * w, tmp + y * w, w, 1, REFLECTION_BLUR_RADIUS);
        memcpy(b, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(b + x, tmp + x, h, w, REFLECTION_BLUR_RADIUS);
        memcpy(b, tmp, (size_t) w * h);
    }

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
        out[i] = rgb888_to_565_dithered(rv, gv, bv, i % w, i / w);
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
    /* Local track whose directory should be searched when embedded art is
     * absent. The lookup and file read stay on this worker thread. Artist/
     * album come from the already-parsed track (or the DB), never from a
     * second metadata_read() -- that re-parse OOMs on huge ID3/APIC tags. */
    char local_track_path[PATH_MAX];
    char artist[128];
    char album[128];
    char album_artist[128];
} cover_decode_request_t;

static pthread_t cover_decode_thread;
static bool cover_decode_active = false;
static atomic_bool cover_decode_done_flag = false;

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

/* Compressed sidecar cap. cover_decode.c also rejects sources above
 * 1200x1200 before allocating RGB888, so a 4000px cover.jpg that still
 * fits in 4 MiB compressed cannot OOM the ~56 MiB target. Oversized art
 * falls back to the default image. */
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)

static void albumart_info_from_path_tags(const char * track_path, const char * artist, const char * album,
                                          const char * album_artist, albumart_info_t * info) {
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", track_path ? track_path : "");
    snprintf(info->artist, sizeof(info->artist), "%s", artist ? artist : "");
    snprintf(info->album, sizeof(info->album), "%s", album ? album : "");
    snprintf(info->albumartist, sizeof(info->albumartist), "%s", album_artist ? album_artist : "");
}

void albumart_info_from_song_row(const song_row_t * song, albumart_info_t * info) {
    albumart_info_from_path_tags(song->path, song->tags.artist, song->tags.album, song->tags.album_artist, info);
}

/* Rockbox albumart.c search: sized file first, then generic cover/folder
 * next to the track, then MUSIC_ROOT_DIR/.open_hiby_player/albumart/<artist>-<album>.
 * Tags are supplied by the caller (already-parsed track or DB row). This
 * must not open the audio file. */
static bool load_external_cover(const char * track_path, const char * artist, const char * album,
                                const char * album_artist, uint8_t ** out_data, uint32_t * out_size) {
    albumart_info_t info;
    albumart_info_from_path_tags(track_path, artist, album, album_artist, &info);
    char found[PATH_MAX];
    if (!albumart_find(&info, found, sizeof(found), COVER_ART_WIDTH, COVER_ART_HEIGHT)) return false;
    return albumart_load_file(found, out_data, out_size, EXTERNAL_COVER_MAX_BYTES);
}

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
    } else if (!req->picture_data && req->local_track_path[0]) {
        load_external_cover(req->local_track_path, req->artist, req->album, req->album_artist,
                            &req->picture_data, &req->picture_size);
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
    atomic_store_explicit(&cover_decode_done_flag, true, memory_order_release); /* written last -- poll_cover_decode() only checks this flag */
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
    if (!req) {
        /* Audit finding: this used to dereference req unconditionally --
         * on this RAM-constrained device, a malloc() this size (the
         * struct embeds a 1536-byte stream_url and a PATH_MAX local_track_
         * path) can genuinely fail under memory pressure, and this runs on
         * essentially every track change, not just a deliberate low-memory
         * test. Free the request's own owned picture_data (same ownership
         * contract every other caller relies on) and just skip this
         * decode -- cover art staying stale for one track is a far better
         * outcome than crashing the whole app. */
        free(r.picture_data);
        return;
    }
    *req = r;
    atomic_store_explicit(&cover_decode_done_flag, false, memory_order_relaxed);
    cover_decode_active = true;
    if (pthread_create(&cover_decode_thread, NULL, cover_decode_thread_func, req) != 0) {
        free(req->picture_data);
        free(req);
        cover_decode_active = false;
    }
}

static void launch_cover_decode(int for_index, uint8_t * picture_data, uint32_t picture_size) {
    cover_decode_request_t r = { .for_index = for_index, .picture_data = picture_data, .picture_size = picture_size,
                                  .stream_url = "", .stream_verify_tls = false };
    launch_cover_decode_req(r);
}

static void launch_cover_decode_from_track(int for_index, const char * track_path, const char * artist,
                                            const char * album, const char * album_artist) {
    cover_decode_request_t r = { .for_index = for_index, .picture_data = NULL, .picture_size = 0,
                                  .stream_url = "", .stream_verify_tls = false };
    snprintf(r.local_track_path, sizeof(r.local_track_path), "%s", track_path ? track_path : "");
    snprintf(r.artist, sizeof(r.artist), "%s", artist ? artist : "");
    snprintf(r.album, sizeof(r.album), "%s", album ? album : "");
    snprintf(r.album_artist, sizeof(r.album_artist), "%s", album_artist ? album_artist : "");
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
    if (!cover_decode_active || !atomic_load_explicit(&cover_decode_done_flag, memory_order_acquire)) return;
    cover_decode_active = false;
    pthread_join(cover_decode_thread, NULL);

    if (cover_decode_result_for_index != playlist_index) {
        free(cover_decode_result_pixels);
        free(cover_decode_result_reflection);
    } else if (!cover_decode_result_ok) {
        free(current_cover_bytes);
        current_cover_bytes = NULL;
        current_cover_for_index = -1;
        lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
        /* No in-memory raw bitmap to reflect for the static placeholder
         * cover -- reset the panel back to its plain background rather
         * than leaving a stale reflection from whatever track played
         * before this one. */
        free(current_reflection_bytes);
        current_reflection_bytes = NULL;
        lv_obj_set_style_bg_image_src(player_overlay_panel, asset_path("playing_plane/buttom.png"), 0);
        player_transition_mark_dirty(); /* cover_img just changed to the placeholder -- see the cache's own doc comment */
    } else {
        free(current_cover_bytes);
        current_cover_bytes = (uint8_t *) cover_decode_result_pixels;
        current_cover_for_index = cover_decode_result_for_index;
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

        /* Bug report: the lyrics screen's blurred backdrop could still show
         * the PREVIOUS track's art even after its own regenerate-pending
         * retry ran, because that retry read whatever current_cover_bytes
         * held at that moment -- and this decode (the one that actually
         * updates current_cover_bytes for the new track) can easily still
         * be in flight when the retry fires, since the two run on
         * independent async pipelines with no ordering between them. This
         * is the one place current_cover_bytes is ever updated for a new
         * track, so triggering the refresh from here (rather than from
         * lyrics_timer_cb()'s own earlier, opportunistic attempt) is the
         * only way to guarantee it always runs against the CORRECT art.
         * launch_lyrics_backdrop_decode() itself already no-ops if a
         * generation happens to already be running (marking it pending
         * instead, same as any other caller), so this is safe to call
         * unconditionally whenever the lyrics screen is open. */
        gui_lyrics_on_cover_changed(playlist_index);
        player_transition_mark_dirty(); /* cover_img/player_overlay_panel's reflection just changed -- see the cache's own doc comment */
    }

    cover_decode_result_pixels = NULL;
    cover_decode_result_reflection = NULL;

    if (cover_decode_pending_valid) {
        cover_decode_pending_valid = false;
        launch_cover_decode_req(cover_decode_pending); /* not launch_cover_decode() -- must carry stream_url too, see that field's own comment */
        /* Real-device incident: ownership of picture_data just passed into
         * launch_cover_decode_req() above (either into a freshly malloc'd
         * req, or back into this same cover_decode_pending if a decode was
         * somehow still active) -- but this struct still holds a copy of
         * that same pointer. Left as-is, a rapid next track change landing
         * before the handed-off decode finishes would call
         * launch_cover_decode_req() again, see cover_decode_active still
         * true, and free(cover_decode_pending.picture_data) a buffer the
         * in-flight decode thread already owns and will free itself --
         * double free, reproduced by rapidly pressing Next/Prev. Clearing
         * the pointer here (not inside launch_cover_decode_req(), which
         * can't tell "just consumed" apart from "still needs freeing" for
         * its own r argument) makes this copy stop looking like it owns
         * the buffer. */
        cover_decode_pending.picture_data = NULL;
        cover_decode_pending.picture_size = 0;
    }
}

/* ---- Lyrics: async .lrc load ------------------------------------------
 * Deliberately independent from the cover-decode pipeline above, despite
 * both running off the UI thread and both being triggered from the same
 * track-change call site (apply_track_metadata_to_ui() below): this job
 * only reads a small text sidecar (lyrics_load_sidecar(), bounded at
 * LYRICS_MAX_FILE_BYTES) rather than decoding/blurring image data, and
 * runs eagerly on every track change (not lazily like the backdrop below)
 * so lyrics are already parsed by the time the user taps the album art.
 * Same "background thread writes then sets a volatile done flag last, poll
 * function on the UI thread only reads after seeing it" contract as
 * cover_decode_done_flag above, and the same "at most one job in flight,
 * a track change arriving mid-load replaces the pending request rather
 * than queuing" shape as launch_cover_decode_req()'s cover_decode_pending. ---- */







/* ---- Lyrics: fullscreen blurred backdrop ------------------------------
 * Generated lazily -- only when the lyrics screen actually opens (see
 * open_lyrics_screen()) -- rather than eagerly per track change like cover
 * art/reflection above: most tracks' lyrics view is never opened, and this
 * needs nothing the async load above produces, just the already-decoded
 * current_cover_bytes. A private copy is taken up front (cover_copy below)
 * rather than reading current_cover_bytes directly from the background
 * thread, since poll_cover_decode() can free and replace that pointer out
 * from under a still-running backdrop job on a rapid track change. ---- */

/* Final brightness is baked in before RGB565 dithering. This used to be a
 * 3/4-bright backdrop followed by LVGL's 40%-black alpha overlay (net 45%),
 * but that second RGB565 blend re-quantized the already-dithered image and
 * brought visible bands back. 9/20 preserves the same net brightness in a
 * single quantization step. */












static void favorite_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    favorite_is_set = !favorite_is_set;
    metadata_db_song_favorite_set(playlist_path_at(playlist_index), favorite_is_set);
    lv_image_set_src(favorite_icon, asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
    if (quick_drawer_favorite_icon) {
        lv_image_set_src(quick_drawer_favorite_icon,
                         asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
        quick_drawer_mark_snapshot_dirty();
    }
}

void arm_next_track_for_audio(int index);

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
    if (quick_drawer_order_icon) {
        lv_image_set_src(quick_drawer_order_icon, asset_path(play_mode_icon_asset(mode)));
        quick_drawer_mark_snapshot_dirty();
    }

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

/* refresh_now_playing_indicators() is defined down with the compact-list
 * screens it updates (Artists/Albums/All Songs/group songs) -- forward-
 * declared here since apply_track_metadata_to_ui() below is the single
 * place a real "this track started playing" event is known to have
 * happened, for every playback source (local library, Group Songs, Files,
 * .m3u playlists). */

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

static void apply_track_metadata_to_ui(int index, track_metadata_t * out_meta) {
    /* Resolved once -- this is playlist_index's first real touch on every
     * track-start (play_track_at_from()/on_track_auto_advanced() both call
     * this immediately after setting playlist_index), so it's also where a
     * lazy All-Songs slot actually gets strdup'd. Every other playlist[index]
     * use below reuses this same resolved pointer rather than re-deriving
     * it (playlist_path_at() is idempotent/cheap on a re-call either way,
     * but there's no reason to). */
    const char * path = playlist_path_at(index);

    char title[128];
    char folder[128];
    get_display_names(path, title, sizeof(title), folder, sizeof(folder));

    bool is_subsonic_stream = subsonic_stream_meta && index < subsonic_stream_meta_count &&
                               strcmp(path, subsonic_stream_meta[index].url) == 0;
    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);

    if (is_remote_track) {
        /* No file to open for a remote track -- unlike the Subsonic branch
         * below, this also fixes the ReplayGain gap Subsonic has today
         * (that branch never calls metadata_read(), so has_replaygain stays
         * false): the plugin declares replaygain_db up front from its own
         * catalog metadata, fed into resolve_replaygain() the same as any
         * local track's tag would be. */
        memset(out_meta, 0, sizeof(*out_meta));
        if (remote_meta.title[0]) {
            snprintf(out_meta->title, sizeof(out_meta->title), "%s", remote_meta.title);
            out_meta->has_title = true;
        }
        if (remote_meta.artist[0]) {
            snprintf(out_meta->artist, sizeof(out_meta->artist), "%s", remote_meta.artist);
            out_meta->has_artist = true;
        }
        if (remote_meta.album[0]) {
            snprintf(out_meta->album, sizeof(out_meta->album), "%s", remote_meta.album);
            out_meta->has_album = true;
        }
        if (remote_meta.has_replaygain) {
            out_meta->has_replaygain = true;
            out_meta->replaygain_gain_db = remote_meta.replaygain_db;
        }
    } else if (is_subsonic_stream) {
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
        metadata_read(path, out_meta);
    }

    snprintf(now_playing_path, sizeof(now_playing_path), "%s", path);
    refresh_now_playing_indicators();

    const char * title_text = out_meta->has_title ? out_meta->title : title;
    const char * folder_text = out_meta->has_artist ? out_meta->artist : folder;

    lv_label_set_text(song_title_label, title_text);
    lv_label_set_text(song_folder_label, folder_text);
    if (quick_drawer_title_label) {
        lv_label_set_text(quick_drawer_title_label, title_text);
        lv_label_set_text(quick_drawer_artist_label, folder_text);
        quick_drawer_mark_snapshot_dirty();
    }
    refresh_format_badge();
    if (is_remote_track && remote_meta.artwork_url[0]) {
        launch_cover_decode_from_url(index, remote_meta.artwork_url, remote_meta.verify_tls);
    } else if (is_subsonic_stream && subsonic_stream_meta[index].cover_url[0]) {
        launch_cover_decode_from_url(index, subsonic_stream_meta[index].cover_url, subsonic_stream_meta[index].verify_tls);
    } else if (is_remote_track) {
        /* No artwork_url and nothing local to fall back to -- unlike the
         * plain "no embedded picture" case below, launch_cover_decode_
         * from_track() must not be tried: path is the synthetic remote://
         * key, not a real file. */
    } else if (out_meta->picture_data && out_meta->picture_size > 0) {
        launch_cover_decode(index, out_meta->picture_data, out_meta->picture_size); /* embedded art has priority; takes ownership */
    } else {
        free(out_meta->picture_data);
        out_meta->picture_data = NULL;
        launch_cover_decode_from_track(index, path, out_meta->artist, out_meta->album, out_meta->album_artist);
    }

    /* No local sidecar (or embedded tag) makes sense for a Subsonic stream
     * or remote-provider URL -- same reasoning cover art uses a server-
     * supplied cover URL instead of a local-file lookup for those. A
     * streamed track just never gets lyrics in this first release. */
    if (!is_subsonic_stream && !is_remote_track) {
        gui_lyrics_load_track(index, path);
    } else {
        gui_lyrics_load_track(index, NULL);
    }
    /* out_meta->lyrics (populated by the metadata_read() call above, if this
     * track has embedded lyrics) is never read here -- launch_lyrics_load()
     * just above does its own independent metadata_read() on a background
     * thread instead of sharing this one (see lyrics_load_thread_func()'s
     * own comment for why: this function runs synchronously on the UI
     * thread at every track change, and re-parsing tags a second time in
     * the background is cheaper than plumbing a malloc'd pointer through a
     * separate async load path that already does its own file I/O anyway).
     * Always free it here so every caller's stack-local track_metadata_t
     * doesn't leak it. */
    free(out_meta->lyrics);
    out_meta->lyrics = NULL;

    favorite_is_set = metadata_db_song_favorite_is_set(path);
    const char * favorite_icon_asset = favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png";
    lv_image_set_src(favorite_icon, asset_path(favorite_icon_asset));
    if (quick_drawer_favorite_icon) {
        lv_image_set_src(quick_drawer_favorite_icon, asset_path(favorite_icon_asset));
        quick_drawer_mark_snapshot_dirty();
    }

    /* Once per real "this track started playing" event -- apply_track_
     * metadata_to_ui() is called exactly here for both an explicit pick
     * (play_track_at_from) and a gapless auto-advance (on_track_
     * auto_advanced), never on a repeat UI refresh of the same still-
     * playing track, so this can't double-count. Backs the Most Played
     * auto-generated playlist (Music > Playlists). */
    metadata_db_song_play_count_increment(path);

    /* The real position/duration come from audio_get_*_seconds() once the
     * decoder's opened -- the timer picks that up within its next tick. */
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    displayed_progress_percent = 0;
    displayed_position_second = -1;
    displayed_duration_second = -1;
    lv_label_set_text(pos_label, "0:00");
    lv_label_set_text(dur_label, "0:00");

    lv_label_set_text_fmt(song_count_label, "%d/%d", index + 1, playlist_count);
    player_transition_mark_dirty(); /* title/artist/format badge/progress reset above all just changed player_screen's own content -- see the cache's own doc comment */
}

/* Resolves which of a track's own ReplayGain fields to actually hand to
 * audio.c, per Settings -> Playback -> ReplayGain's mode: Off (no gain at
 * all), Per Track (the default -- normalizes every track to the same
 * perceived loudness), or Per Album (preserves intentional relative
 * loudness differences between tracks on the same album). Falls back to
 * track gain when Per Album is selected but this particular file has no
 * album-level tag -- most taggers only write one or the other depending on
 * whether the rip was tagged as a whole album or track-at-a-time, and
 * silently playing unnormalized instead of falling back to whatever IS
 * available would be a worse outcome than just using track gain here. */
static void resolve_replaygain(const track_metadata_t * meta, bool * out_has_gain, double * out_gain_db,
                                bool * out_has_peak, double * out_peak) {
    int mode = current_settings.replaygain_mode;
    if (mode == 2 && meta->has_replaygain_album) {
        *out_has_gain = true;
        *out_gain_db = meta->replaygain_album_gain_db;
        /* Prefer the album's own peak for clipping protection, but fall
         * back to the track's peak when the file has album gain without
         * album peak (not every tagger writes both) -- audio.c's
         * replaygain_to_linear() treats has_peak=false as "no clamp at
         * all", so leaving this unconditionally false whenever album peak
         * is absent would silently drop clipping protection even though a
         * real, usable peak value (the track's own) was sitting right
         * there. Track peak describes this specific file, so it's a safe,
         * if slightly more conservative, stand-in for the whole album's. */
        if (meta->has_replaygain_album_peak) {
            *out_has_peak = true;
            *out_peak = meta->replaygain_album_peak;
        } else {
            *out_has_peak = meta->has_replaygain_peak;
            *out_peak = meta->replaygain_peak;
        }
        return;
    }
    bool use_track = mode != 0;
    *out_has_gain = use_track && meta->has_replaygain;
    *out_gain_db = meta->replaygain_gain_db;
    *out_has_peak = use_track && meta->has_replaygain_peak;
    *out_peak = meta->replaygain_peak;
}

/* Tells audio.c what comes after `index` (per the current play mode -- see
 * compute_auto_advance_index()) so its playback thread can gapless-handoff
 * or crossfade into it near `index`'s natural end without a GUI round-trip.
 * Must be re-called (from on_track_auto_advanced) every time the thread
 * advances on its own, or the chain of automatic transitions stops after
 * one hop. */
void arm_next_track_for_audio(int index) {
    int next_index = compute_auto_advance_index(index);
    if (next_index < 0) {
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
        return;
    }
    const char * next_path = playlist_path_at(next_index);
    remote_track_meta_t next_remote_meta;
    bool next_is_remote_track = remote_track_meta_copy_for_path(next_path, &next_remote_meta);
    bool has_gain, has_peak;
    double gain_db, peak;
    if (next_is_remote_track) {
        /* metadata_read() can't open a synthetic "remote://" path -- same
         * gap this fixes in apply_track_metadata_to_ui() for the CURRENT
         * track, needed again here for the gapless-prefetched NEXT one
         * (on_track_auto_advanced()'s own comment: audio.c applies whatever
         * gain was armed here, not anything recomputed at handoff time). */
        has_gain = next_remote_meta.has_replaygain;
        gain_db = next_remote_meta.replaygain_db;
        has_peak = false;
        peak = 0.0;
    } else {
        track_metadata_t next_meta;
        metadata_read(next_path, &next_meta);
        resolve_replaygain(&next_meta, &has_gain, &gain_db, &has_peak, &peak);
        free(next_meta.picture_data); /* only needed the gain/peak fields, not the art or lyrics */
        free(next_meta.lyrics);
    }
    audio_set_next_track(next_path, has_gain, gain_db, has_peak, peak);
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

    /* Kept in lockstep so any still-unresolved lazy slot after `pos` keeps
     * mapping to the right song once shifted -- see playlist_lazy_sort_
     * order's own comment. playlist[pos] is already resolved/owned (just
     * strdup'd above), so its own new slot here is never read; the value
     * doesn't matter. */
    if (playlist_lazy_sort_order) {
        int * grown_order = realloc(playlist_lazy_sort_order, sizeof(int) * (size_t) (playlist_count + 1));
        playlist_lazy_sort_order = grown_order;
        memmove(&playlist_lazy_sort_order[pos + 1], &playlist_lazy_sort_order[pos],
                sizeof(int) * (size_t) (playlist_count - pos));
        playlist_lazy_sort_order[pos] = -1;
    }
    playlist_count++;
    queued_pending_count++;
    queue_next_insert_index = pos + 1;

    lv_label_set_text_fmt(song_count_label, "%d/%d", playlist_index + 1, playlist_count);
    /* Re-arm gapless preload -- what comes right after playlist_index may
     * have just changed (a brand new queue, or this insert landing exactly
     * there). */
    arm_next_track_for_audio(playlist_index);
    remote_control_sync_queue((const char * const *) &playlist[playlist_index + 1], queued_pending_count);

    show_info_toast("Added to queue");
}

static void queue_remove_song_at_offset(int offset) {
    if (offset < 0 || offset >= queued_pending_count || playlist_index < 0) return;
    int pos = playlist_index + 1 + offset;
    free(playlist[pos]);
    memmove(&playlist[pos], &playlist[pos + 1], sizeof(char *) * (size_t) (playlist_count - pos - 1));
    if (playlist_lazy_sort_order) {
        memmove(&playlist_lazy_sort_order[pos], &playlist_lazy_sort_order[pos + 1],
                sizeof(int) * (size_t) (playlist_count - pos - 1));
    }
    playlist_count--;
    queued_pending_count--;
    queue_next_insert_index = queued_pending_count > 0 ? playlist_index + 1 + queued_pending_count : -1;
    lv_label_set_text_fmt(song_count_label, "%d/%d", playlist_index + 1, playlist_count);
    arm_next_track_for_audio(playlist_index);
    remote_control_sync_queue(queued_pending_count > 0 ? (const char * const *) &playlist[playlist_index + 1] : NULL,
                              queued_pending_count);
    show_info_toast("Removed from queue");
}

static void queue_clear_pending(void) {
    while (queued_pending_count > 0) queue_remove_song_at_offset(queued_pending_count - 1);
    show_info_toast("Queue cleared");
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
static void notify_plugin_track_started(const track_metadata_t * meta, const char * path) {
    snprintf(plugin_now_playing_title, sizeof(plugin_now_playing_title), "%s", meta->title);
    snprintf(plugin_now_playing_artist, sizeof(plugin_now_playing_artist), "%s", meta->artist);
    snprintf(plugin_now_playing_album, sizeof(plugin_now_playing_album), "%s", meta->album);
    plugin_now_playing_duration = audio_get_duration_seconds();
    plugin_now_playing_loaded = true;

    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    plugin_manager_notify_track_started(meta->title, meta->artist, meta->album, plugin_now_playing_duration,
                                         is_remote_track ? remote_meta.provider : "", is_remote_track ? remote_meta.track_id : "");
}

static void play_track_at_from(int index, double start_seconds) {
    if (index < 0 || index >= playlist_count) return;

    const char * block_reason = external_dac_block_reason();
    if (block_reason) {
        show_error_toast(block_reason);
        return;
    }

    cancel_pending_progress_seek();
    user_seeking = false;
    playlist_index = index;

    track_metadata_t meta;
    apply_track_metadata_to_ui(index, &meta); /* resolves this slot if it's a still-lazy All Songs entry */
    const char * path = playlist_path_at(index);
    bool has_gain, has_peak;
    double gain_db, peak;
    resolve_replaygain(&meta, &has_gain, &gain_db, &has_peak, &peak);
    audio_play_file_at(path, start_seconds, has_gain, gain_db, has_peak, peak);
    arm_next_track_for_audio(index);

#ifndef HOST_BUILD
    hiby_sys_server_report_metadata(meta.title, meta.artist, meta.album, meta.genre,
                                     (long) (audio_get_duration_seconds() * 1000.0));
    hiby_sys_server_report_position((long) (start_seconds * 1000.0));
#endif
    notify_plugin_track_started(&meta, path);

    set_play_button_state(true);
    /* Manual next/previous and a deferred startup resume can already be on
     * the player screen.  Do not stack a duplicate copy of the same screen
     * just because playback is being (re)started there. */
    if (lv_screen_active() != player_screen) nav_push(player_screen);

    /* A remote track's stream_url can expire or be single-use -- resuming
     * into it blind on next launch can't work the way resuming a local
     * file (or even a Subsonic stream, which at least re-derives its own
     * salted URL from a stable server+song id) can. Leave last_track
     * untouched rather than saving a synthetic key that build_saved_
     * resume_playlist() has no way to turn back into a playable URL --
     * same "don't build expiring-URL-aware resume in this pass" scope
     * call as everywhere else remote tracks touch existing machinery. */
    if (!remote_track_path_is_remote(path)) {
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", path);
        current_settings.last_position = start_seconds;
    }
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
    notify_plugin_track_started(&meta, playlist_path_at(index));

    set_play_button_state(true);

    /* See play_track_at_from()'s own comment on why a remote track's
     * synthetic key is never saved as last_track. */
    if (!remote_track_path_is_remote(playlist_path_at(index))) {
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", playlist_path_at(index));
        current_settings.last_position = 0.0;
    }
    settings_save(&current_settings);
}

static void play_track_at(int index) {
    play_track_at_from(index, 0.0);
}

void on_file_selected(char ** new_playlist, int count, int selected_index) {
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
    remote_control_sync_queue(NULL, 0);
    play_track_at(selected_index);
}

/* Same as on_file_selected() above, but for a tap that needs to start
 * partway into the track rather than at 0:00 -- currently only CUE track
 * playback (see cue_track_row_click_cb()): each of new_playlist's entries
 * is the SAME physical audio file repeated once per CUE track, and
 * start_seconds is the tapped track's own INDEX 01 offset within it. */
void on_file_selected_at(char ** new_playlist, int count, int selected_index, double start_seconds) {
    free_playlist();
    playlist = new_playlist;
    playlist_count = count;
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    remote_control_sync_queue(NULL, 0);
    play_track_at_from(selected_index, start_seconds);
}

/* set_player_source_group_songs() is defined further down, right after
 * group_songs_entries/count/title_label -- it needs those already in
 * scope. These three don't. */
void clear_player_source(void) {
    player_source_kind = PLAYER_SOURCE_NONE;
    player_source_all_songs_index = -1;
    player_source_recently_added_index = -1;
    free(player_source_group_title);
    player_source_group_title = NULL;
    free_group_song_entries(player_source_group_entries, player_source_group_count);
    player_source_group_entries = NULL;
    player_source_group_count = 0;
    player_source_group_pos = -1;
    player_source_file_browser_row = -1;
}

void set_player_source_all_songs(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_ALL_SONGS;
    player_source_all_songs_index = display_index;
    current_settings.last_source_kind = 1;
    current_settings.last_source_name[0] = '\0';
}

void set_player_source_recently_added(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_RECENTLY_ADDED;
    player_source_recently_added_index = display_index;
    /* No dedicated resume kind for this source -- falls back to "unknown",
     * same as Favorites/Most Played/user playlists (see group_song_row_
     * click_cb's own last_source_kind assignment); only All Songs and Album
     * get a real boot-resume slot (settings.h's own last_source_kind
     * comment). */
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
}


void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_GROUP_SONGS;
    copy_group_song_entries(&player_source_group_entries, entries, count);
    player_source_group_count = count;
    player_source_group_pos = selected_index;
    if (title) player_source_group_title = strdup(title);
    current_settings.last_source_kind = 2;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s", title ? title : "");
}

void set_player_source_file_browser(const char * dir, int row) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_FILE_BROWSER;
    snprintf(player_source_file_browser_dir, sizeof(player_source_file_browser_dir), "%s", dir);
    player_source_file_browser_row = row;
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
}

/* Wraps on_file_selected() as file_browser_init()'s select_cb, rather than
 * passing on_file_selected directly, so the source snapshot above only
 * ever gets set for an actual folder-browse tap -- on_file_selected()
 * itself is shared by every play-launch path (All Songs, group songs,
 * Subsonic downloads, DLNA casts, ...) and has no way to tell which of
 * them is calling it. file_browser_get_last_selected_dir()/_row() are
 * only valid synchronously within file_browser.c's own select_cb() call,
 * which is exactly where this reads them. */
void on_file_browser_selected(char ** new_playlist, int count, int selected_index) {
    set_player_source_file_browser(file_browser_get_last_selected_dir(), file_browser_get_last_selected_row());
    on_file_selected(new_playlist, count, selected_index);
}

static void toggle_play_pause(void) {
    if (playlist_index < 0) return; /* nothing loaded yet */
    if (deferred_resume_pending) {
        double start_seconds = deferred_resume_position;
        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        play_track_at_from(playlist_index, start_seconds);
        return;
    }
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

void crossfade_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.crossfade_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    audio_set_crossfade_enabled(current_settings.crossfade_enabled);
    settings_save(&current_settings);
    refresh_quick_drawer_crossfade_icon(); /* see its own comment -- keeps the drawer icon in sync */
}

void car_mode_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.car_mode_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    if (current_settings.car_mode_enabled) {
        show_info_toast("Car Mode powers the device off when it loses external power, and "
                         "automatically resumes playback once power is restored.");
    }
}

void swipe_up_home_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.swipe_up_home_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (current_settings.swipe_up_home_enabled) {
        lv_obj_remove_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN);
    }
    settings_save(&current_settings);
}

void screen_dimming_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_dimming_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (!current_settings.screen_dimming_enabled) {
        backlight_set_dimmed(false);
    }
    settings_save(&current_settings);
}

void hide_player_topbar_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.hide_player_topbar = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    sync_player_topbar_visibility(lv_screen_active());
    player_transition_mark_dirty(); /* topbar/back-button target-state visibility just changed -- see the cache's own doc comment */
}

void led_indicator_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.led_indicator_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    led_control_apply(current_settings.led_indicator_enabled);
    settings_save(&current_settings);
}

void charge_limiter_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.charge_limiter_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    settings_save(&current_settings);
}

void safe_charging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.safe_charging_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    safe_charging_poll(current_settings.safe_charging_enabled, true);
    settings_save(&current_settings);
}

/* refresh_battery_topbar() (defined earlier in this file, topbar setup near
 * gui_init()'s own layout code) already re-syncs the wifi/bt icon positions
 * itself whenever battery_topbar_group's hidden flag actually changes --
 * called here so toggling this switch is reflected immediately, without
 * waiting for the next battery poll tick. */
void battery_percent_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.show_battery_percent = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    refresh_battery_topbar();
}

/* refresh_clock_label() re-syncs the AM/PM sprite's own hidden flag itself
 * -- called here so toggling this switch is reflected immediately, same
 * reasoning as battery_percent_switch_event_cb() just above. */
void clock_24h_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.clock_24h = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    refresh_clock_label();
}

/* Log-scale mapping so the slider gives fine control at low frequencies
 * (where the ear is more sensitive to small Hz changes) instead of wasting
 * most of the slider's travel on the top octave. */

static void pending_progress_seek_timer_cb(lv_timer_t * timer) {
    (void) timer;
    pending_progress_seek_timer = NULL;
    audio_seek(pending_progress_seek_seconds);
}

static void progress_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        user_seeking = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        double duration = audio_get_duration_seconds();
        int32_t percent = lv_slider_get_value(slider);
        pending_progress_seek_seconds = duration * ((double) percent / 100.0);
        cancel_pending_progress_seek();
        pending_progress_seek_timer = lv_timer_create(pending_progress_seek_timer_cb, 150, NULL);
        lv_timer_set_repeat_count(pending_progress_seek_timer, 1);
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
static void poll_dlna_control(void);
/* Defined alongside library_scan_once()'s own background-thread wrapper,
 * much further down -- forward-declared here since subsonic_library_
 * download_thread_func() (defined well before that point) needs to trigger
 * a rescan once its own batch download finishes. */
void start_library_rescan(void);
void poll_wifi_scan(void);
void poll_wifi_connect(void);
void poll_wifi_connect_saved(void);
void poll_wifi_disconnect(void);
void poll_wifi_forget(void);
void poll_bt_scan(void);
void poll_bt_connect(void);
void poll_bt_forget(void);

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
static bool inactivity_dimmed = false;
static int visible_status_poll_tick_counter = 0;

/* Earliest possible inactivity age for the interactive UI. LVGL's display
 * inactivity timestamp can predate gui_init()'s splash -> Home transition
 * (and, on the target, lv_display_trigger_activity() alone has now been
 * observed not to reliably discard that startup age). Clamp the value used
 * by dimming/timeout to elapsed interactive time so startup can never spend
 * the user's timeout budget. After this age grows past LVGL's normally-reset
 * inactivity value the clamp becomes a no-op, preserving ordinary touch and
 * hardware-button timeout behavior. */
static uint32_t interactive_ui_start_tick = 0;
static bool interactive_ui_started = false;

void gui_reset_interactive_timeout_baseline(void) {
    interactive_ui_start_tick = lv_tick_get();
    interactive_ui_started = true;
    lv_display_trigger_activity(NULL);
}

/* The panel dominates power while lit. Dim well before the configured full
 * timeout, but keep enough time for reading and never override an explicit
 * user's screen-off duration. */
#define SCREEN_DIM_AFTER_MS 10000U
#define VISIBLE_STATUS_POLL_TICKS 4 /* 2 seconds at the 500 ms control tick */
static bool radios_suspended = false;
static bool wifi_was_on_before_suspend = false;
static bool bt_was_on_before_suspend = false;

/* Tracks whether audio was playing as of the last tick the screen was off,
 * so the radio-suspend/idle-shutdown clocks (both anchored on
 * screen_off_since_tick) can be restarted when playback actually stops --
 * see the reset logic where this is used, just below. */
static bool screen_off_playback_active = false;

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


static bool library_rescan_success_pending;
static bool download_active;
static bool album_thumbnail_active;
static atomic_bool album_thumb_gen_active;

static bool shutdown_background_work_active(void) {
    return library_rescan_active || library_rescan_success_pending || album_thumbnail_active ||
           atomic_load(&album_thumb_gen_active) || download_active || subsonic_library_download_active ||
           subsonic_connect_active || plugin_manager_has_background_work() || playlist_files_has_active_write();
}

/* Real-device bug report: waking from suspend needed two power-button
 * presses -- see the resume fixup below (right after power_suspend_now())
 * for the full mechanism. An earlier version of this comment attributed the
 * bug entirely to an unreset LVGL inactivity clock (now fixed by
 * resume_from_suspend_fixups()'s own lv_display_trigger_activity() call)
 * and treated this window as covering only a rare, secondary race -- real
 * diagnostic logging during a live repro instead found this window itself
 * to be the actual remaining cause. hw_buttons.c only sets its
 * short-tap-consumed flag on the button's RELEASE, not the initial press
 * that wakes the kernel (see its own handle_key_event(), value==0 branch) --
 * and a real capture measured 2350ms between this window being armed (right
 * as power_suspend_now() returns, at/near the press that woke the kernel)
 * and that same press's release finally being consumed, comfortably past
 * the old 1000ms budget. Once the window auto-expires "unused," that
 * release is read as a brand new deliberate toggle-off press instead of
 * being recognized as the wake press's own echo -- exactly the "press 1
 * wakes then immediately goes dark, press 2 actually wakes it" report.
 * Raised with real margin above that measured gap. Short grace window, not
 * a one-shot drain: the physical press that wakes the kernel is captured by
 * hw_buttons.c's own independent evdev reader thread, whose timing relative
 * to this (main) thread's own resume handling isn't guaranteed -- a drain
 * attempted too early could miss a flag that thread hadn't set yet. Any
 * power-button press consumed within this window of a resume is treated as
 * an echo of the wake press and silently discarded rather than toggling the
 * screen; a genuinely deliberate second press to go back to sleep right
 * after waking landing inside this now-longer window is a rarer, but
 * real, tradeoff accepted in exchange for the wake press itself no longer
 * routinely misfiring. */
#define RESUME_POWER_DRAIN_WINDOW_MS 3000
static uint32_t resumed_from_suspend_tick = 0;
static bool resumed_from_suspend_pending = false;
/* power_suspend_now() returns from inside update_timer_cb(), after that
 * callback already sampled screen_was_on/screen_on_now. Remember that
 * out-of-band wake so the next control tick still runs every ordinary
 * screen_just_woke refresh/status/radio-reset action. */
static bool force_screen_just_woke = false;

/* Keeps the three pieces of screen runtime state inseparable. Merely
 * enabling an indev does not restart its paused LVGL read timer, and merely
 * lighting the panel does not restart LVGL's paused refresh timer. Used by
 * both the ordinary on/off edge below and the special suspend-return path,
 * which changes backlight state too late in the current timer callback for
 * that ordinary edge detector to observe it. */
static void apply_screen_runtime_state(bool screen_on) {
    lv_indev_enable(NULL, screen_on);
    audio_set_low_power_mode(!screen_on);

    lv_timer_t * refr_timer = lv_display_get_refr_timer(lv_display_get_default());
    if (refr_timer) {
        if (screen_on) lv_timer_resume(refr_timer);
        else lv_timer_pause(refr_timer);
    }

    int indev_timer_count = 0;
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_timer_t * read_timer = lv_indev_get_read_timer(indev);
        if (!read_timer) continue;
        if (screen_on) lv_timer_resume(read_timer);
        else lv_timer_pause(read_timer);
        indev_timer_count++;
    }

    if (screen_on) lv_async_call(full_redraw_async_cb, NULL);
#ifdef UI_PERF_TRACE
    printf("PERF screen_runtime screen_on=%d refr_timer=%d indev_timers=%d\n",
           screen_on, refr_timer != NULL, indev_timer_count);
#endif
}

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
    apply_screen_runtime_state(true);
    force_screen_just_woke = true;
    lv_display_trigger_activity(NULL);
    resumed_from_suspend_tick = lv_tick_get();
    resumed_from_suspend_pending = true;
    DBG_LOG("resume: grace window armed at tick=%u\n", resumed_from_suspend_tick);
}

/* Defined with the rest of the All Songs screen, much further down --
 * forward-declared here since update_timer_cb() (just below) needs it for
 * the remote-control play-by-index consumer. */
/* Defined with the rest of the remote-control scoped-play machinery, much
 * further down -- forward-declared here since update_timer_cb() (just
 * below) needs it for the remote-control play-by-index consumer. */

/* Defined with the rest of the power-off countdown popup, much further
 * down -- forward-declared here since update_timer_cb() (just below) needs
 * them for the power-button long-press consumer. */

/* Defined alongside the rest of live search's async DB query, much further
 * down -- forward-declared here since update_timer_cb() (just below) polls
 * it every tick, same as poll_cover_decode()/poll_lyrics_load(). */

/* Settings -> Playback -> Play/Pause Button (current_settings.
 * play_pause_button_mode). Mode 2 needs to tell a single press apart from
 * the first half of a double-click, so it defers the decision behind a
 * short timer -- same reset-then-resume idiom as TEXT_ENTRY_MULTITAP_MS
 * further down in this file -- rather than acting immediately; modes 0 and
 * 1 fire with zero added latency since there's nothing to disambiguate.
 * Real-device incident: update_timer_cb only polls hw_buttons every 500ms
 * (see its own lv_timer_create() call), so a click that straddles two poll
 * windows can show up here up to ~500ms later than the physical press that
 * caused it -- on top of the real gap between the two clicks themselves.
 * hw_buttons_consume_play_pause() returning a count instead of a bool
 * (see hw_buttons.c) already makes a double-click landing inside a single
 * poll window resolve immediately with no timer involved at all; this
 * window only needs to cover the slower, poll-straddling case, hence the
 * headroom above the 500ms poll period itself. */
#define PLAY_PAUSE_DOUBLE_CLICK_MS 700
static lv_timer_t * play_pause_click_timer = NULL;
static int play_pause_click_count = 0;

static void physical_skip_prev_track(void) {
    int prev_index = compute_manual_step_index(playlist_index, -1);
    if (prev_index >= 0) play_track_at(prev_index);
}

static void play_pause_click_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    lv_timer_pause(play_pause_click_timer);
    play_pause_click_count = 0;
    toggle_play_pause();
}

static void handle_physical_play_pause_press(void) {
    int mode = current_settings.play_pause_button_mode;
    if (mode == 1) {
        physical_skip_prev_track();
        return;
    }
    if (mode == 2) {
        if (!play_pause_click_timer) {
            play_pause_click_timer = lv_timer_create(play_pause_click_timeout_cb, PLAY_PAUSE_DOUBLE_CLICK_MS, NULL);
            lv_timer_pause(play_pause_click_timer);
        }
        play_pause_click_count++;
        if (play_pause_click_count >= 2) {
            lv_timer_pause(play_pause_click_timer);
            play_pause_click_count = 0;
            physical_skip_prev_track();
        } else {
            lv_timer_reset(play_pause_click_timer);
            lv_timer_resume(play_pause_click_timer);
        }
        return;
    }
    toggle_play_pause();
}

static void update_timer_cb(lv_timer_t * timer) {
    (void) timer;

    /* Physical volume/skip/play-pause buttons: applied here, on the one
     * thread allowed to touch LVGL widgets (see hw_buttons.h). Play/pause
     * is a count, not a bool -- this poll only runs every 500ms, and a real
     * double-click's two presses routinely land inside one poll window; a
     * count of 2 here dispatches immediately, twice in a row, letting
     * handle_physical_play_pause_press()'s own click-count state (mode 2)
     * see it as a same-tick double-click without waiting on its timer. */
    int played_paused_count = hw_buttons_consume_play_pause();
    for (int i = 0; i < played_paused_count; i++) {
        handle_physical_play_pause_press();
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
    int64_t remote_queue_id;
    if (remote_control_consume_queue_index(&remote_queue_id)) {
        song_row_t remote_queue_row;
        if (metadata_db_get_song_by_id(remote_queue_id, &remote_queue_row)) queue_add_song(remote_queue_row.path);
    }
    int remote_queue_remove_offset;
    if (remote_control_consume_queue_remove(&remote_queue_remove_offset))
        queue_remove_song_at_offset(remote_queue_remove_offset);
    if (remote_control_consume_queue_clear()) queue_clear_pending();
    int64_t remote_play_id;
    char remote_play_playlist[128], remote_play_artist[128], remote_play_album_artist[128], remote_play_album[128];
    if (remote_control_consume_play_index(&remote_play_id, remote_play_playlist, sizeof(remote_play_playlist),
                                           remote_play_artist, sizeof(remote_play_artist), remote_play_album_artist,
                                           sizeof(remote_play_album_artist), remote_play_album,
                                           sizeof(remote_play_album))) {
        /* remote_play_id is a song id (metadata_db.c's rowid-based
         * song_row_t.id) -- resolve it to a path, then hand that straight to
         * play_remote_control_song() (defined with the rest of the
         * remote-control scoped-play machinery, much further down), which
         * resolves the path plus the playlist/artist/album context into the
         * right playlist and position via its own DB queries, falling back
         * to the whole library (All Songs, by title offset) when no scope
         * applies. */
        song_row_t remote_play_row;
        if (metadata_db_get_song_by_id(remote_play_id, &remote_play_row)) {
            play_remote_control_song(remote_play_row.path, remote_play_playlist, remote_play_artist,
                                      remote_play_album_artist, remote_play_album);
        }
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
        static bool car_shutdown_pending = false;
        bool charging = battery_is_charging();
        if (current_settings.car_mode_enabled && last_charging && !charging &&
            (audio_is_playing() || audio_is_paused())) car_shutdown_pending = true;
        if (car_shutdown_pending && !shutdown_background_work_active()) {
            current_settings.last_position = audio_get_position_seconds();
            settings_save(&current_settings);
            idle_shutdown_now(); /* full poweroff -- does not return, see idle_shutdown.h */
        }
        if (charging || !current_settings.car_mode_enabled) car_shutdown_pending = false;
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
    if (played_paused_count > 0 || skipped_next || skipped_prev || volume_delta != 0) {
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
    if (interactive_ui_started) {
        uint32_t interactive_age_ms = lv_tick_elaps(interactive_ui_start_tick);
        if (screen_inactive_ms > interactive_age_ms) screen_inactive_ms = interactive_age_ms;
    }
    bool screen_on_before_timeout = backlight_screen_is_on();
    /* Reading lyrics while a track is actually playing is real, continuous
     * screen use with little or no touch activity to keep resetting LVGL's
     * own indev-driven inactivity clock (screen_inactive_ms above) --
     * exempt this screen from both dimming and the full auto-timeout
     * entirely, the same lv_screen_active()-gated exclusion shape already
     * used for bt_dac_overlay_screen/usb_dac_overlay_screen elsewhere in
     * this file (e.g. poll_quick_drawer_drag()'s own gesture exclusions),
     * rather than trying to synthesize fake touch activity to fool the
     * shared clock. Paused, though, is no different from sitting on any
     * other screen not actively being read/watched -- real-device feedback
     * was explicit that timeout/dim/suspend should behave normally then,
     * not stay suppressed just because the lyrics view happens to still be
     * open. */
    bool lyrics_screen_active = lv_screen_active() == gui_lyrics_get_screen() && audio_is_playing();
    if (current_settings.screen_dimming_enabled && screen_on_before_timeout &&
        !inactivity_dimmed && !lyrics_screen_active && screen_inactive_ms >= SCREEN_DIM_AFTER_MS) {
        backlight_set_dimmed(true);
        inactivity_dimmed = true;
    } else if (screen_on_before_timeout && inactivity_dimmed &&
               (!current_settings.screen_dimming_enabled || lyrics_screen_active || screen_inactive_ms < SCREEN_DIM_AFTER_MS)) {
        backlight_set_dimmed(false);
        inactivity_dimmed = false;
    }
    if (current_settings.screen_timeout_enabled && backlight_screen_is_on() &&
        !lyrics_screen_active &&
        screen_inactive_ms >= (uint32_t) current_settings.screen_timeout_seconds * 1000) {
        backlight_set_screen_on(false);
        inactivity_dimmed = false;
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
    bool screen_just_woke = screen_on_now && (!screen_was_on || force_screen_just_woke);
    if (screen_just_woke) force_screen_just_woke = false;

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
        apply_screen_runtime_state(screen_on_now);

        /* NEXT_TODO_IMPLEMENTATION_PROMPT.md Task 1 -- pause LVGL's own
         * display-refresh timer while the backlight is off: nothing on
         * screen can be seen, so there is no reason to keep re-rendering
         * (or even re-checking for invalidated areas) at anywhere near its
         * normal cadence. update_timer_cb() (this very function) is NOT
         * this timer -- it's registered completely separately in
         * gui_init() and keeps running every 500ms regardless of this,
         * which is what still services hardware buttons, charging,
         * hotplug, and async-job completion while dark (see this
         * function's own body for the full list). Resumed the moment the
         * screen comes back on, together with invalidating the active
         * screen and both persistent layers so the very first visible
         * frame after waking is already fully correct (current track/
         * position, battery/charge, Wi-Fi/BT, volume, album art, topbar
         * visibility) rather than showing whatever was on screen right
         * before it went dark. The invalidation itself is deferred one
         * async tick (full_redraw_async_cb, shared with slide_transition_
         * anim_x_cb()'s own compositor-failure recovery -- see its own
         * comment), not called inline here -- this function is itself
         * already running from inside the SAME lv_timer_handler() pass
         * that would need to service that redraw, and forcing a
         * synchronous re-entrant refresh from inside a timer callback is
         * exactly the kind of reentrancy this codebase has already hit
         * real bugs from elsewhere -- lv_async_call() runs within this
         * same lv_timer_handler() invocation, just after all timers
         * finish, which is still effectively immediate. */
        /* Real-device finding: pausing the display-refresh timer alone did
         * NOT reduce main-loop wakeup frequency at all -- confirmed via
         * UI_PERF_TRACE, the loop kept waking ~65 times/second with the
         * screen off, just doing far less work per wakeup (avg handler time
         * dropped from ~250-500us to ~15-45us, but the WAKEUP ITSELF still
         * costs power regardless of how little work it does once awake).
         * Root cause: lv_indev_enable(NULL, false) above only suppresses
         * event dispatch -- each registered indev (the touchscreen here)
         * has its OWN separate periodic read timer (lv_indev_get_read_
         * timer(), created internally by LVGL alongside the indev itself)
         * that keeps polling on its own default ~16-33ms period regardless
         * of whether the indev is enabled. Pausing that too, for every
         * registered indev, is what actually lets the idle-cap change in
         * main.c matter. */
    }

    if (screen_just_woke) {
        inactivity_dimmed = false;
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
                atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
                wifi_toggle_target_enabled = true;
                if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
                    wifi_toggle_active = false;
            }
            if (bt_was_on_before_suspend && !bt_is_powered_cached && !bt_toggle_active) {
                bt_toggle_active = true;
                atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
                    if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, NULL) != 0) {
        bt_toggle_active = false;
    }
            }
            radios_suspended = false;
        }
    } else if (!screen_on_now) {
        bool playing_now_for_idle = audio_is_playing();
        if (screen_was_on) {
            /* Just went to sleep this tick -- the 10-minute countdown starts
             * from here, not from whenever the inactivity that caused it
             * began. */
            screen_off_since_tick = lv_tick_get();
        } else if (screen_off_playback_active && !playing_now_for_idle) {
            /* Real-device bug report: playback that continued past the
             * screen going dark (e.g. a long album) reaching the end of the
             * queue could suspend/poweroff the device on the very next tick
             * instead of waiting a full RADIO_SUSPEND_DELAY_MS/
             * idle_shutdown_minutes window -- because both clocks below are
             * measured from screen_off_since_tick (when the SCREEN went
             * off), which had already elapsed past the threshold while
             * audio_is_playing() was gating them off. Restart the clock the
             * moment playback actually stops, so the device only sleeps
             * after being genuinely idle (screen off AND silent) for the
             * configured duration, not merely screen-off. */
            screen_off_since_tick = lv_tick_get();
        }
        screen_off_playback_active = playing_now_for_idle;
        /* bt_is_powered_cached, not bt_control_is_powered(), deliberately --
         * the latter forks a process, and this condition is checked every
         * tick while the screen is off, not throttled like refresh_bt_icon()
         * is. The cached value is frozen at whatever it was when the screen
         * went dark, which is fine: nothing in this app can change BT power
         * state without a UI the user can't reach with the screen off. */
        if (!radios_suspended && !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            !audio_is_playing() && !battery_is_charging() && !shutdown_background_work_active() &&
            lv_tick_elaps(screen_off_since_tick) >= RADIO_SUSPEND_DELAY_MS) {
            wifi_was_on_before_suspend = wifi_control_is_enabled();
            bt_was_on_before_suspend = bt_is_powered_cached;
            if (wifi_was_on_before_suspend && !wifi_toggle_active) {
                wifi_toggle_active = true;
                atomic_store_explicit(&wifi_toggle_done_flag, false, memory_order_relaxed);
                wifi_toggle_target_enabled = false;
                if (pthread_create(&wifi_toggle_thread, NULL, wifi_toggle_thread_func, NULL) != 0)
                    wifi_toggle_active = false;
            }
            if (bt_was_on_before_suspend && !bt_toggle_active) {
                bt_toggle_active = true;
                atomic_store_explicit(&bt_toggle_done_flag, false, memory_order_relaxed);
                    if (pthread_create(&bt_toggle_thread, NULL, bt_toggle_thread_func, NULL) != 0) {
        bt_toggle_active = false;
    }
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
            !audio_is_playing() && !battery_is_charging() && !shutdown_background_work_active() &&
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
                 * Also folds in one of two fixes for a related real-device
                 * report -- waking from suspend needed two power-button
                 * presses, the first visibly flashing the backlight on then
                 * straight back off. First cause: lv_display_get_inactive_time(NULL),
                 * read by the auto-screen-timeout check just below, is fed
                 * only by lv_display_trigger_activity() calls, none of which
                 * happen anywhere during the entire suspend duration (the
                 * whole app, including this timer, is frozen) -- so the
                 * instant the screen is force-enabled, that check could see
                 * an inactivity duration spanning the sleep and immediately
                 * flip the screen back off again on this same tick.
                 * Resetting it explicitly here (lv_display_trigger_activity()
                 * below) fixes that half. A second, independent cause
                 * remained even with that fix: RESUME_POWER_DRAIN_WINDOW_MS's
                 * own grace window (see its comment, right above
                 * resume_from_suspend_fixups()) was too short relative to
                 * how late the wake press's own RELEASE (not its down edge)
                 * gets consumed -- confirmed by real diagnostic logging
                 * during a live repro, not by inspection alone.
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
        /* Hardware/sysfs status does not need the 500 ms latency reserved for
         * buttons and playback events. Poll it every two seconds and force a
         * refresh on wake; this removes three quarters of those reads. */
        if (screen_just_woke || ++visible_status_poll_tick_counter >= VISIBLE_STATUS_POLL_TICKS) {
            visible_status_poll_tick_counter = 0;
            refresh_clock_label();
            refresh_battery_topbar();
            refresh_headphone_icon();
            poll_usb_audio_output();
        }
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
    safe_charging_poll(current_settings.safe_charging_enabled, false);
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
            (playlist_count > 0 && playlist_index >= 0 && playlist_index < playlist_count)
                ? playlist_path_at(playlist_index)
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
    poll_subsonic_browse();
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
    plugin_manager_poll();
    poll_bt_scan();
    poll_bt_connect();
    poll_bt_forget();
    poll_library_rescan();
    poll_sd_format();
    poll_import_web_stop();
    poll_sleep_timer();
    poll_usb_mode_switch();
    poll_usb_storage_hotplug();
    poll_sd_card_hotplug();
    poll_cover_decode();
    gui_lyrics_poll_load();
    gui_lyrics_poll_backdrop();
    poll_search_job();

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

    /* All correctness-critical work above (buttons, queue transitions and
     * completion of requested background operations) still runs at 500 ms.
     * Everything below only redraws the player screen, so skip it while the
     * panel is physically off. This preserves hardware-button latency while
     * eliminating invisible slider/label rendering and asset I/O. */
    if (!backlight_screen_is_on()) return;

    if (playlist_index < 0 || user_seeking) return;

    double position = audio_get_position_seconds();
    double duration = audio_get_duration_seconds();

    if (duration > 0) {
        int32_t percent = (int32_t) ((position / duration) * 100.0);
        if (percent != displayed_progress_percent) {
            displayed_progress_percent = percent;
            lv_slider_set_value(progress_slider, percent, LV_ANIM_OFF);
        }
    }

    int position_second = (int) position;
    int duration_second = (int) duration;
    if (position_second != displayed_position_second) {
        char pos_str[16];
        displayed_position_second = position_second;
        format_time(position, pos_str, sizeof(pos_str));
        lv_label_set_text(pos_label, pos_str);
    }
    if (duration_second != displayed_duration_second) {
        char dur_str[16];
        displayed_duration_second = duration_second;
        format_time(duration, dur_str, sizeof(dur_str));
        lv_label_set_text(dur_label, dur_str);
    }

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

/* add_to_playlist moved to gui_library.c */

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
        row_label_enable_marquee(row);
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

void open_queue_screen(void) {
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
    lv_obj_set_height(label, font ? lv_font_get_line_height(font) : 24);
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
            configure_scrolling_row_label(row, row_w - 2 * LIST_ROW_LABEL_INSET);
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
    for (int i = 0; i < count; i++) new_playlist[i] = strdup(paths[i]);
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
void plugin_text_entry_done_cb(const char * text, void * user_data) {
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

    char * to_delete = strdup(playlist_path_at(playlist_index));
    int del_index = playlist_index;

    /* What to play next is decided BEFORE touching the file or the array --
     * whatever ends up sitting at del_index once the deleted entry is
     * removed (i.e. what used to be right after it), or the new last track
     * if the deleted one was last. Doesn't try to honor Shuffle/Repeat here
     * -- a delete is a one-off structural change to the queue, not a
     * "what's next" playback decision. */
    free(playlist[del_index]);
    for (int i = del_index; i < playlist_count - 1; i++) playlist[i] = playlist[i + 1];
    /* Kept in lockstep -- see playlist_lazy_sort_order's own comment. */
    if (playlist_lazy_sort_order) {
        for (int i = del_index; i < playlist_count - 1; i++) playlist_lazy_sort_order[i] = playlist_lazy_sort_order[i + 1];
    }
    playlist_count--;

    if (playlist_count == 0) {
        audio_stop();
        plugin_manager_notify_stopped();
        free(playlist);
        playlist = NULL;
        playlist_index = -1;
        free(playlist_lazy_sort_order);
        playlist_lazy_sort_order = NULL;
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

/* Defined much further down, alongside build_import_rescan_popup() (the
 * shared "are you sure?" 2-button popup shape's own doc comment) --
 * forward-declared here since this and every other popup builder before
 * that point in the file needs it. */
lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode,
                                       lv_obj_t ** out_title, const char * body_text, const char * confirm_text,
                                       lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row,
                                       const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                                       lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);

/* Defined alongside build_confirm_popup() above -- see its own doc comment
 * for why this is a separate shared shape (an N-row menu, not a yes/no
 * confirmation) and why both are forward-declared here. */
/* menu_popup_row_t and build_menu_popup declared in gui.h */

static void build_delete_song_popup(void) {
    /* LV_LABEL_LONG_DOT, not WRAP -- this title's text is set later
     * (delete_song_confirm_prompt() below) to "Delete <filename>?..." with
     * an arbitrary-length real filename spliced in, so it needs to
     * truncate rather than potentially wrap across several lines. */
    delete_song_popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &delete_song_popup_title, NULL, "Delete",
                                             lv_color_make(255, 120, 120), delete_song_confirm_cb, NULL, "Cancel",
                                             accent_lv_color(), delete_song_cancel_cb, NULL,
                                             delete_song_popup_backdrop_cb, &delete_song_popup_backdrop);
}

/* ---- The "more" 3-row menu itself ---- */
static lv_obj_t * more_menu_popup;
static lv_obj_t * more_menu_popup_backdrop;

void hide_more_menu_popup(void) {
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

static void more_menu_add_to_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (playlist_index < 0) return;
    open_add_to_playlist_for(playlist_path_at(playlist_index));
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

    lv_label_set_text_fmt(delete_song_popup_title, "Delete %s?\nThis cannot be undone.", basename_of(playlist_path_at(playlist_index)));
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
    static const menu_popup_row_t rows[] = {
        { "List", more_menu_list_cb, false },
        { "Queue", more_menu_queue_cb, false },
        { "Add to Playlist", more_menu_add_to_playlist_cb, false },
        { "EQ", more_menu_eq_cb, false },
        { "Delete", more_menu_delete_cb, true },
    };
    more_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])), more_menu_popup_backdrop_cb,
                                        &more_menu_popup_backdrop);
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
static char song_context_menu_target_path[600] = ""; /* 600, matching song_row_t.path's own bound (metadata_db.h) */

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
void open_song_context_menu(const char * path) {
    snprintf(song_context_menu_target_path, sizeof(song_context_menu_target_path), "%s", path);
    lv_obj_remove_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(song_context_menu_popup_backdrop);
    lv_obj_move_foreground(song_context_menu_popup);
}

static void build_song_context_menu_popup(void) {
    static const menu_popup_row_t rows[] = {
        { "Add to Queue", song_context_menu_add_to_queue_cb, false },
        { "Add to Playlist", song_context_menu_add_to_playlist_cb, false },
        { "Cancel", song_context_menu_cancel_cb, false },
    };
    song_context_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                                song_context_menu_popup_backdrop_cb,
                                                &song_context_menu_popup_backdrop);
}


static void cover_img_tap_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_lyrics_open_screen();
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
    lv_obj_add_flag(cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cover_img, cover_img_tap_cb, LV_EVENT_CLICKED, NULL);

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
    player_dismiss_btn = dismiss_btn;
    lv_obj_set_size(dismiss_btn, 64, 64);
    lv_obj_align(dismiss_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(dismiss_btn, 0, 0);
    lv_obj_set_style_border_width(dismiss_btn, 0, 0);
    lv_obj_remove_flag(dismiss_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dismiss_btn, LV_OBJ_FLAG_CLICKABLE);
    /* Visibility toggled dynamically by sync_player_topbar_visibility() per
     * Settings > Display > "Hide Player/Lyrics Top Bar" -- visible here by
     * default (its normal, initial-build state) whenever the setting is
     * off. See build_flattened_transition_frame() for how the Phase 2
     * transition cache captures this button's TARGET-state visibility
     * correctly even while Player is inactive, without relying on this
     * live object's own current flag value. */
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
    /* Real-device bug report: a long song title just grew title_row's flex
     * child past the favorite icon instead of stopping at it -- unlike
     * row_label_enable_marquee()'s usual callers (list rows), this label had
     * no bounded width for LVGL's circular long mode to scroll within, so it
     * rendered at its full unclipped text width and overlapped the icon
     * next to it. flex_grow gives it exactly the row's remaining width
     * (title_row's width minus the icon), same as any other flex-grow
     * child, which is all LV_LABEL_LONG_SCROLL_CIRCULAR needs to know it
     * overflows and should marquee -- same shared style/2s pause as every
     * other scrolling row label in the app (row_marquee_anim). */
    lv_obj_set_flex_grow(song_title_label, 1);
    row_label_enable_marquee(song_title_label);

    favorite_icon = lv_image_create(title_row);
    lv_image_set_src(favorite_icon, asset_path("playing_plane/collect_out.png"));
    lv_obj_add_flag(favorite_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(favorite_icon, favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_style(favorite_icon, &icon_press_style, LV_STATE_PRESSED); /* see icon_press_style's own comment */

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
    const lv_image_dsc_t * progress_bg_mem = asset_png_memory("playing_plane/progress_bg.png");
    const lv_image_dsc_t * progress_mem = asset_png_memory("playing_plane/progress.png");
    const lv_image_dsc_t * cursor_mem = asset_png_memory("playing_plane/cursor.png");
    lv_obj_set_style_bg_image_src(progress_slider, progress_bg_mem ? (const void *) progress_bg_mem : asset_path("playing_plane/progress_bg.png"), LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(progress_slider, progress_mem ? (const void *) progress_mem : asset_path("playing_plane/progress.png"), LV_PART_INDICATOR);
    lv_obj_set_style_bg_image_src(progress_slider, cursor_mem ? (const void *) cursor_mem : asset_path("playing_plane/cursor.png"), LV_PART_KNOB);
    /* Real-device bug report: accent color didn't apply here -- see
     * apply_accent_color()'s own comment on why an image-art slider needs
     * bg_image_recolor, not just bg_color. */
    lv_obj_add_style(progress_slider, &style_accent, LV_PART_INDICATOR);
    lv_obj_add_style(progress_slider, &style_accent, LV_PART_KNOB);
    /* Keep the dedicated playing-plane art here: unlike the reused 360px
     * volume rail sprites, these assets match this progress rail's design. */
    lv_obj_set_style_radius(progress_slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(progress_slider, 30, LV_PART_KNOB);
    lv_obj_set_style_height(progress_slider, 30, LV_PART_KNOB);
    /* Real-device bug report: seeking (swiping across the bar) sometimes
     * triggered the app-wide back-swipe gesture instead. Root cause: unlike
     * every other draggable slider in this file (screen_timeout_slider,
     * startup_volume_slider, sleep_timer_slider, idle_shutdown_slider, ...,
     * all of which call this with 20), this 12px-tall bar never widened its
     * touch target past LVGL's tiny built-in default (LV_DPX(8), set in
     * lv_slider_constructor). A touch landing just off that thin band missed
     * the slider's hit-test area entirely and fell through to `overlay`
     * behind it, which -- unlike the slider -- does carry
     * LV_OBJ_FLAG_GESTURE_BUBBLE (see enable_gesture_bubble_recursive()), so
     * the drag bubbled up to screen_gesture_event_cb() as a real navigation
     * swipe. */
    lv_obj_set_ext_click_area(progress_slider, 20);
    lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_ALL, NULL);
    /* See screen_gesture_event_cb()'s own comment -- covers a press that
     * lands just off the slider's own hit-test box (still within
     * ext_click_area's reach for a tap, but a fast swipe's start point can
     * land outside even that) from being hijacked into a back-swipe. */
    register_swipe_dead_zone(progress_slider);

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
    lv_obj_add_style(order_icon, &icon_press_style, LV_STATE_PRESSED); /* see icon_press_style's own comment */

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
    /* Not transport_btn_ctx_t's fixed normal/pressed asset-swap -- this
     * icon's own "normal" image already alternates between btn_play.png and
     * btn_pause.png depending on playback state (set_play_button_state()),
     * so a fixed pressed_path would flash the wrong artwork half the time.
     * icon_press_style dims whichever of the two is currently showing
     * instead. */
    lv_obj_add_style(play_btn, &icon_press_style, LV_STATE_PRESSED);

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
    lv_obj_add_style(more_icon, &icon_press_style, LV_STATE_PRESSED); /* see icon_press_style's own comment */

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

/* ---- Fullscreen lyrics view --------------------------------------------
 * Tap the album art (cover_img_tap_cb above) to open, no animation -- a
 * nav_push()ed screen like any other (not a layered overlay on the player
 * screen), so it gets its own gesture handling for free instead of
 * fighting the player screen's own transport/seek touch handling
 * underneath. Exit is a right-swipe (the standard back gesture) ONLY --
 * real-device feedback: a tap-anywhere-to-dismiss made an ordinary tap
 * while reading (e.g. to pause the auto-follow idle countdown) too easy to
 * fire by accident, and the app-wide left-swipe-to-player-screen gesture
 * (poll_quick_drawer_drag()'s player_swipe_* state) looked like a broken,
 * looping transition when triggered from a screen that's already reached
 * FROM the player screen -- see the lv_screen_active() != gui_lyrics_get_screen()
 * exclusions added to that function, and player_swipe_press_excluded()'s
 * own doc comment for the general shape of this kind of exclusion.
 * lyrics_gesture_event_cb() below is deliberately NOT the shared
 * screen_gesture_event_cb() (which pops via the animated screen_
 * transition_slide()) -- real-device feedback asked for no animation
 * entering OR exiting this screen, so this pops with a plain lv_screen_
 * load() instead, the same instant-cut nav_push() already uses.
 *
 * Virtualization follows the same "a spacer object establishes the real
 * scroll range, a small pool of reused objects is repositioned/relabeled
 * as the view scrolls" mechanic screen_builders.c's compact_list already
 * uses (see compact_list_update_window()) -- just purpose-built and
 * simpler, since the whole parsed lyrics_doc_t is already resident in
 * memory (capped at LYRICS_MAX_LINES) with nothing to page in from disk. ---- */

/* Fixed on-screen y where the active line always sits, in the viewport's
 * own coordinates (25% of the 800px screen) -- real-device feedback: the
 * active line must stay in the same place while the rest of the text
 * scrolls past it (the familiar Spotify/Apple-Music-style fullscreen
 * lyrics behavior), not drift to wherever a viewport-centering formula
 * happens to land it, and the very first line must already start there
 * rather than flush against the top edge. See lyrics_timer_cb()'s own
 * target calculation. */
/* Equal to the anchor above, not an independent value -- this is what
 * makes target = TOP_PAD + index*STRIDE - ANCHOR_Y land at exactly 0 (no
 * clamping needed) for index 0, so the very first line already renders at
 * the anchor position instead of the clamp-to-0 fallback leaving it at the
 * top edge. */
/* "A few seconds of no further manual scrolling" per LYRICS_SUPPORT_DRAFT.md. */















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


/* build_files_screen moved to gui_library.c */



const char * basename_of(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
/* group_song_entries moved to gui_library.c */


const char * playlist_path_at(int index) {
    if (playlist[index]) return playlist[index];
    int sort_pos = playlist_lazy_sort_order[index];
    song_row_t row;
    int got = playlist_lazy_order_is_recency ? metadata_db_get_songs_page_by_recency(sort_pos, 1, &row)
                                              : metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, sort_pos, 1, &row);
    if (got == 1) {
        playlist[index] = strdup(row.path);
    } else {
        playlist[index] = strdup("");
    }
    return playlist[index];
}

void on_file_selected_lazy_all_songs(int selected_index) {
    int64_t count64 = metadata_db_get_song_count();
    int count = count64 > 0 ? (int) count64 : 0;

    free_playlist();
    playlist = calloc((size_t) count, sizeof(char *));
    playlist_count = count;
    playlist_lazy_sort_order = malloc(sizeof(int) * (size_t) count);
    for (int i = 0; i < count; i++) playlist_lazy_sort_order[i] = i;
    playlist_lazy_order_is_recency = false;
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    remote_control_sync_queue(NULL, 0);
    play_track_at(selected_index);
}

void on_file_selected_lazy_recently_added(int selected_index) {
    int64_t count64 = metadata_db_get_song_count();
    int count = count64 > 0 ? (int) count64 : 0;

    free_playlist();
    playlist = calloc((size_t) count, sizeof(char *));
    playlist_count = count;
    playlist_lazy_sort_order = malloc(sizeof(int) * (size_t) count);
    for (int i = 0; i < count; i++) playlist_lazy_sort_order[i] = i;
    playlist_lazy_order_is_recency = true;
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    remote_control_sync_queue(NULL, 0);
    play_track_at(selected_index);
}

const char * gui_plugin_get_play_mode(void) {
    switch ((play_mode_t) current_settings.play_mode) {
        case PLAY_MODE_REPEAT_ALL: return "repeat_all";
        case PLAY_MODE_REPEAT_ONE: return "repeat_one";
        case PLAY_MODE_SHUFFLE:    return "shuffle";
        case PLAY_MODE_SEQUENTIAL:
        default:                   return "sequential";
    }
}

const char * gui_plugin_get_current_track_path(void) {
    if (playlist_index < 0 || playlist_index >= playlist_count) return NULL;
    return playlist_path_at(playlist_index);
}

char ** gui_plugin_get_artist_albums(const char * artist, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 0 || count64 > INT_MAX) return NULL;
    int album_count = (int) count64;
    group_row_t * rows = malloc(sizeof(*rows) * (size_t) album_count);
    char ** names = calloc((size_t) album_count, sizeof(*names));
    if (!rows || !names) { free(rows); free(names); return NULL; }
    int n = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, 0, album_count, rows);
    for (int i = 0; i < n; i++) {
        names[i] = strdup(rows[i].name);
        if (!names[i]) {
            for (int j = 0; j < i; j++) free(names[j]);
            free(names);
            free(rows);
            return NULL;
        }
    }
    free(rows);
    *out_count = n;
    return names;
}

static char ** load_plugin_album_paths(const char * artist, const char * album, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_songs_filtered(NULL, artist, NULL, album);
    if (count64 <= 0 || count64 > INT_MAX) return NULL;
    int count = (int) count64;
    char ** paths = calloc((size_t) count, sizeof(*paths));
    if (!paths) return NULL;
    song_row_t rows[64];
    int loaded = 0;
    while (loaded < count) {
        int want = count - loaded;
        if (want > 64) want = 64;
        int got = metadata_db_get_songs_filtered_page(NULL, artist, NULL, album, loaded, want, rows);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            paths[loaded + i] = strdup(rows[i].path);
            if (!paths[loaded + i]) {
                for (int j = 0; j < loaded + i; j++) free(paths[j]);
                free(paths);
                return NULL;
            }
        }
        loaded += got;
        if (got < want) break;
    }
    *out_count = loaded;
    return paths;
}

char ** gui_plugin_get_album_tracks(const char * artist, const char * album, int * out_count) {
    *out_count = 0;
    return load_plugin_album_paths(artist, album, out_count);
}

char ** gui_plugin_get_next_album_tracks(const char * artist, const char * current_album, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 1 || count64 > INT_MAX) return NULL;
    group_row_t rows[32];
    int offset = 0;
    while (offset < count64) {
        int want = (int) (count64 - offset);
        if (want > 32) want = 32;
        int got = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset, want, rows);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (strcasecmp(rows[i].name, current_album) != 0) continue;
            if (i + 1 < got) return load_plugin_album_paths(artist, rows[i + 1].name, out_count);
            group_row_t next;
            if (metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset + got, 1, &next) == 1)
                return load_plugin_album_paths(artist, next.name, out_count);
            return NULL;
        }
        offset += got;
        if (got < want) break;
    }
    return NULL;
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
    if (library_rescan_active) return false;
    start_library_rescan();
    return library_rescan_active;
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
    char spool_path[PATH_MAX];
    atomic_bool done;
    bool ok;
    int count;
    atomic_int progress;
} scan_walk_work_t;

/* Binary length-prefixed spool: paths can contain whitespace and, unlike a
 * newline-delimited temporary file, this remains correct even for odd names.
 * The spool lives on the SD card, so discovery storage scales with library
 * size without consuming the device's RAM. */
static bool scan_spool_visit_cb(const char * path, void * user) {
    FILE * f = (FILE *) user;
    size_t n = strlen(path);
    if (n > UINT32_MAX) return false;
    uint32_t len = (uint32_t) n;
    return fwrite(&len, sizeof(len), 1, f) == 1 && fwrite(path, 1, n, f) == n;
}

static void * scan_walk_worker(void * arg) {
    scan_walk_work_t * w = (scan_walk_work_t *) arg;
    FILE * spool = fopen(w->spool_path, "wb");
    if (!spool) {
        w->ok = false;
        w->done = true;
        return NULL;
    }
    w->ok = file_browser_walk_all_songs_excluding_top_level(
        w->root, AUDIOBOOKS_LIBRARY_DIR_NAME, scan_spool_visit_cb, spool, &w->count, &w->progress);
    if (fclose(spool) != 0) w->ok = false;
    atomic_store_explicit(&w->done, true, memory_order_release);
    return NULL;
}

#define SCAN_WALK_THREAD_STACK_SIZE (1024 * 1024)
static bool scan_all_songs_with_timeout(const char * root, char * out_spool_path, size_t out_spool_size,
                                        int * out_count) {
    scan_walk_work_t * w = calloc(1, sizeof(*w));
    if (!w) return false;
    snprintf(w->root, sizeof(w->root), "%s", root);
    snprintf(w->spool_path, sizeof(w->spool_path), "%s/.open_hiby_scan_%ld_%p.tmp",
             root, (long) getpid(), (void *) w);

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
    pthread_detach(thread);

    int last_seen_progress = 0;
    int stalled_ms = 0;
    for (;;) {
        if (atomic_load_explicit(&w->done, memory_order_acquire)) {
            bool ok = w->ok;
            if (ok) {
                snprintf(out_spool_path, out_spool_size, "%s", w->spool_path);
                *out_count = w->count;
            } else {
                remove(w->spool_path);
            }
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

    /* The worker may be in uninterruptible I/O. Do not touch/free its state.
     * Its uniquely named spool can be orphaned safely and removed on a later
     * maintenance pass; critically, it owns no tagcache lock or GUI memory. */
    fprintf(stderr, "Warning: scan of %s stalled with no progress for %ds (possible filesystem corruption)\n",
            root, LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS / 1000);
    return false;
}

static bool scan_spool_read_path(FILE * f, char * path, size_t path_size) {
    uint32_t len = 0;
    if (fread(&len, sizeof(len), 1, f) != 1) return false;
    if (len == 0 || len >= path_size) {
        if (fseek(f, (long) len, SEEK_CUR) != 0) return false;
        path[0] = '\0';
        return true;
    }
    if (fread(path, 1, len, f) != len) return false;
    path[len] = '\0';
    return true;
}

static void scan_one_song_into_db(const char * path) {
    struct stat st;
    bool have_stat = stat(path, &st) == 0;
    int64_t mtime = have_stat ? (int64_t) st.st_mtime : 0;
    int64_t size = have_stat ? (int64_t) st.st_size : 0;

    cached_tags_t cached;
    if (have_stat && metadata_db_get(path, mtime, size, &cached)) return;

    track_metadata_t meta;
    metadata_read_isolated(path, &meta, LIBRARY_SCAN_FILE_TIMEOUT_MS);

    cached_tags_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    snprintf(fresh.title, sizeof(fresh.title), "%s", meta.has_title ? meta.title : "");
    snprintf(fresh.artist, sizeof(fresh.artist), "%s", meta.has_artist ? meta.artist : "Unknown Artist");
    snprintf(fresh.album, sizeof(fresh.album), "%s", meta.has_album ? meta.album : "Unknown Album");
    const char * album_artist_value = meta.has_album_artist ? meta.album_artist
                                     : (meta.has_artist ? meta.artist : "Unknown Artist");
    snprintf(fresh.album_artist, sizeof(fresh.album_artist), "%s", album_artist_value);
    snprintf(fresh.genre, sizeof(fresh.genre), "%s", meta.has_genre ? meta.genre : "Unknown Genre");
    free(meta.picture_data);
    free(meta.lyrics); /* always NULL here (metadata_read_isolated() itself already frees/NULLs it before the pipe write), freeing defensively for symmetry */

    if (have_stat) metadata_db_put(path, mtime, size, &fresh);
}

/* Overall scan progress, polled by update_timer_cb while library_rescan_active
 * is true to drive the "Updating music database..." screen's progress bar
 * (Settings > Update Music Database) -- purely cosmetic, for the user's
 * peace of mind that a rescan is actually moving rather than stuck, since
 * the underlying incremental scan (metadata_db.c's mtime/size cache) is
 * already fast on an unchanged library. _total is set once discovered_count
 * is known (library_scan_once(), before its spool-reading loop starts);
 * _done is advanced by that same loop, one file at a time (see
 * scan_one_song_into_db()'s own caller in library_scan_once()). */
static volatile int library_scan_progress_done = 0;
static int library_scan_progress_total = 0;

/* Defined later, alongside the rest of the Books screen (needs
 * books_scan_txt_files_with_timeout(), the live-walk fallback it shares
 * with populate_books_files_screen()'s old scanning code). Folds the
 * now-removed "Scanning" row's job into this same rescan, per real-device
 * feedback -- one rescan action, not two separate ones for music and
 * books. */


/* Refreshes the persistent playlist cache (metadata_db.c) from
 * PLAYLISTS_DIR only (the SD card's Playlists folder), not a walk of the
 * whole music tree. Folded into library_scan_once() so it runs on Update
 * Music Database. Ordinary in-app create/delete update the cache with a
 * single insert/delete instead of calling this. */
static void rescan_playlists(void) {
    char ** paths = NULL;
    int count = 0;
    playlist_files_scan(PLAYLISTS_DIR, &paths, &count);
    metadata_db_playlist_replace_all(paths, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}


void library_scan_once(void) {
    library_scan_progress_done = 0;
    library_scan_progress_total = 0;

    metadata_db_open();
    gui_books_rescan();
    rescan_playlists();

    char spool_path[PATH_MAX] = {0};
    int discovered_count = 0;
    if (!scan_all_songs_with_timeout(MUSIC_ROOT_DIR, spool_path, sizeof(spool_path), &discovered_count)) {
        return; /* preserve the last known-good in-memory + on-disk library */
    }

    library_scan_progress_total = discovered_count;
    metadata_db_begin_update();

    FILE * spool = fopen(spool_path, "rb");
    if (!spool) {
        metadata_db_abort_update();
        remove(spool_path);
        return;
    }

    bool complete = true;
    char path[PATH_MAX];
    int done = 0;
    while (done < discovered_count) {
        if (!scan_spool_read_path(spool, path, sizeof(path))) {
            complete = false;
            break;
        }
        if (path[0] != '\0') scan_one_song_into_db(path);
        library_scan_progress_done = ++done;
    }
    fclose(spool);
    remove(spool_path);

    if (!complete) {
        metadata_db_abort_update();
        return;
    }

    if (!metadata_db_end_update())
        fprintf(stderr, "Warning: music database commit failed -- keeping last on-disk library\n");

    library_load_from_cache_only();
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
 * here (not removed): opening the on-disk tagcache (metadata_db.c) is
 * a bounded index load, not per-file filesystem I/O, and
 * finishes in well under a second even for a large library -- it's
 * specifically the filesystem walk + per-file tag-parsing pass that had to
 * move to the user-triggered-only path. All four library screens (All
 * Songs, Artists, Albums, Album Artist) and every drill-down/search/A-Z
 * feature reachable from them are DB-paged from the moment they're built,
 * so boot never needs to build any whole-library in-memory snapshot at
 * all -- the boot-time OOM this function exists to avoid (a 32,000-song
 * library's worth of per-song structs, plus a widget-per-row cost, turning
 * a completed scan into a boot loop on this device's 55MB RAM) simply has
 * nothing left to trigger it. */
void library_load_from_cache_only(void) {
    library_scan_progress_done = 0;
    library_scan_progress_total = 0;

    /* Close first so a remounted card is not served from a still-open
     * handle against the previous (or empty unmounted) mount. */
    metadata_db_close();
    metadata_db_open();
}

/* Search binding IDs -- indices into search_bindings[], defined together
 * with the rest of the live-search infrastructure much further down (near
 * the A-Z index section). Declared this early only because
 * all_songs_row_click_cb below (used by build_all_songs_screen(), itself
 * earlier in the file than the other three screens' own row-click
 * callbacks) needs to remap a filtered display index back to the real one
// all_songs, recently_added, group_songs moved to gui_library.c


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


/* Background download thread + a "done" flag polled from update_timer_cb --
 * the same shape as audio.c's own track_finished flag, for the same reason:
 * LVGL isn't thread-safe, so the thread can't touch any screen/widget
 * state directly. */
static volatile bool download_success_flag = false;

typedef struct {
    char url[1536];
    char dest_path[512];
    bool verify_tls;
} download_request_t;




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

/* --- Modal API Implementation --- */
/* gui_busy_* and gui_theme_font moved to gui_notifications.c and gui_theme.c */

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



/* songs/albums_to_expand are handed off to the thread, which frees them --
 * callers must pass owned, malloc'd copies, never a shared cache pointer
 * (subsonic_songs_cache/subsonic_albums_cache themselves can be replaced by
 * the next browse click while this download is still in flight). Exactly
 * one of songs or albums_to_expand should be non-NULL (Mode A vs Mode B --
 * see subsonic_library_download_request_t's own comment). playlist_name
 * NULL/empty means "don't create a .m3u." */


/* Shared by the artists/albums/songs screens: a titled list of compact
 * rows, one per index-based click callback, built fresh from a persistent
 * (never-deleted) list container each time the user drills into a new
 * artist/album -- same "rebuild in place" approach as the local library's
 * group_songs_screen, since a real server library can have far too many
 * artists/albums/songs to pre-build a screen per one. */


/* Pill-styled toggle/chevron rows for the Wi-Fi/Bluetooth settings screens'
 * dynamic top section -- same real assets (touch_list/item_bg.png,
 * settings/on.png/off.png) as screen_builders.c's build_pill_list_screen(),
 * but built directly into an already-existing dynamically-repopulated list
 * container rather than that function's own fixed item array, since these
 * screens mix a handful of fixed settings rows with variable-length device/
 * network lists below them (build_pill_list_screen has no notion of
 * "and also whatever these dynamic sections need appended after"). */
lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    int32_t row_width = pill_row_default_width();
    lv_obj_set_size(row, row_width, 124);
    lv_obj_add_style(row, &style_theme_screen_bg, 0);
    if (row_width == 448) {
        lv_obj_set_style_bg_image_src(row, asset_path("touch_list/item_bg.png"), 0);
    } else {
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    }
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 24, 0);
    configure_scrolling_row_label(label, row_width - 136);
    return row;
}

lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click) {
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

lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * label_text, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);

    /* No matching real chevron asset found in theme2 at this screen scale
     * (see screen_builders.c's own PILL_ACCESSORY_CHEVRON) -- plain text is
     * the honest fallback rather than forcing a mismatched sprite. */
    lv_obj_t * chevron = lv_label_create(row);
    lv_label_set_text(chevron, ">");
    lv_obj_add_style(chevron, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(chevron, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -20, 0);

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, NULL);
    return row;
}

lv_obj_t * add_section_header(lv_obj_t * parent, const char * text) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
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

/* subsonic_songs_screen is reused for "one album's songs" (Download creates
 * no playlist file) and "one playlist's songs" (Download also creates a
 * local .m3u alongside the downloaded files) -- this tracks which, and the
 * playlist's own name when it's the latter. Set by whichever click handler
 * populates the screen. */


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

/* The actual logic behind tapping a Subsonic song, index into subsonic_
 * songs_cache -- split out from subsonic_song_row_click_cb() (immediately
 * below, the real LVGL callback) purely so it has a plain (int) signature
 * callable directly, without needing to fabricate an lv_event_t. */











/* ---- Subsonic screen redesign: Playlists (new top-level menu row) and the
 * flat "every album in the library" Albums row -- both reuse existing
 * screens/caches (subsonic_songs_screen and subsonic_albums_screen
 * respectively) rather than needing screens of their own, since a
 * playlist's songs and an album's songs are shown identically, and the flat
 * album list is shown identically to one artist's album list. ---- */





/* Real-device bug report: both Download buttons started the actual download
 * the instant they were tapped, no confirmation -- easy to hit by accident
 * (e.g. reaching for the search icon right next to the songs one) with no
 * way to back out once a possibly-large download had already started.
 * Split each into "show a confirm popup" (the icon's own click handler,
 * below) + "the actual download" (run only from the popup's own Download
 * button) -- same confirm/cancel popup shape as every other destructive-ish
 * action in this file (build_confirm_popup()). One shared popup for both
 * buttons (same as wifi_action_popup/bt_action_popup's own "one popup, a
 * pending-kind flag picks what Confirm actually does" shape) rather than
 * two near-identical popup instances. */





/* Top-of-screen Download button shared by subsonic_songs_screen (an
 * album's or a playlist's songs -- see subsonic_songs_context_is_playlist)
 * -- downloads exactly what's currently shown, into the local library. */


/* Top-of-screen Download button on subsonic_albums_screen, active only when
 * it's showing "an Artist page" (see subsonic_albums_context_artist) --
 * downloads every song across every album currently listed. Hands the
 * thread the album list itself (Mode B, see subsonic_library_download_
 * request_t's own comment) rather than pre-fetching every album's songs
 * here on the UI thread first, since that could be a lot of sequential
 * network round trips for an artist with many albums. */




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
/* Snapshot of whichever server this in-flight attempt is for -- the
 * request struct itself is freed inside the thread function before
 * poll_subsonic_connect() ever runs, so this is what poll_subsonic_
 * connect() reads back to know what to persist on success. */





/* ---- Saved Servers -- every server metadata_db_subsonic_server_save() has
 * remembered after a successful connect (see poll_subsonic_connect()
 * above), listed by URL; tapping one reconnects with its stored
 * credentials via the same start_subsonic_connect() the New Connection
 * form below also uses. ---- */







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













/* ---- Subsonic entry point -- just the two options; subsonic_tile_cb below
 * (Stream Media's Subsonic tile) pushes this instead of what used to be a
 * single combined setup+connect screen for one connection only. ---- */

/* artist_albums moved to gui_library.c */
/* Library core moved to gui_library.c */
/*
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

    lv_obj_t * scr = build_icon_grid_screen("Stream Media", generic_back_cb, items, count, 100, false, 0);
    finalize_screen_navigation(scr);
    return scr;
}


/* ---- Basic .txt reader ----
 *
 * "Books" row of the Books menu (originally "Files", folded into this one
 * row and renamed per real-device feedback -- see build_books_screen()):
 * a flat list of every .txt file under BOOKS_ROOT_DIR, sourced from the
 * persistent book cache (metadata_db.c), tap one to read it. Keeping book
 * discovery inside this dedicated directory avoids recursively walking a
 * large music library just to find text files. Deliberately basic --
 * no per-directory navigation (file_browser.c's UI, built for browsing
 * playable audio and building playlists, doesn't fit this shape of
 * problem), no pagination, just load the whole file into one scrollable
 * label. */














/* Real-device bug report: recursively looking for books across the whole SD
 * card visibly slowed the device down and could look like a crash. Book
 * discovery is now restricted to BOOKS_ROOT_DIR, but remains on a bounded
 * worker so a damaged card cannot block the database-update worker forever.
 * There's no per-file tag-reading here to cache the way
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







/* Real-device feedback: the Books menu's old separate "Scanning" row was a
 * dead stub, and the actual per-visit scan (in populate_books_files_screen(),
 * before this existed) made that screen slow to open every time. Both
 * problems share one fix: a real persistent book cache (metadata_db.c, same shape as media's
 * own), refreshed only here -- called from library_scan_once(), i.e. folded
 * into the existing Settings > Update Music Database action, not a separate
 * menu item. The stock database is deliberately not consulted here: it can
 * contain text files from anywhere on the card, while this player's book
 * contract is now exclusively BOOKS_ROOT_DIR. */












/* Shared click handler for every plugin-registered Books list row below --
 * user_data is the row's index into plugin_manager's own plugin_books_
 * list_items[] (not an LVGL object), same index-not-object shape
 * plugin_stream_tile_click_cb() already uses for Stream Media tiles. */


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


/* ---- Firmware update confirmation popup -- same hand-built top-layer
 * overlay shape as eq_reset_popup/bt_dac_leave_popup (this codebase doesn't
 * use LVGL's lv_msgbox anywhere). Confirmation is required since this
 * reboots the device into its own recovery partition -- see
 * firmware_update.h for how that reuses the exact same "*.upt on the SD
 * card + /usr/bin/bootmode.sh Recovery" mechanism the stock closed-source
 * hiby_player's own "Firmware Update" menu item uses, so no custom flashing
 * logic lives here at all. ---- */

/* Rebuild the queue in the same logical context that supplied last_track.
 * Older settings files have kind 0 and retain the legacy containing-folder
 * fallback. Album and All Songs contexts are reconstructed via direct DB
 * queries (metadata_db_count_songs_filtered()/get_songs_filtered_page()/
 * get_song_title_offset()) -- so the player's List action and next/prev
 * order survive a reboot instead of silently becoming a different queue --
 * with no dependency on any in-memory library array being loaded. If
 * last_track no longer resolves in the DB (metadata_db_get_song_by_path()
 * fails, e.g. the file was removed since the last scan) or its saved kind's
 * own DB lookup comes up empty, every kind-1/kind-2 branch below falls
 * through to the same live-filesystem-walk fallback (file_browser_build_
 * playlist_for_path()) that already handles kind 0. */
static bool resume_playlist_needs_lazy_order = false;

static bool build_saved_resume_playlist(char *** out_playlist, int * out_count, int * out_index) {
    resume_playlist_needs_lazy_order = false;
    song_row_t current;
    bool indexed = metadata_db_get_song_by_path(current_settings.last_track, &current);

    if (indexed && current_settings.last_source_kind == 2 && current_settings.last_source_name[0] &&
        strcasecmp(current.tags.album, current_settings.last_source_name) == 0) {
        int64_t count64 = metadata_db_count_songs_filtered(NULL, NULL, current.tags.album_artist, current.tags.album);
        if (count64 > 0 && count64 <= INT_MAX) {
            int count = (int) count64;
            group_song_entry_t * entries = calloc((size_t) count, sizeof(*entries));
            char ** paths = calloc((size_t) count, sizeof(*paths));
            song_row_t rows[64];
            int loaded = 0, selected = -1;
            while (entries && paths && loaded < count) {
                int want = count - loaded;
                if (want > 64) want = 64;
                int got = metadata_db_get_songs_filtered_page(NULL, NULL, current.tags.album_artist,
                                                               current.tags.album, loaded, want, rows);
                if (got <= 0) break;
                for (int i = 0; i < got; i++) {
                    char title[128];
                    metadata_db_song_display_title(&rows[i], title, sizeof(title));
                    entries[loaded + i].path = strdup(rows[i].path);
                    entries[loaded + i].title = strdup(title);
                    paths[loaded + i] = strdup(rows[i].path);
                    if (strcmp(rows[i].path, current_settings.last_track) == 0) selected = loaded + i;
                    if (!entries[loaded + i].path || !entries[loaded + i].title || !paths[loaded + i]) {
                        got = 0;
                        break;
                    }
                }
                if (got <= 0) break;
                loaded += got;
                if (got < want) break;
            }
            if (loaded == count && selected >= 0) {
                set_player_source_group_songs_direct(entries, count, current.tags.album, selected);
                free_group_song_entries(entries, count);
                *out_playlist = paths;
                *out_count = count;
                *out_index = selected;
                return true;
            }
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths ? paths[i] : NULL);
            free(paths);
        }
    }

    if (indexed && current_settings.last_source_kind == 1) {
        int64_t count64 = metadata_db_get_song_count();
        int64_t selected64 = metadata_db_get_song_title_offset(current_settings.last_track);
        if (count64 > 0 && count64 <= INT_MAX && selected64 >= 0 && selected64 < count64) {
            *out_playlist = calloc((size_t) count64, sizeof(char *));
            if (!*out_playlist) return false;
            *out_count = (int) count64;
            *out_index = (int) selected64;
            resume_playlist_needs_lazy_order = true;
            set_player_source_all_songs(*out_index);
            return true;
        }
    }

    clear_player_source();
    return file_browser_build_playlist_for_path(current_settings.last_track, out_playlist, out_count, out_index);
}

static void install_saved_resume_playlist(char ** resume_playlist, int resume_count) {
    free_playlist();
    playlist = resume_playlist;
    playlist_count = resume_count;
    if (resume_playlist_needs_lazy_order) {
        playlist_lazy_sort_order = malloc(sizeof(int) * (size_t) resume_count);
        if (playlist_lazy_sort_order) {
            for (int i = 0; i < resume_count; i++) playlist_lazy_sort_order[i] = i;
        }
    }
}

static void prepare_deferred_resume(int index, double start_seconds) {
    playlist_index = index;
    track_metadata_t meta;
    apply_track_metadata_to_ui(index, &meta);
    set_play_button_state(false);
    deferred_resume_pending = true;
    deferred_resume_position = start_seconds;
    nav_push(player_screen);
}

/* Wakes quick_drawer_drag_timer/az_index_drag_timer the instant a new press
 * begins anywhere on screen -- registered on the pointer indev itself
 * (LV_EVENT_PRESSED, not tied to any one widget) rather than each screen's
 * own objects, for the same reason poll_quick_drawer_drag()'s own doc
 * comment gives for polling raw indev state in the first place: this is the
 * one press-related event LVGL dispatches regardless of which object was
 * actually hit. Both timers pause themselves the instant they see nothing
 * left to track (see their own handle comments); without this, a paused
 * timer would never resume, silently breaking every gesture that relies on
 * it. lv_timer_resume() on an already-running timer is a harmless no-op, so
 * this doesn't need to check either timer's current state first. */
static void resume_fast_gesture_timers_cb(lv_event_t * e) {
    (void) e;
    lv_timer_resume(quick_drawer_drag_timer);
    lv_timer_resume(az_index_drag_timer);
}

void gui_init(uint32_t screen_width, uint32_t screen_height) {
#ifndef HOST_BUILD
    boot_checkpoint("gui_init entered");
    bt_boot_suppress_enabled = access(BT_INIT_OK_FLAG_PATH, F_OK) != 0;
    bt_boot_off_observations = 0;
#endif
    settings_load(&current_settings);
#ifndef HOST_BUILD
    boot_checkpoint("settings_load done");
#endif
    /* Must run before anything could turn Wi-Fi/Bluetooth on and trigger
     * wifi_on.sh/bt_init's own one-time read of the file this bind-mounts
     * over -- see hostname_apply()'s own comment. */
    gui_theme_init();
    gui_notifications_init();
    hostname_apply(current_settings.hostname);

    /* Must run before any screen below captures a gui_theme_font(GUI_FONT_ROLE_SUBTEXT)/20/22/28
     * pointer into its own style -- see this function's own doc comment. */

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
    backlight_set_normal_percent(current_settings.brightness_percent);

    led_control_apply(current_settings.led_indicator_enabled);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    safe_charging_poll(current_settings.safe_charging_enabled, true);
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

    /* LV_OPA_80, not LV_OPA_COVER -- see apply_accent_color()'s own comment
     * on this same property for why COVER flattens on.png's handle and
     * track into one indistinguishable solid color. Mirrored here since
     * this is the initial setup at boot, before apply_accent_color() might
     * ever run again. */


    fallback_font_init_early(current_settings.font_size_tier, current_settings.lyrics_font_size_tier); /* must run before any style/screen captures &app_font_16/&app_font_22/&app_font_lyrics -- see fallback_font.h */

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
    gui_lyrics_init();
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
    /* No whole-library load anywhere in this boot path, on purpose --
     * remote_control.c queries metadata_db.c directly (its own METADATA_DB_
     * GUARD) rather than needing a synced copy of the library, and each of
     * the five build_*_screen() calls above already activates its own
     * paged provider against the DB internally (see build_all_songs_
     * screen()'s own comment), so there's nothing left that would need a
     * whole-library array at boot for any reason, Remote Control enabled
     * or not. */
/* A-Z index & search registered in gui_library_init */


/* files_search and artist_albums initialized in gui_library_init */
    gui_text_input_init();
    stream_media_screen = build_stream_media_screen();


    /* Artists/Albums, unlike the rest of this file's ~25 build_subsonic_
     * list_screen() screens, can genuinely scale with library size --
     * getArtists.view has no size cap and getAlbumList2.view's own "every
     * album" browse returns up to 500 (see subsonic_client.c) -- so these
     * two use the same virtualized compact_list build_all_songs_screen()/
     * build_artists_screen()/etc. already do, not the plain flex list every
     * other (inherently small/bounded) settings/browse screen in this file
     * shares. Built empty (item_count 0); populated later via compact_list_
     * set_items() once a real fetch actually completes (poll_subsonic_
     * connect()/poll_subsonic_browse()), same lazy-population shape as the
     * local library screens. enable_now_playing is false for both -- a
     * Subsonic artist/album row has no local playlist_index to highlight
     * against. */
    gui_subsonic_init();

    /* Artists/Albums, unlike the rest of this file's ~25 build_subsonic_
     * list_screen() screens, can genuinely scale with library size --
     * getArtists.view has no size cap and getAlbumList2.view's own "every
     * album" browse returns up to 500 (see subsonic_client.c) -- so these
     * two use the same virtualized compact_list build_all_songs_screen()/
     * build_artists_screen()/etc. already do, not the plain flex list every
     * other (inherently small/bounded) settings/browse screen in this file
     * shares. Built empty (item_count 0); populated later via compact_list_
     * set_items() once a real fetch actually completes (poll_subsonic_
     * connect()/poll_subsonic_browse()), same lazy-population shape as the
     * local library screens. enable_now_playing is false for both -- a
     * Subsonic artist/album row has no local playlist_index to highlight
     * against. */

    /* Top-of-screen Download buttons -- see subsonic_download_artist_btn_cb()/
     * subsonic_download_songs_btn_cb() for what each actually downloads.
     * Added directly onto the already-built screens (same pattern as
     * build_wifi_screen()'s Rescan button) rather than threading a new
     * parameter through build_subsonic_list_screen() itself, which ~20
     * other, unrelated screens also share. Hidden by default -- shown only
     * when the screen's own click handler determines it's actually
     * applicable (an Artist page for the albums one; always for the songs
     * one, whether it's an album or a playlist). */
    /* Real-device bug report: the "Download" text button started the
     * download immediately with no confirmation (see subsonic_download_
     * songs_btn_cb()'s/subsonic_download_artist_btn_cb()'s own comment for
     * that half of the fix) and, separately, was asked to become an icon
     * instead of a text label -- a plain downward-arrow-into-a-tray glyph
     * (stream_media/download.png, a new asset this app adds via the
     * THEME_OVERRIDE_ROOT mechanism, same as stream_media/subsonic.png's
     * own precedent -- there's no stock icon for a Subsonic-only feature)
     * rather than reusing an LVGL built-in symbol font glyph, which would
     * have clashed with this app's own consistently hand-drawn icon set. */

    /* Same live search as Artists/Albums/Album Artist/All Songs (see the
     * search_binding_t infra above), extended to Subsonic's own Artists and
     * Albums lists -- now virtualized compact_lists too (see their own
     * build_compact_list_screen() comment above), so search_apply_filter()
     * repopulates them via compact_list_set_items() same as every other
     * binding, no special-casing needed here anymore. No A-Z index -- same
     * reasoning as Files (search-only), and unlike Files this isn't even
     * alphabetically sorted (server order). Registered once here only --
     * unlike the local-library bindings, these screens are never rebuilt (no
     * equivalent of a library rescan), so there's no second registration
     * site to mirror this at. */
    gui_library_init();
    gui_network_init();
    gui_settings_init();
    gui_books_init();
    build_power_off_countdown_popup();
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
    register_static_snapshot(4, gui_books_get_screen());
    register_static_snapshot(5, about_screen);
    register_static_snapshot(6, settings_screen);
    register_static_snapshot(7, settings_system_screen);
    register_static_snapshot(8, dac_home_screen);

    build_status_bar();
    build_volume_popup();
    build_home_indicator_bar();
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

    /* The screen-timeout clock belongs to the interactive UI, not startup.
     * update_timer_cb used to be created before the splash settle loop, so
     * LVGL counted library/screen construction and the visible splash as
     * user inactivity; with a short timeout it could switch the panel off
     * immediately after (or even during) the splash. Establish the activity
     * baseline at the exact splash -> Home transition, then start runtime
     * polling. The fast gesture timers likewise have no work while the
     * splash is the only visible screen -- and self-pause again (see their
     * own handle comments) the moment the very first tick after this finds
     * nothing pressed, rather than running at ~60fps for the rest of the
     * app's life regardless of whether anyone's touching the screen: with
     * no other timer registered below LV_DEF_REFR_PERIOD, these two used to
     * be the sole reason main()'s own usleep(lv_timer_handler()) could never
     * sleep longer than one frame, forever, including idle/screen-off
     * playback. resume_fast_gesture_timers_cb() wakes both again the
     * instant a new press begins, on whichever indev is the real
     * touchscreen (find_pointer_indev() is safe to call here -- the target
     * build's touch indev is already registered by main.c well before
     * gui_init() runs). */
    gui_reset_interactive_timeout_baseline();
    lv_timer_create(update_timer_cb, 500, NULL);
    quick_drawer_drag_timer = lv_timer_create(poll_quick_drawer_drag, LV_DEF_REFR_PERIOD, NULL);
    az_index_drag_timer = lv_timer_create(poll_az_index_drag, LV_DEF_REFR_PERIOD, NULL);
    lv_indev_t * gesture_indev = find_pointer_indev();
    if (gesture_indev) lv_indev_add_event_cb(gesture_indev, resume_fast_gesture_timers_cb, LV_EVENT_PRESSED, NULL);
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
            if (build_saved_resume_playlist(&resume_playlist, &resume_count, &resume_index)) {
                install_saved_resume_playlist(resume_playlist, resume_count);
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
        if (build_saved_resume_playlist(&resume_playlist, &resume_count, &resume_index)) {
            install_saved_resume_playlist(resume_playlist, resume_count);
            if (current_settings.resume_mode == 2) {
                /* Do not open ALSA at all until the user presses Play.  On
                 * a headphone-less boot, start-then-pause could lose the
                 * race to an output-open failure and consume this queue. */
                prepare_deferred_resume(resume_index, current_settings.last_position);
            } else play_track_at_from(resume_index, current_settings.last_position);
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
