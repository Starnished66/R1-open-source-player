#include "hw_volume_coalesce.h"
#include "audio.h"
#include "gui_shell.h"

/* Coalesce live volume feedback to at most 20 worker requests per second
 * -- same interval/reasoning as the original, single-caller implementation
 * this was extracted from. */
#define HW_VOLUME_COALESCE_INTERVAL_MS 50

static int hw_volume_coalesce_clamp(int percent) {
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

static void hw_volume_coalesce_timer_cb(lv_timer_t * timer) {
    hw_volume_coalesce_t * hv = (hw_volume_coalesce_t *) lv_timer_get_user_data(timer);
    if (hv->pending >= 0) {
        int pending = hv->pending;
        hv->pending = -1;
        audio_request_volume((float) pending / 100.0f);
    }
    if (!hv->drag_active && hv->pending < 0) lv_timer_pause(hv->timer);
}

void hw_volume_coalesce_drag_begin(hw_volume_coalesce_t * hv) {
    hv->drag_active = true;
    if (!hv->timer) {
        hv->timer = lv_timer_create(hw_volume_coalesce_timer_cb, HW_VOLUME_COALESCE_INTERVAL_MS, hv);
        if (hv->timer) lv_timer_pause(hv->timer);
    }
    if (!hv->timer) return; /* allocation failure -- degrades to no coalescing, not a crash */
    lv_timer_reset(hv->timer);
    lv_timer_resume(hv->timer);
}

void hw_volume_coalesce_drag_update(hw_volume_coalesce_t * hv, int percent) {
    hv->pending = hw_volume_coalesce_clamp(percent);
}

void hw_volume_coalesce_drag_end(hw_volume_coalesce_t * hv, int percent) {
    percent = hw_volume_coalesce_clamp(percent);
    hv->pending = -1;
    hv->drag_active = false;
    audio_set_volume((float) percent / 100.0f);
    refresh_volume_topbar(percent);
    if (hv->timer) lv_timer_pause(hv->timer);
}

void hw_volume_coalesce_cancel(hw_volume_coalesce_t * hv) {
    hv->pending = -1;
    hv->drag_active = false;
    if (hv->timer) lv_timer_pause(hv->timer);
}

void hw_volume_coalesce_teardown(hw_volume_coalesce_t * hv) {
    if (hv->pending >= 0) {
        int pending = hv->pending;
        hv->pending = -1;
        audio_set_volume((float) pending / 100.0f);
        refresh_volume_topbar(pending);
    }
    hv->drag_active = false;
    if (hv->timer) {
        lv_timer_delete(hv->timer);
        hv->timer = NULL;
    }
}
