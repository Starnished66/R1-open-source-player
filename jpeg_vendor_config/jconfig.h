/* Hand-written replacement for libjpeg's generated jconfig.h.
 * Tailored for MIPS32 Linux / musl libc and host GCC environments.
 * Follows the Independent JPEG Group (IJG) v9f specification.
 */
#ifndef JCONFIG_H
#define JCONFIG_H

#define HAVE_PROTOTYPES 1
#define HAVE_UNSIGNED_CHAR 1
#define HAVE_UNSIGNED_SHORT 1
/* #undef void */
/* #undef const */
/* #undef CHAR_IS_UNSIGNED */
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#define HAVE_LOCALE_H 1
/* #undef NEED_BSD_STRINGS */
/* #undef NEED_SYS_TYPES_H */
/* #undef NEED_FAR_POINTERS */
/* #undef NEED_SHORT_EXTERNAL_NAMES */
/* #undef INCOMPLETE_TYPES_BROKEN */

#ifdef JPEG_INTERNALS

/* Arithmetic right shift is standard on MIPS and x86 GCC */
/* #undef RIGHT_SHIFT_IS_UNSIGNED */

#ifndef INLINE
#if defined(__GNUC__)
#define INLINE __inline__
#else
#define INLINE
#endif
#endif

/* jmemnobs is memory-only; no backing store temporary files */
#define NO_MKTEMP 1

#endif /* JPEG_INTERNALS */

#endif /* JCONFIG_H */
