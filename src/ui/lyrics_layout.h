#ifndef LYRICS_LAYOUT_H
#define LYRICS_LAYOUT_H

#include "lyrics.h"
#include "lvgl.h"
#include <stdint.h>

typedef struct {
    int32_t * offsets;
    int count;
    int32_t top_pad;
    int32_t row_width;
    int32_t row_gap;
    const lv_font_t * font;
} lyrics_layout_t;

void lyrics_layout_build(lyrics_layout_t * layout, const lyrics_doc_t * doc, const lv_font_t * font,
                         int32_t row_width, int32_t row_gap, int32_t top_pad);
void lyrics_layout_destroy(lyrics_layout_t * layout);
int32_t lyrics_layout_y(const lyrics_layout_t * layout, int index);
int32_t lyrics_layout_height(const lyrics_layout_t * layout, int index);
int lyrics_layout_first_at_y(const lyrics_layout_t * layout, int32_t y, int count);

#endif
