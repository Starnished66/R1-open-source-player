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

/* ---- Bridge for src/plugins/plugin_manager.c ----
 * plugin_manager.c owns Lua state/script lifecycle and has no LVGL code of
 * its own; these let a plugin's C API calls (plugin.show_list/play_file/
 * play_list/show_toast, see plugin_manager.c's register_plugin_api()) reach
 * gui.c's existing screens/playback plumbing instead of duplicating it. */

/* Repopulates and pushes a shared plugin-list screen (one of a small
 * reusable pool -- see gui.c's own comment on why a pool rather than a
 * single shared screen) showing `labels[0..count)` as plain tappable rows.
 * Tapping row i calls plugin_manager_list_item_selected(i). */
void gui_plugin_show_list(const char * title, const char * const * labels, int count);

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

#endif /* GUI_H */
