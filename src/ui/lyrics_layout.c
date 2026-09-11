#include "lyrics_layout.h"

#include <stdlib.h>
#include <string.h>

static int32_t fallback_stride(const lyrics_layout_t * layout) {
    return lv_font_get_line_height(layout->font) * LYRICS_MAX_LINE_BYTES + layout->row_gap;
}

static int32_t lyrics_line_glyph_overshoot(const char * text, const lv_font_t * font) {
    if (!text || !text[0] || !font) return 0;
    int32_t worst = 0;
    /* uint32_t, not size_t -- _lv_text_encoded_next()'s own signature is
     * uint32_t (*)(const char *, uint32_t *) (lv_text.h). See gui_track_info.c's
     * own comment on this exact same real correctness pitfall on a 64-bit host. */
    uint32_t i = 0;
    while (text[i] != '\0') {
        uint32_t letter = _lv_text_encoded_next(text, &i);
        if (_lv_text_is_marker(letter)) continue;
        lv_font_glyph_dsc_t g_dsc;
        if (!lv_font_get_glyph_dsc(font, &g_dsc, letter, 0)) continue;
        if (g_dsc.box_w == 0 || g_dsc.box_h == 0) continue; /* ordinary whitespace, etc. -- nothing drawn */
        int32_t overshoot = -(font->base_line + (int32_t) g_dsc.ofs_y);
        if (overshoot > worst) worst = overshoot;
    }
    return worst;
}

void lyrics_layout_destroy(lyrics_layout_t * layout) {
    if (!layout) return;
    free(layout->offsets);
    free(layout->line_space);
    memset(layout, 0, sizeof(*layout));
}

void lyrics_layout_build(lyrics_layout_t * layout, const lyrics_doc_t * doc, const lv_font_t * font,
                         int32_t row_width, int32_t row_gap, int32_t top_pad) {
    lyrics_layout_destroy(layout);
    layout->font = font;
    layout->row_width = row_width;
    layout->row_gap = row_gap;
    layout->top_pad = top_pad;
    if (!doc || doc->count <= 0) return;

    int32_t * offsets = malloc(sizeof(*offsets) * (size_t) (doc->count + 1));
    int32_t * line_space = malloc(sizeof(*line_space) * (size_t) doc->count);
    if (!offsets || !line_space) {
        free(offsets);
        free(line_space);
        return;
    }
    offsets[0] = top_pad;
    for (int i = 0; i < doc->count; i++) {
        int32_t overshoot = lyrics_line_glyph_overshoot(doc->lines[i].text, font);
        lv_point_t size;
        lv_text_get_size(&size, doc->lines[i].text, font, 0, overshoot, row_width, LV_TEXT_FLAG_NONE);
        int32_t height = size.y + overshoot;
        int32_t minimum = lv_font_get_line_height(font) + overshoot;
        if (height < minimum) height = minimum;
        line_space[i] = overshoot;
        offsets[i + 1] = offsets[i] + height + row_gap;
    }
    layout->offsets = offsets;
    layout->line_space = line_space;
    layout->count = doc->count;
}

int32_t lyrics_layout_y(const lyrics_layout_t * layout, int index) {
    if (layout->offsets && index >= 0 && index <= layout->count) return layout->offsets[index];
    return layout->top_pad + index * fallback_stride(layout);
}

int32_t lyrics_layout_height(const lyrics_layout_t * layout, int index) {
    if (layout->offsets && index >= 0 && index < layout->count) {
        return layout->offsets[index + 1] - layout->offsets[index] - layout->row_gap;
    }
    return fallback_stride(layout) - layout->row_gap;
}

int32_t lyrics_layout_line_space(const lyrics_layout_t * layout, int index) {
    if (layout->line_space && index >= 0 && index < layout->count) return layout->line_space[index];
    return 0;
}

int lyrics_layout_first_at_y(const lyrics_layout_t * layout, int32_t y, int count) {
    if (count <= 0) return 0;
    if (!layout->offsets || layout->count != count) {
        int first = y / fallback_stride(layout);
        return first < count ? first : count - 1;
    }
    int lo = 0, hi = count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (layout->offsets[mid + 1] <= y) lo = mid + 1;
        else hi = mid;
    }
    return lo < count ? lo : count - 1;
}
