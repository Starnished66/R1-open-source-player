#ifndef COVER_DECODE_H
#define COVER_DECODE_H

#include <stdbool.h>
#include <stdint.h>

/* Decodes compressed embedded-cover-art bytes (JPEG or PNG) and resizes them
 * with a "cover fit" (scale to fully fill target_w x target_h, center-
 * cropping whichever dimension overflows -- same as a photo app's cover/
 * thumbnail mode) into a newly malloc()'d RGB565 buffer the caller owns and
 * must free(). Returns false (leaving *out_pixels untouched) for an
 * unsupported format or a decode failure. */
bool cover_decode_to_rgb565(const uint8_t * data, uint32_t size, int target_w, int target_h, uint16_t ** out_pixels);

#endif /* COVER_DECODE_H */
