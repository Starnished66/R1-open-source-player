#ifndef STB_VORBIS_HEADER_SHIM_H
#define STB_VORBIS_HEADER_SHIM_H

/* stb_vorbis.c is a single-file library that's ALSO its own complete
 * translation unit (compiled directly via the Makefile's APP_SRCS, giving
 * the real implementation exactly once) -- this shim exists so any OTHER
 * .c file that just needs the function declarations (vorbis_decoder.c) can
 * `#include "stb_vorbis.h"` without re-including (and re-compiling) the
 * ~5000-line implementation into its own translation unit, which would
 * duplicate every symbol and fail at link time. STB_VORBIS_HEADER_ONLY
 * makes stb_vorbis.c itself skip everything past its own declaration
 * section (see its own `#ifndef STB_VORBIS_HEADER_ONLY` guard). */
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

#endif /* STB_VORBIS_HEADER_SHIM_H */
