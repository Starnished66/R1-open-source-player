#ifndef GUI_H
#define GUI_H

#include "lvgl/lvgl.h"
#include <stdint.h>
#include <stddef.h>

/* Initialize the user interface elements and callbacks */
void gui_init(uint32_t screen_width, uint32_t screen_height);

/* Shows a minimal boot-settle splash screen -- call once, as early as
 * possible after display setup, before gui_init(). See gui.c's own
 * comment above the definition for why. Target-only: HOST_BUILD callers
 * don't have a real cold-boot race to guard against. */
#ifndef HOST_BUILD
void gui_show_boot_splash(void);
#endif

/* ---- Bridge for src/plugins/plugin_manager.c ----
 * plugin_manager.c owns Lua state/script lifecycle and has no LVGL code of
 * its own; these let a plugin's C API calls (plugin.show_list/play_file/
 * play_list/show_toast, see plugin_manager.c's register_plugin_api()) reach
 * gui.c's existing screens/playback plumbing instead of duplicating it. */

/* Repopulates and pushes a shared plugin-list screen (one of a small
 * reusable pool -- see gui.c's own comment on why a pool rather than a
 * single shared screen) showing `labels[0..count)` as plain tappable rows.
 * Tapping row i calls plugin_manager_list_item_selected(i). icon_paths[i]/
 * text_sizes[i] (either may be NULL wholesale, or contain per-row NULLs) are
 * optional per-row icon (raw absolute filesystem path, see
 * screen_builders.h's pill_row_apply_icon())/text-size ("small"/"medium"/
 * "large", already validated by plugin_manager.c) -- a row with neither set
 * anywhere in this call keeps today's exact plain-label rendering. height
 * (0 = default 84px) applies to every row in this call, not per-row. */
void gui_plugin_show_list(const char * title, const char * const * labels, const char * const * icon_paths,
                           const char * const * text_sizes, int32_t height, int count);

/* Repopulates and pushes a shared plugin-settings-list screen -- a SEPARATE
 * pool from gui_plugin_show_list()'s own (PLUGIN_SETTINGS_LIST_SCREEN_POOL_
 * SIZE slots, plugin_manager.h), for a plugin's own nested settings submenu
 * built from real toggle/slider rows, not plain tappable text. Parallel
 * arrays, row_types[i] one of PLUGIN_SETTINGS_ROW_TAP/_TOGGLE/_SLIDER
 * (plugin_manager.h) -- toggle_initial[]/slider_min[]/slider_max[]/
 * slider_value[] are read only for the matching row type, ignored (may be
 * garbage) otherwise. icon_paths[i]/heights[i]/text_sizes[i] are the same
 * per-row options as gui_plugin_show_list() above, except height IS per-row
 * here (ignored for a "slider" row -- see screen_builders.h's PILL_ROW_
 * HEIGHT_MIN comment for why a resized row needs a different background,
 * which a slider's own fixed-layout card doesn't support). Returns the pool
 * slot index this call landed in, so plugin_manager.c can key its own
 * per-slot Lua callback-ref storage off the same slot gui.c actually used. */
int gui_plugin_show_settings_list(const char * title, const int * row_types, const char * const * labels,
                                   const bool * toggle_initial, const int * slider_min, const int * slider_max,
                                   const int * slider_value, const char * const * icon_paths, const int32_t * heights,
                                   const char * const * text_sizes, int count);

/* Starts playback of a brand-new playlist built from `paths[0..count)`,
 * starting at paths[start_index] -- same "starting something new clears
 * Up Next" semantics as every other play-launch path (on_file_selected()).
 * Copies paths/strings itself; caller retains ownership of its own array. */
void gui_plugin_play_paths(const char * const * paths, int count, int start_index);

/* Shows the same transient toast used elsewhere in the app (e.g. "Added to
 * queue"). */
void gui_plugin_show_toast(const char * msg);

/* Sets one of the three shared background-color styles (screen_builders.h's
 * style_theme_screen_bg/style_theme_card_bg, or list_row_style itself for
 * "list_row") live, app-wide -- slot must be "screen", "card", or
 * "list_row" (plugin_manager.c's l_plugin_set_background_color() already
 * validates this before calling here, so this trusts its caller). rgb is
 * a packed 0xRRGGBB value. */
void gui_plugin_set_background_color(const char * slot, uint32_t rgb);

/* Same shape, for text color -- slot must be "primary" or "muted"
 * (plugin_manager.c's l_plugin_set_text_color() already validates this).
 * See screen_builders.h's style_theme_text_primary/style_theme_text_muted
 * comment for what each covers and what's deliberately excluded. */
void gui_plugin_set_text_color(const char * slot, uint32_t rgb);

/* ---- Playback control bridges for plugin.toggle_pause()/stop()/next_track()/
 * prev_track()/seek()/set_volume()/is_playing()/is_paused()/get_position()/
 * get_duration() ----
 *
 * Unlike peq.c's plugin.eq_*() bindings (which call peq_set_*()/peq_save()
 * directly from plugin_manager.c, no LVGL/gui.c state involved), these can't
 * just call audio_toggle_pause()/audio_stop()/audio_set_volume() directly --
 * gui.c's own native call sites always pair each of those with extra local
 * state (the play/pause icon, the volume slider/popup, shuffle-aware
 * next/prev stepping, DAC-mode blocking) that would otherwise go stale. Each
 * bridge below just calls the exact same local gui.c helper the native UI
 * itself already uses for that action. */

/* Calls the same local toggle_play_pause() the play/pause button's own
 * click handler uses -- includes its external_dac_block_reason() guard,
 * set_play_button_state() icon update, and last_position checkpoint. */
void gui_plugin_toggle_pause(void);

/* audio_stop() + set_play_button_state(false), guarded by audio_is_playing()
 * -- same pairing every native audio_stop() call site already uses (e.g.
 * gui.c's Bluetooth-DAC-mode-enable handler). */
void gui_plugin_stop(void);

/* Shuffle-aware next/prev -- compute_manual_step_index() + play_track_at(),
 * the same pair gui.c's own remote-control (BT/phone) consume path uses. */
void gui_plugin_next_track(void);
void gui_plugin_prev_track(void);

/* Seeks the current track to an absolute position in seconds. */
void gui_plugin_seek(double seconds);

/* Clamps to [0,100], then mirrors gui.c's own remote-control volume path:
 * audio_set_volume() + the volume_slider widget + current_settings.volume/
 * settings_save() + show_volume_popup()/refresh_volume_topbar(), so a
 * plugin-driven volume change looks identical to a hardware/remote one. */
void gui_plugin_set_volume(int percent);

/* Pure state reads -- trivial wraps of audio_is_playing()/audio_is_paused()/
 * audio_get_position_seconds()/audio_get_duration_seconds(), routed through
 * gui.c for the same "plugin_manager.c has no audio-state code of its own"
 * boundary the rest of this block keeps, even though none of these need any
 * gui.c-local state. */
bool gui_plugin_is_playing(void);
bool gui_plugin_is_paused(void);
double gui_plugin_get_position_seconds(void);
double gui_plugin_get_duration_seconds(void);

/* ---- Bridges for plugin.get_now_playing()/set_interval()/clear_interval()/
 * show_text_input() -- see plugin_manager.h's own comments on the Lua-facing
 * shape of each. ---- */

/* Fills title/artist/album (NUL-terminated, truncated to fit) and
 * *out_duration_seconds from gui.c's own cached now-playing state (the same
 * metadata already passed to a "track_started" event handler, populated at
 * the same two call sites that fire it) -- returns false (buffers/duration
 * untouched) if nothing is currently loaded. */
bool gui_plugin_get_now_playing(char * title_out, size_t title_size, char * artist_out, size_t artist_size,
                                 char * album_out, size_t album_size, double * out_duration_seconds);

/* Creates/deletes an lv_timer for plugin pool slot `slot` (0-based, into a
 * PLUGIN_MAX_INTERVALS-sized array) -- the shared timer callback reads its
 * own slot back out and calls plugin_manager_interval_fired(slot). Same
 * repeating-timer mechanism this file's own update_timer_cb() already uses
 * for its 500ms polling loop. */
void gui_plugin_set_interval(int slot, uint32_t period_ms);
void gui_plugin_clear_interval(int slot);

/* Thin wrapper around this file's own show_text_entry() singleton screen
 * (numeric=false always -- a plugin wanting a numeric-only keypad can still
 * just validate/convert the returned string itself). See
 * plugin_manager.h's own doc comment on plugin.show_text_input() for the
 * singleton/cancel-semantics caveats this inherits as-is. */
void gui_plugin_show_text_input(const char * title, const char * initial_text, bool is_password);

#endif /* GUI_H */
