#ifndef UTF8_UTIL_H
#define UTF8_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Truncates a null-terminated UTF-8 string at a safe boundary so no multibyte
 * character is split. Returns the length in bytes of the copied string. */
size_t utf8_truncate_safe(char * dst, const char * src, size_t max_bytes);

/* Length-bounded UTF-8 truncation: examines only up to min(src_len, dst_size - 1)
 * bytes of src without performing heap allocations. Safely truncates at valid
 * character boundaries and sanitizes invalid sequences. */
size_t utf8_truncate_safe_bounded(char * dst, size_t dst_size, const char * src, size_t src_len);

/* Validates and sanitizes a UTF-8 string in place, replacing overlong encodings,
 * UTF-16 surrogates (U+D800..U+DFFF), out-of-range codepoints (> U+10FFFF),
 * and malformed continuation bytes with '?'. */
void utf8_sanitize(char * str);

#ifdef __cplusplus
}
#endif

#endif /* UTF8_UTIL_H */
