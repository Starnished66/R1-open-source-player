#ifndef HW_VOLUME_COALESCE_H
#define HW_VOLUME_COALESCE_H

#include <stdbool.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared "drag a volume slider" debounce, extracted from gui_player.c's
 * volume popup (the only caller before this existed) so the quick drawer's
 * own volume slider can reuse the exact same, already-proven behavior
 * instead of a second, simpler, worse copy of it -- that copy called
 * audio_request_volume() directly and untethered on every LV_EVENT_
 * VALUE_CHANGED tick, which is what made it feel sluggish. The real fix is
 * this coalescing: while dragging, only a cheap int is recorded per tick
 * (drag_update()); a 50ms timer (at most 20/s) is what actually reaches
 * audio_request_volume() -- which itself hands off to a process-lifetime
 * worker thread via a single-slot "latest wins" queue (audio.c) -- and the
 * settled value (drag_end()) is applied once, synchronously and
 * authoritatively, via audio_set_volume().
 *
 * One instance per independent slider (the Player screen's popup and the
 * quick drawer's own each get their own -- only one can be mid-drag at a
 * time since this is a single-touch UI, but there's no need to share the
 * instance itself, only this code). Caller owns the struct's storage (no
 * heap allocation); always declare it with an explicit `= { .pending = -1
 * }` initializer, not a bare zero-initialized static, since 0 is a real,
 * valid pending percentage and would otherwise look like "0% pending" on
 * a freshly loaded screen that has never been touched. */
typedef struct {
    int pending;         /* -1 = nothing pending */
    bool drag_active;
    lv_timer_t * timer;   /* created lazily on first drag_begin(), paused when idle */
} hw_volume_coalesce_t;

/* Call from LV_EVENT_PRESSED on the slider. Lazily creates the timer on
 * first use; safe to call again on a later drag. */
void hw_volume_coalesce_drag_begin(hw_volume_coalesce_t * hv);

/* Call from LV_EVENT_VALUE_CHANGED. Cheap -- records the latest value
 * only; the timer (not this call) is what actually reaches audio.c.
 * Clamped to 0-100 internally. */
void hw_volume_coalesce_drag_update(hw_volume_coalesce_t * hv, int percent);

/* Call from LV_EVENT_RELEASED/LV_EVENT_PRESS_LOST with the slider's final
 * value. Applies it synchronously via audio_set_volume() and
 * refresh_volume_topbar(percent), regardless of whatever was (or wasn't)
 * still pending, then pauses the timer. Caller is still responsible for
 * its own extra per-screen UI sync afterward (e.g. the Player screen's
 * own volume_slider widget, or the drawer's snapshot-dirty mark) and for
 * persisting settings -- this only owns the audio-facing debounce. */
void hw_volume_coalesce_drag_end(hw_volume_coalesce_t * hv, int percent);

/* An external, authoritative volume write (not from this slider) just
 * happened -- drop any pending drag sample and stop the timer without
 * applying anything (the external write already superseded it). Cannot
 * retract a value already handed to audio.c's own worker thread; that is
 * fine, since audio_set_volume()'s own generation counter (audio.h) is
 * what actually makes a later authoritative write win. */
void hw_volume_coalesce_cancel(hw_volume_coalesce_t * hv);

/* For in-process UI reload / screen teardown. If a drag was active
 * (check hv->drag_active) the caller must re-read its own slider's
 * current value and pass it to drag_update() BEFORE calling this --
 * this function cannot do that itself since it does not know which
 * widget owns the drag (this is a deliberate, reviewed limitation, not an
 * oversight: see gui_player_teardown()'s own caller-side re-read for the
 * pattern to copy). Flushes whatever ended up pending as a final,
 * synchronous apply -- audio_set_volume() + refresh_volume_topbar(),
 * exactly like drag_end(), just using hv->pending as the value instead of
 * an explicit argument, matching gui_player_teardown()'s own existing
 * behavior (its volume_hw_apply_final() call already does this same
 * refresh_volume_topbar() even at teardown, since it updates other
 * modules' widgets, not this screen's own) -- then deletes the timer and
 * resets the struct back to its initial state. A no-op (still deletes the
 * timer) when nothing was pending. */
void hw_volume_coalesce_teardown(hw_volume_coalesce_t * hv);

#ifdef __cplusplus
}
#endif

#endif /* HW_VOLUME_COALESCE_H */
