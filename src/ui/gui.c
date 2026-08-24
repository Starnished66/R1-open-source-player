#include "gui.h"
#include "gui_library.h"
#include "gui_queue.h"
#include "gui_player.h"
#include "gui_plugins.h"
#include "gui_shell.h"
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

lv_obj_t * stream_media_screen;

void sync_player_topbar_visibility(lv_obj_t * screen);







/* The 480x320 panel behind the transport controls (title/artist/progress/
 * time/controls_row) -- built in build_player_screen(), but also targeted
 * by poll_cover_decode()/compute_reflection_bytes() below to swap its background
 * between the plain static buttom.png (no embedded art to reflect) and a
 * freshly generated per-track reflection, hence file-scope rather than a
 * local inside build_player_screen(). */
/* Backing pixels for the currently-displayed embedded cover art, if any --
 * a plain RGB565 bitmap produced by cover_decode_to_rgb565() (see
 * poll_cover_decode() below), not the original compressed JPEG/PNG bytes. Freed
 * and replaced whenever a new track loads; must outlive the lv_image_set_src()
 * call since current_cover_dsc.data just points at it, unlike a plain PNG
 * file path. */
uint8_t * current_cover_bytes;
int current_cover_for_index = -1;
/* Same idea as current_cover_bytes/current_cover_dsc above, for the
 * generated reflection (see generate_reflection()) shown as
 * player_overlay_panel's background. */



bool favorite_is_set = false;
/* Clock, top bar center: real topbar/N.png digit + topbar/colon.png
 * sprites, same asset family as the volume readout below, instead of an
 * lv_label -- switched from font text so it's pixel-identical in size/style
 * to the volume/headphone indicator rather than an approximate font-size
 * match (real-device feedback: "match the size of ... the volume and
 * headphone indicator"). Fixed HH:MM layout, always all 5 slots visible
 * (no leading-zero hiding the way the volume/battery readouts need, since
 * a clock always shows both digits of the hour). */
/* Settings -> System -> "24-Hour Clock" (current_settings.clock_24h). Extra
 * flex-row child after the 5 digit slots, hidden entirely in 24h mode --
 * topbar/am.png and topbar/pm.png are pre-existing theme assets, unused by
 * any code before this setting. clock_topbar_group is LV_SIZE_CONTENT and
 * center-aligned to the status bar band, so hiding/showing this slot
 * reflows and re-centers the whole clock automatically, same as any other
 * flex child visibility change in this file. */
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

/* Loud-volume warning color threshold, read once at startup from the stock
 * firmware's own /usr/resource/config.json (see device_config.h) -- e.g. a
 * real R1 Pro had this set to 40 via the stock Settings screen. -1 means
 * "feature disabled" (file missing/host build, or vol_warn_enable=0 in the
 * config), in which case the volume digits always stay their native white
 * and never recolor red, regardless of level. */

/* Quick-access drawer mirrors of the persistent status bar / player-screen
 * widgets above -- kept in sync from the same single update sites (see
 * refresh_clock_label/refresh_battery_topbar/refresh_wifi_icon/
 * set_play_button_state/favorite_icon_event_cb/apply_track_metadata_to_ui)
 * rather than introducing a second, separately-polled source of truth.
 * NULL until build_quick_drawer() runs; every update site guards on that. */

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


char ** playlist = NULL;
int playlist_count = 0;
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
int * playlist_lazy_sort_order = NULL;
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
int queued_pending_count = 0;
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
/* play_mode_t defined in gui.h */


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

static void cancel_pending_progress_seek(void) {
    if (pending_progress_seek_timer) {
        lv_timer_delete(pending_progress_seek_timer);
        pending_progress_seek_timer = NULL;
    }
}

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
bool player_transition_cache_dirty = true;

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

void player_transition_cache_async_cb(void * unused) {
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

/* slide_transition_ctx_t defined in gui.h */

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

void slide_transition_anim_x_cb(void * var, int32_t v) {
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

void slide_transition_done_cb(lv_anim_t * a) {
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
slide_transition_ctx_t * begin_slide_transition(lv_obj_t * to_scr, bool forward) {
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
/* Defined later alongside player_swipe_press_excluded()'s own raw-polling
 * dead-zone machinery -- needed here too, by screen_gesture_event_cb()
 * below, see its own comment. */
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
/* Status bar and Quick Drawer moved to gui_shell.c */


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

const char * play_mode_icon_asset(play_mode_t mode) {
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
int compute_manual_step_index(int index, int direction) {
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
void get_display_names(const char * path, char * title_out, size_t title_size,
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



void set_play_button_state(bool is_playing) {
    lv_image_set_src(play_btn, asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png"));
    player_transition_mark_dirty(); /* play_btn lives on player_screen -- see the cache's own doc comment */
    if (quick_drawer_play_btn) {
        lv_image_set_src(quick_drawer_play_btn,
                         asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png"));
        quick_drawer_mark_snapshot_dirty();
    }
}

/* resolve_replaygain moved to gui_player.c */

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
void queue_add_song(const char * path) {
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
char plugin_now_playing_title[128];
char plugin_now_playing_artist[128];
char plugin_now_playing_album[128];
double plugin_now_playing_duration;
bool plugin_now_playing_loaded = false;

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

void play_track_at(int index) {
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

void toggle_play_pause(void) {
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

void play_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    toggle_play_pause();
}

/* Standard CD-player/iPod convention: a tap partway into a track restarts
 * it, and only a tap already near the start (real device feedback: "first
 * press rewind, second press goes to previous song") moves to the actual
 * previous track -- no separate double-tap timer needed, since "already
 * near the start" is naturally true right after the first tap rewound it. */
#define PREV_BUTTON_REWIND_THRESHOLD_SECONDS 3.0

void prev_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0) return;

    if (audio_get_position_seconds() > PREV_BUTTON_REWIND_THRESHOLD_SECONDS) {
        audio_seek(0.0);
        return;
    }

    int prev_index = compute_manual_step_index(playlist_index, -1);
    if (prev_index >= 0) play_track_at(prev_index);
}

void next_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0) return;
    int next_index = compute_manual_step_index(playlist_index, 1);
    if (next_index >= 0) play_track_at(next_index);
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



void volume_slider_event_cb(lv_event_t * e) {
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


static bool download_active;
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
            gui_shell_resume_connections(wifi_was_on_before_suspend, bt_was_on_before_suspend);
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
            gui_shell_suspend_connections(&wifi_was_on_before_suspend, &bt_was_on_before_suspend);
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
        gui_shell_update_topbar(screen_just_woke);
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
    gui_shell_poll();
    plugin_manager_poll();
    poll_bt_scan();
    poll_bt_connect();
    poll_bt_forget();
    poll_library_rescan();
    poll_sd_format();
    poll_import_web_stop();
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

    gui_player_update_progress();

    refresh_format_badge();
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
/* Queue screen moved to gui_queue.c */
/* Plugin list and settings screens moved to gui_plugins.c */

/* Player screen and popups moved to gui_player.c */
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

/* Plugin settings list screens moved to gui_plugins.c */



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
    gui_shell_resume_fast_timers();
    gui_library_resume_fast_timers();
}

void gui_init(uint32_t screen_width, uint32_t screen_height) {
#ifndef HOST_BUILD
    boot_checkpoint("gui_init entered");
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

    gui_player_init(screen_width, screen_height);
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
    gui_queue_init();
    gui_plugins_init();
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
