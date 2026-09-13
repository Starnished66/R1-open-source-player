#ifndef PLUGIN_INTERNAL_H
#define PLUGIN_INTERNAL_H

#include <stdbool.h>
#include <stdio.h>

/* Shared plugin ID character-set validation:
 * Enforces [A-Za-z0-9._-] and non-empty. Does not enforce length limits,
 * as callers enforce differing maximum lengths (e.g. PLUGIN_STORAGE_ID_MAX 63
 * in plugin_storage.c, 63 in l_plugin_define, 39 in l_plugin_register_home_tile). */
static inline bool plugin_id_charset_ok(const char * id) {
    if (!id || !id[0]) return false;
    for (const unsigned char * p = (const unsigned char *) id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) return false;
    }
    return true;
}

/* Shared MD5 digest to lowercase hex string formatter:
 * Takes 16-byte raw MD5 digest and writes 32 hex characters plus NUL terminator
 * into out (buffer must be at least 33 bytes). */
static inline void md5_digest_to_hex(const unsigned char digest[16], char out[33]) {
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
}

#endif /* PLUGIN_INTERNAL_H */
