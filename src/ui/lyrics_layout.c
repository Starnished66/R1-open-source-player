#include "lyrics_layout.h"

#include <stdlib.h>
#include <string.h>

static int32_t fallback_stride(const lyrics_layout_t * layout) {
    return lv_font_get_line_height(layout->font) * LYRICS_MAX_LINE_BYTES + layout->row_gap;
}

void lyrics_layout_destroy(lyrics_layout_t * layout) {
    if (!layout) return;
    free(layout->offsets);
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
    if (!offsets) return;
    offsets[0] = top_pad;
    for (int i = 0; i < doc->count; i++) {
        lv_point_t size;
        lv_text_get_size(&size, doc->lines[i].text, font, 0, 0, row_width, LV_TEXT_FLAG_NONE);
        int32_t height = size.y;
        int32_t minimum = lv_font_get_line_height(font);
        if (height < minimum) height = minimum;
        offsets[i + 1] = offsets[i] + height + row_gap;
    }
    layout->offsets = offsets;
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
