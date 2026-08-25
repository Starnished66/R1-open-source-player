#include "fallback_font.h"
#include "settings.h"
#include "debug_log.h"
#include "gui_lyrics.h"
#include "screen_builders.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <utime.h>
#include <ctype.h>
#include <limits.h>

lv_font_t app_font_16;
lv_font_t app_font_20;
lv_font_t app_font_22;
lv_font_t app_font_28;
lv_font_t app_font_lyrics;

#ifdef HOST_BUILD
  #define FALLBACK_FONT_ROOT "assets/fonts/"
  #define FALLBACK_FONT_FILE FALLBACK_FONT_ROOT "cjk_cyrillic.ttf"
  #define KOREAN_FONT_FILE FALLBACK_FONT_ROOT "korean.ttf"
  #define THAI_FONT_FILE FALLBACK_FONT_ROOT "thai.ttf"
  #define CUSTOM_FONT_SD_DIR "music/Fonts"
  #define CUSTOM_FONT_ALT_SD_DIR "Fonts"
  #define STAGING_DIR "build_host/fonts"
  #define STAGING_CUSTOM_FILE "build_host/fonts/custom_staged.ttf"
  #define STAGING_TMP_FILE "build_host/fonts/custom_staged.tmp"
#else
  #define FALLBACK_FONT_FILE "/usr/resource/fonts/cjk_cyrillic.ttf"
  #define KOREAN_FONT_FILE "/usr/resource/fonts/Korean.ttf"
  #define THAI_FONT_FILE "/usr/resource/fonts/Thai.ttf"
  #define CUSTOM_FONT_SD_DIR "/data/mnt/sd_0/Fonts"
  #define CUSTOM_FONT_ALT_SD_DIR "/data/mnt/sd_0/fonts"
  #define STAGING_DIR "/data/app_data/fonts"
  #define STAGING_CUSTOM_FILE "/data/app_data/fonts/custom_staged.ttf"
  #define STAGING_TMP_FILE "/data/app_data/fonts/custom_staged.tmp"
#endif

extern player_settings_t current_settings;
extern void settings_save(const player_settings_t * s);
extern void player_transition_mark_dirty(void);

#ifndef HOST_BUILD
extern void boot_checkpoint(const char * step);
#endif

static int s_font_size_tier = 0;
static int s_lyrics_font_size_tier = 2;
static char s_active_custom_name[64] = "";
static bool s_custom_staged_valid = false;
static uint32_t s_custom_font_generation = 0;
static bool s_fallback_loaded = false;

typedef enum {
    FACE_SRC_CUSTOM = 0,
    FACE_SRC_CJK,
    FACE_SRC_KOREAN,
    FACE_SRC_THAI,
    FACE_SRC_COUNT
} face_source_type_t;

typedef struct {
    face_source_type_t type;
    int pixel_size;
    uint32_t custom_generation;
    lv_font_t * font;
} loaded_face_entry_t;

#define MAX_LOADED_FACES 32
static loaded_face_entry_t s_loaded_faces[MAX_LOADED_FACES];
static int s_loaded_face_count = 0;

typedef struct {
    loaded_face_entry_t entries[MAX_LOADED_FACES];
    int count;
} face_table_t;

static void tier_pixel_sizes(int tier, int * out_16_slot, int * out_20_slot, int * out_22_slot, int * out_28_slot) {
    switch (tier) {
        case 1: /* Medium */
            *out_16_slot = 20;
            *out_20_slot = 24;
            *out_22_slot = 26;
            *out_28_slot = 32;
            break;
        case 2: /* Large / BlindMF */
            *out_16_slot = 24;
            *out_20_slot = 30;
            *out_22_slot = 34;
            *out_28_slot = 40;
            break;
        default: /* Small (0) */
            *out_16_slot = 16;
            *out_20_slot = 20;
            *out_22_slot = 22;
            *out_28_slot = 28;
            break;
    }
}

static const lv_font_t * get_montserrat_font_for_px(int px) {
    switch (px) {
        case 16: return &lv_font_montserrat_16;
        case 20: return &lv_font_montserrat_20;
        case 22: return &lv_font_montserrat_22;
        case 24: return &lv_font_montserrat_24;
        case 26: return &lv_font_montserrat_26;
        case 28: return &lv_font_montserrat_28;
        case 30: return &lv_font_montserrat_30;
        case 32: return &lv_font_montserrat_32;
        case 34: return &lv_font_montserrat_34;
        case 40: return &lv_font_montserrat_40;
        default: return &lv_font_montserrat_20;
    }
}

static void font_chain_append(lv_font_t ** head, lv_font_t * font) {
    if (!font) return;
    font->fallback = NULL;
    if (!*head) {
        *head = font;
        return;
    }
    lv_font_t * curr = *head;
    while (curr->fallback) {
        if (curr == font || curr->fallback == font) return; /* prevent cycles */
        curr = (lv_font_t *) curr->fallback;
    }
    if (curr != font) {
        curr->fallback = font;
    }
}

static const char * face_source_file_path(face_source_type_t type, bool custom_staged) {
    switch (type) {
        case FACE_SRC_CUSTOM:
            return custom_staged ? STAGING_CUSTOM_FILE : NULL;
        case FACE_SRC_CJK:
            return FALLBACK_FONT_FILE;
        case FACE_SRC_KOREAN:
            return KOREAN_FONT_FILE;
        case FACE_SRC_THAI:
            return THAI_FONT_FILE;
        default:
            return NULL;
    }
}

static lv_font_t * load_face_from_file(face_source_type_t type, int pixel_size, bool custom_staged) {
    const char * filepath = face_source_file_path(type, custom_staged);
    if (!filepath) return NULL;

    struct stat st;
    if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) {
        return NULL;
    }

    char lv_path[PATH_MAX + 4];
    snprintf(lv_path, sizeof(lv_path), "S:%s", filepath);
    lv_font_t * font = lv_tiny_ttf_create_file(lv_path, pixel_size);
    if (!font) {
        DBG_LOG("fallback_font: failed loading face type=%d size=%d from %s\n", (int)type, pixel_size, filepath);
        return NULL;
    }
    return font;
}

static lv_font_t * get_or_load_face_in_table(face_table_t * tbl, face_source_type_t type, int pixel_size, bool custom_staged, uint32_t custom_gen) {
    if (!tbl) return NULL;
    for (int i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].type == type && tbl->entries[i].pixel_size == pixel_size) {
            return tbl->entries[i].font;
        }
    }
    if (tbl->count >= MAX_LOADED_FACES) {
        DBG_LOG("fallback_font: max loaded faces reached in candidate table (%d)\n", MAX_LOADED_FACES);
        return NULL;
    }

    /* Reuse from existing s_loaded_faces if matching and unmodified */
    lv_font_t * existing_font = NULL;
    for (int i = 0; i < s_loaded_face_count; i++) {
        if (s_loaded_faces[i].type == type && s_loaded_faces[i].pixel_size == pixel_size) {
            if (type == FACE_SRC_CUSTOM) {
                /* Only reuse if custom staging state and generation match exactly */
                if (custom_staged && s_custom_staged_valid && s_loaded_faces[i].custom_generation == custom_gen) {
                    existing_font = s_loaded_faces[i].font;
                    break;
                }
            } else {
                existing_font = s_loaded_faces[i].font;
                break;
            }
        }
    }

    lv_font_t * font = existing_font ? existing_font : load_face_from_file(type, pixel_size, custom_staged);
    if (!font) {
        return NULL;
    }

    tbl->entries[tbl->count].type = type;
    tbl->entries[tbl->count].pixel_size = pixel_size;
    tbl->entries[tbl->count].custom_generation = (type == FACE_SRC_CUSTOM) ? custom_gen : 0;
    tbl->entries[tbl->count].font = font;
    tbl->count++;
    return font;
}

static bool build_candidate_slot(face_table_t * tbl, lv_font_t * out_font, int pixel_size, bool custom_staged, uint32_t custom_gen, bool include_fallbacks) {
    lv_font_t * primary_custom = NULL;
    if (custom_staged) {
        primary_custom = get_or_load_face_in_table(tbl, FACE_SRC_CUSTOM, pixel_size, custom_staged, custom_gen);
        if (!primary_custom) {
            DBG_LOG("fallback_font: failed loading custom face at %d px\n", pixel_size);
            return false;
        }
    }

    if (primary_custom) {
        *out_font = *primary_custom;
    } else {
        const lv_font_t * mont = get_montserrat_font_for_px(pixel_size);
        *out_font = *mont;
    }
    out_font->fallback = NULL;

    if (include_fallbacks) {
        lv_font_t * cjk = get_or_load_face_in_table(tbl, FACE_SRC_CJK, pixel_size, custom_staged, custom_gen);
        lv_font_t * kr = get_or_load_face_in_table(tbl, FACE_SRC_KOREAN, pixel_size, custom_staged, custom_gen);
        lv_font_t * th = get_or_load_face_in_table(tbl, FACE_SRC_THAI, pixel_size, custom_staged, custom_gen);

        lv_font_t * fallback_head = NULL;
        if (cjk) font_chain_append(&fallback_head, cjk);
        if (kr)  font_chain_append(&fallback_head, kr);
        if (th)  font_chain_append(&fallback_head, th);

        out_font->fallback = fallback_head;
    }
    return true;
}

bool fallback_font_validate_file(const char * path) {
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (st.st_size < 4096 || st.st_size > MAX_CUSTOM_FONT_FILE_SIZE) return false;

    FILE * f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[4];
    if (fread(hdr, 1, 4, f) != 4) { fclose(f); return false; }
    fclose(f);

    uint32_t magic = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                     ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
    if (magic != 0x00010000 && magic != 0x74727565) {
        DBG_LOG("fallback_font: invalid TTF magic 0x%08X in %s\n", (unsigned int)magic, path);
        return false;
    }

    char lv_path[PATH_MAX + 4];
    snprintf(lv_path, sizeof(lv_path), "S:%s", path);

    /* Validate test loads and metrics at all tier pixel sizes */
    static const int s_test_sizes[] = { 16, 20, 22, 24, 26, 28, 30, 32, 34, 40 };
    lv_font_t * test_fonts[sizeof(s_test_sizes)/sizeof(s_test_sizes[0])] = {0};
    bool valid = true;

    for (size_t s = 0; s < sizeof(s_test_sizes)/sizeof(s_test_sizes[0]); s++) {
        int px = s_test_sizes[s];
        lv_font_t * tf = lv_tiny_ttf_create_file(lv_path, px);
        if (!tf) {
            DBG_LOG("fallback_font: test create failed at size %d for %s\n", px, path);
            valid = false;
            break;
        }
        test_fonts[s] = tf;

        int32_t lh = lv_font_get_line_height(tf);
        if (lh <= 0 || lh > 150) {
            DBG_LOG("fallback_font: invalid line height %d at size %d for %s\n", (int)lh, px, path);
            valid = false;
            break;
        }

        /* Check full printable Latin ASCII coverage (0x20 ' ' through 0x7E '~') */
        for (uint32_t cp = 0x20; cp <= 0x7E; cp++) {
            lv_font_glyph_dsc_t g_dsc;
            if (!lv_font_get_glyph_dsc(tf, &g_dsc, cp, 0)) {
                DBG_LOG("fallback_font: missing Latin glyph 0x%02X ('%c') at size %d in %s\n",
                        (unsigned int)cp, (char)cp, px, path);
                valid = false;
                break;
            }
        }
        if (!valid) break;
    }

    for (size_t s = 0; s < sizeof(s_test_sizes)/sizeof(s_test_sizes[0]); s++) {
        if (test_fonts[s]) {
            lv_tiny_ttf_destroy(test_fonts[s]);
        }
    }
    return valid;
}

static bool stage_custom_font_from_sd(const char * custom_filename) {
    if (!custom_filename || !custom_filename[0] || strcmp(custom_filename, "Default") == 0) {
        return true;
    }

    if (strchr(custom_filename, '/') || strchr(custom_filename, '\\') || strstr(custom_filename, "..")) {
        DBG_LOG("fallback_font: rejected unsafe custom font filename: %s\n", custom_filename);
        return false;
    }

    char src_path[PATH_MAX];
    snprintf(src_path, sizeof(src_path), "%s/%s", CUSTOM_FONT_SD_DIR, custom_filename);
    struct stat st;
    if (stat(src_path, &st) != 0) {
        snprintf(src_path, sizeof(src_path), "%s/%s", CUSTOM_FONT_ALT_SD_DIR, custom_filename);
        if (stat(src_path, &st) != 0) {
            DBG_LOG("fallback_font: custom font source not found: %s\n", custom_filename);
            return false;
        }
    }

    if (!S_ISREG(st.st_mode) || st.st_size < 4096 || st.st_size > MAX_CUSTOM_FONT_FILE_SIZE) {
        DBG_LOG("fallback_font: invalid custom font size %lld for %s\n", (long long)st.st_size, src_path);
        return false;
    }

#ifdef HOST_BUILD
    mkdir("build_host", 0755);
#else
    mkdir("/data", 0755);
    mkdir("/data/app_data", 0755);
#endif
    mkdir(STAGING_DIR, 0755);

    struct stat st_staged;
    if (stat(STAGING_CUSTOM_FILE, &st_staged) == 0 &&
        st_staged.st_size == st.st_size &&
        st_staged.st_mtime == st.st_mtime &&
        fallback_font_validate_file(STAGING_CUSTOM_FILE)) {
        return true;
    }

    struct statvfs sv;
    if (statvfs(STAGING_DIR, &sv) == 0) {
        uint64_t free_bytes = (uint64_t)sv.f_bavail * (uint64_t)sv.f_bsize;
        if (free_bytes < (uint64_t)st.st_size * 2 + 65536) {
            DBG_LOG("fallback_font: insufficient disk space for font staging (%llu free)\n", (unsigned long long)free_bytes);
            return false;
        }
    }

    int fd_src = open(src_path, O_RDONLY);
    if (fd_src < 0) return false;

    unlink(STAGING_TMP_FILE);
    int fd_dst = open(STAGING_TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_dst < 0) {
        close(fd_src);
        return false;
    }

    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(fd_src, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(fd_dst, buf + written, (size_t)(n - written));
            if (w <= 0) { ok = false; break; }
            written += w;
        }
        if (!ok) break;
    }
    if (n < 0) ok = false;

    if (ok) {
        if (fsync(fd_dst) != 0) ok = false;
    }
    close(fd_src);
    if (close(fd_dst) != 0) ok = false;

    if (!ok) {
        unlink(STAGING_TMP_FILE);
        DBG_LOG("fallback_font: copy/sync error while staging %s\n", custom_filename);
        return false;
    }

    struct utimbuf ut = { .actime = st.st_atime, .modtime = st.st_mtime };
    utime(STAGING_TMP_FILE, &ut);

    if (!fallback_font_validate_file(STAGING_TMP_FILE)) {
        unlink(STAGING_TMP_FILE);
        DBG_LOG("fallback_font: validation failed for staged %s\n", custom_filename);
        return false;
    }

    if (rename(STAGING_TMP_FILE, STAGING_CUSTOM_FILE) != 0) {
        unlink(STAGING_TMP_FILE);
        return false;
    }

    int dir_fd = open(STAGING_DIR, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    return true;
}

static int compare_font_names(const void * a, const void * b) {
    return strcasecmp((const char *)a, (const char *)b);
}

int fallback_font_discover_custom(char out_names[][64], int max_count) {
    if (!out_names || max_count <= 0) return 0;
    int count = 0;

    const char * dirs[] = { CUSTOM_FONT_SD_DIR, CUSTOM_FONT_ALT_SD_DIR };
    for (size_t d = 0; d < sizeof(dirs)/sizeof(dirs[0]); d++) {
        DIR * dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent * entry;
        while ((entry = readdir(dir)) != NULL && count < max_count) {
            if (entry->d_name[0] == '.') continue;
            size_t len = strlen(entry->d_name);
            if (len < 5 || len >= 64) continue; /* Reject if does not fit in 64-byte buffer */
            if (strcasecmp(entry->d_name + len - 4, ".ttf") != 0) continue;

            /* Verify filename contains only valid safe characters */
            bool safe = true;
            for (size_t i = 0; i < len; i++) {
                char c = entry->d_name[i];
                if (c < 32 || c > 126 || c == '/' || c == '\\') {
                    safe = false;
                    break;
                }
            }
            if (!safe) continue;

            bool duplicate = false;
            for (int i = 0; i < count; i++) {
                if (strcasecmp(out_names[i], entry->d_name) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "%s/%s", dirs[d], entry->d_name);
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size <= MAX_CUSTOM_FONT_FILE_SIZE) {
                memcpy(out_names[count], entry->d_name, len);
                out_names[count][len] = '\0';
                count++;
            }
        }
        closedir(dir);
    }

    if (count > 1) {
        qsort(out_names, (size_t)count, 64, compare_font_names);
    }
    return count;
}

const char * fallback_font_get_custom_name(void) {
    if (s_custom_staged_valid && s_active_custom_name[0]) {
        return s_active_custom_name;
    }
    return "Default";
}

void fallback_font_refresh_ui(void) {
    lv_obj_t * act = lv_screen_active();
    if (act) lv_obj_invalidate(act);
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());

    compact_list_refresh_all();
    quick_drawer_mark_snapshot_dirty();
    gui_lyrics_refresh_layout();
    player_transition_mark_dirty();
}

bool fallback_font_apply_custom(const char * custom_filename) {
    bool is_default = (!custom_filename || !custom_filename[0] || strcmp(custom_filename, "Default") == 0);
    bool candidate_staged_valid = false;
    uint32_t target_gen = s_custom_font_generation;

    if (!is_default) {
        if (!stage_custom_font_from_sd(custom_filename)) {
            DBG_LOG("fallback_font: failed staging custom font %s\n", custom_filename);
            return false;
        }
        candidate_staged_valid = true;
        /* If custom font name changed, increment generation to prevent reusing old font's faces */
        if (!s_custom_staged_valid || strcmp(s_active_custom_name, custom_filename) != 0) {
            target_gen++;
        }
    } else {
        candidate_staged_valid = false;
    }

    int size_16, size_20, size_22, size_28;
    tier_pixel_sizes(s_font_size_tier, &size_16, &size_20, &size_22, &size_28);
    int size_lyrics = (s_lyrics_font_size_tier == 1) ? 32 : 40;

    /* Build candidate faces in separate table to ensure 100% transactional safety */
    face_table_t candidate_table = {0};
    lv_font_t cand_16, cand_20, cand_22, cand_28, cand_lyrics;

    bool ok = true;
    ok = ok && build_candidate_slot(&candidate_table, &cand_16, size_16, candidate_staged_valid, target_gen, s_fallback_loaded);
    ok = ok && build_candidate_slot(&candidate_table, &cand_20, size_20, candidate_staged_valid, target_gen, s_fallback_loaded);
    ok = ok && build_candidate_slot(&candidate_table, &cand_22, size_22, candidate_staged_valid, target_gen, s_fallback_loaded);
    ok = ok && build_candidate_slot(&candidate_table, &cand_28, size_28, candidate_staged_valid, target_gen, s_fallback_loaded);
    ok = ok && build_candidate_slot(&candidate_table, &cand_lyrics, size_lyrics, candidate_staged_valid, target_gen, s_fallback_loaded);

    if (!ok) {
        DBG_LOG("fallback_font: candidate build failed; rolling back without touching active font stack\n");
        /* Destroy newly created faces in candidate table that were not part of s_loaded_faces */
        for (int i = 0; i < candidate_table.count; i++) {
            lv_font_t * cand_font = candidate_table.entries[i].font;
            bool was_in_old = false;
            for (int j = 0; j < s_loaded_face_count; j++) {
                if (s_loaded_faces[j].font == cand_font) { was_in_old = true; break; }
            }
            if (!was_in_old && cand_font) {
                lv_tiny_ttf_destroy(cand_font);
            }
        }
        return false;
    }

    /* All candidate slots built successfully. Destroy faces in s_loaded_faces that are no longer used. */
    for (int j = 0; j < s_loaded_face_count; j++) {
        lv_font_t * old_font = s_loaded_faces[j].font;
        bool in_new = false;
        for (int i = 0; i < candidate_table.count; i++) {
            if (candidate_table.entries[i].font == old_font) { in_new = true; break; }
        }
        if (!in_new && old_font) {
            lv_tiny_ttf_destroy(old_font);
        }
    }

    /* Atomic commit of font descriptors to global handles */
    app_font_16 = cand_16;
    app_font_20 = cand_20;
    app_font_22 = cand_22;
    app_font_28 = cand_28;
    app_font_lyrics = cand_lyrics;

    s_loaded_face_count = candidate_table.count;
    for (int i = 0; i < candidate_table.count; i++) {
        s_loaded_faces[i] = candidate_table.entries[i];
    }

    s_custom_font_generation = target_gen;
    s_custom_staged_valid = candidate_staged_valid;
    if (is_default) {
        s_active_custom_name[0] = '\0';
    } else {
        utf8_truncate_safe(s_active_custom_name, custom_filename, sizeof(s_active_custom_name));
    }

    fallback_font_refresh_ui();
    return true;
}

void fallback_font_on_sd_mounted(void) {
    if (current_settings.custom_font[0] == '\0' || strcmp(current_settings.custom_font, "Default") == 0) {
        return;
    }
    if (s_custom_staged_valid && strcmp(s_active_custom_name, current_settings.custom_font) == 0) {
        return;
    }
    DBG_LOG("fallback_font: SD card mounted, retrying configured font: %s\n", current_settings.custom_font);
    fallback_font_apply_custom(current_settings.custom_font);
}

void fallback_font_init_early(int font_size_tier, int lyrics_font_size_tier) {
    s_font_size_tier = font_size_tier;
    s_lyrics_font_size_tier = lyrics_font_size_tier;

    unlink(STAGING_TMP_FILE);

    bool staged = false;
    if (current_settings.custom_font[0] && strcmp(current_settings.custom_font, "Default") != 0) {
        staged = stage_custom_font_from_sd(current_settings.custom_font);
        if (staged) {
            s_custom_staged_valid = true;
            s_custom_font_generation++;
            utf8_truncate_safe(s_active_custom_name, current_settings.custom_font, sizeof(s_active_custom_name));
        }
    }

    int size_16, size_20, size_22, size_28;
    tier_pixel_sizes(font_size_tier, &size_16, &size_20, &size_22, &size_28);
    int size_lyrics = (lyrics_font_size_tier == 1) ? 32 : 40;

    face_table_t init_table = {0};
    build_candidate_slot(&init_table, &app_font_16, size_16, s_custom_staged_valid, s_custom_font_generation, false);
    build_candidate_slot(&init_table, &app_font_20, size_20, s_custom_staged_valid, s_custom_font_generation, false);
    build_candidate_slot(&init_table, &app_font_22, size_22, s_custom_staged_valid, s_custom_font_generation, false);
    build_candidate_slot(&init_table, &app_font_28, size_28, s_custom_staged_valid, s_custom_font_generation, false);
    build_candidate_slot(&init_table, &app_font_lyrics, size_lyrics, s_custom_staged_valid, s_custom_font_generation, false);

    s_loaded_face_count = init_table.count;
    for (int i = 0; i < init_table.count; i++) {
        s_loaded_faces[i] = init_table.entries[i];
    }
}

void fallback_font_load_now(void) {
    s_fallback_loaded = true;

    int size_16, size_20, size_22, size_28;
    tier_pixel_sizes(s_font_size_tier, &size_16, &size_20, &size_22, &size_28);
    int size_lyrics = (s_lyrics_font_size_tier == 1) ? 32 : 40;

    face_table_t table = {0};
    lv_font_t cand_16, cand_20, cand_22, cand_28, cand_lyrics;

    build_candidate_slot(&table, &cand_16, size_16, s_custom_staged_valid, s_custom_font_generation, true);
    build_candidate_slot(&table, &cand_20, size_20, s_custom_staged_valid, s_custom_font_generation, true);
    build_candidate_slot(&table, &cand_22, size_22, s_custom_staged_valid, s_custom_font_generation, true);
    build_candidate_slot(&table, &cand_28, size_28, s_custom_staged_valid, s_custom_font_generation, true);
    build_candidate_slot(&table, &cand_lyrics, size_lyrics, s_custom_staged_valid, s_custom_font_generation, true);

    /* Atomic swap */
    app_font_16 = cand_16;
    app_font_20 = cand_20;
    app_font_22 = cand_22;
    app_font_28 = cand_28;
    app_font_lyrics = cand_lyrics;

    s_loaded_face_count = table.count;
    for (int i = 0; i < table.count; i++) {
        s_loaded_faces[i] = table.entries[i];
    }

    fallback_font_refresh_ui();
}

static void fallback_font_load_deferred(lv_timer_t * timer) {
    lv_timer_delete(timer);

#ifndef HOST_BUILD
    boot_checkpoint("fallback_font_load_deferred entered");
#endif
    DBG_LOG("fallback_font: loading multilingual fallbacks (tier=%d)\n", s_font_size_tier);
    fallback_font_load_now();
#ifndef HOST_BUILD
    boot_checkpoint("fallback_font_load_deferred done");
#endif
}

#define FALLBACK_FONT_LOAD_DELAY_MS 500
void fallback_font_schedule_deferred_load(void) {
    lv_timer_create(fallback_font_load_deferred, FALLBACK_FONT_LOAD_DELAY_MS, NULL);
}
