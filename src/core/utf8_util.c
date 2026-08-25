#include "utf8_util.h"
#include <string.h>

void utf8_sanitize(char * str) {
    if (!str) return;
    uint8_t * p = (uint8_t *)str;
    while (*p) {
        if (*p < 0x80) {
            /* 1-byte ASCII (0x00..0x7F) */
            p++;
        } else if (*p >= 0xC2 && *p <= 0xDF) {
            /* 2-byte sequence (U+0080..U+07FF) */
            if ((p[1] & 0xC0) == 0x80) {
                p += 2;
            } else {
                *p = '?';
                p++;
            }
        } else if (*p == 0xE0) {
            /* 3-byte sequence (U+0800..U+0FFF), reject overlongs 0x80..0x9F */
            if ((p[1] >= 0xA0 && p[1] <= 0xBF) && ((p[2] & 0xC0) == 0x80)) {
                p += 3;
            } else {
                *p = '?';
                p++;
            }
        } else if (*p >= 0xE1 && *p <= 0xEC) {
            /* 3-byte sequence (U+1000..U+CFFF) */
            if (((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80)) {
                p += 3;
            } else {
                *p = '?';
                p++;
            }
        } else if (*p == 0xED) {
            /* 3-byte sequence (U+D000..U+D7FF), reject UTF-16 surrogates U+D800..U+DFFF (0xA0..0xBF) */
            if ((p[1] >= 0x80 && p[1] <= 0x9F) && ((p[2] & 0xC0) == 0x80)) {
                p += 3;
            } else {
                *p = '?';
                p++;
            }
        } else if (*p >= 0xEE && *p <= 0xEF) {
            /* 3-byte sequence (U+E000..U+FFFF) */
            if (((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80)) {
                p += 3;
            } else {
                *p = '?';
                p++;
            }
        } else if (*p == 0xF0) {
            /* 4-byte sequence (U+10000..U+3FFFF), reject overlongs 0x80..0x8F */
            if ((p[1] >= 0x90 && p[1] <= 0xBF) && ((p[2] & 0xC0) == 0x80) && ((p[3] & 0xC0) == 0x80)) {
                p += 4;
            } else {
                *p = '?';
                p++;
            }
        } else if (*p >= 0xF1 && *p <= 0xF3) {
            /* 4-byte sequence (U+40000..U+FFFFF) */
            if (((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80) && ((p[3] & 0xC0) == 0x80)) {
                p += 4;
            } else {
                *p = '?';
                p++;
            }
        } else if (*p == 0xF4) {
            /* 4-byte sequence (U+100000..U+10FFFF), reject > U+10FFFF (0x90..0xBF) */
            if ((p[1] >= 0x80 && p[1] <= 0x8F) && ((p[2] & 0xC0) == 0x80) && ((p[3] & 0xC0) == 0x80)) {
                p += 4;
            } else {
                *p = '?';
                p++;
            }
        } else {
            /* Overlong lead (0xC0, 0xC1), stray continuation (0x80..0xBF), or invalid (0xF5..0xFF) */
            *p = '?';
            p++;
        }
    }
}

size_t utf8_truncate_safe_bounded(char * dst, size_t dst_size, const char * src, size_t src_len) {
    if (!dst || dst_size == 0) return 0;
    if (!src || src_len == 0) {
        dst[0] = '\0';
        return 0;
    }

    size_t max_copy = dst_size - 1;
    if (max_copy > src_len) {
        max_copy = src_len;
    }

    size_t cut = max_copy;
    while (cut > 0 && ((uint8_t)src[cut] & 0xC0) == 0x80) {
        cut--;
    }

    if (cut > 0) {
        uint8_t lead = (uint8_t)src[cut];
        size_t seq_len = 1;
        if (lead < 0x80) seq_len = 1;
        else if ((lead & 0xE0) == 0xC0) seq_len = 2;
        else if ((lead & 0xF0) == 0xE0) seq_len = 3;
        else if ((lead & 0xF8) == 0xF0) seq_len = 4;

        if (cut + seq_len > max_copy) {
            /* Multibyte codepoint is truncated at boundary; omit incomplete lead byte */
        } else {
            /* Entire sequence fits */
            cut += seq_len;
        }
    }

    memcpy(dst, src, cut);
    dst[cut] = '\0';
    utf8_sanitize(dst);
    return cut;
}

size_t utf8_truncate_safe(char * dst, const char * src, size_t max_bytes) {
    if (!src) return utf8_truncate_safe_bounded(dst, max_bytes, NULL, 0);
    return utf8_truncate_safe_bounded(dst, max_bytes, src, strlen(src));
}
