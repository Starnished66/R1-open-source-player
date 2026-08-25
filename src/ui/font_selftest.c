#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "fallback_font.h"
#include "settings.h"
#include "subsonic_saved_servers.h"
#include "screen_builders.h"
#include "utf8_util.h"
#include "lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h"

#define HOST_SETTINGS_FILE "./open_hiby_player_settings.txt"
#define HOST_SETTINGS_TMP  "./open_hiby_player_settings.txt.tmp"

/* Mocks for standalone selftest */
extern player_settings_t current_settings;
void player_transition_mark_dirty(void) {}
void gui_lyrics_refresh_layout(void) {}
void quick_drawer_mark_snapshot_dirty(void) {}
void compact_list_refresh_all(void) {}

/* --- Test 1: UTF-8 Boundary Safe Truncation --- */
static void test_utf8_truncate_safe(void) {
    printf("Testing UTF-8 safe boundary truncation...\n");
    char buf[64];

    /* 1-byte ASCII */
    const char * ascii = "Hello World";
    size_t n = utf8_truncate_safe(buf, ascii, 6);
    assert(n == 5);
    assert(strcmp(buf, "Hello") == 0);

    /* 2-byte Cyrillic: "Привет" (12 bytes: П=2, р=2, и=2, в=2, е=2, т=2) */
    const char * cyrillic = "Привет";
    n = utf8_truncate_safe(buf, cyrillic, 5);
    assert(n == 4);
    assert(strncmp(buf, "Пр", 4) == 0);

    n = utf8_truncate_safe(buf, cyrillic, 4);
    assert(n == 2);
    assert(strncmp(buf, "П", 2) == 0);

    /* 3-byte CJK: "日本語" (9 bytes: 日=3, 本=3, 語=3) */
    const char * cjk = "日本語";
    n = utf8_truncate_safe(buf, cjk, 6);
    assert(n == 3);
    assert(strncmp(buf, "日", 3) == 0);

    n = utf8_truncate_safe(buf, cjk, 8);
    assert(n == 6);
    assert(strncmp(buf, "日本", 6) == 0);

    /* 4-byte UTF-8 Emoji: "🎵🎶" (8 bytes: 4 + 4) */
    const char * emoji = "🎵🎶";
    n = utf8_truncate_safe(buf, emoji, 4);
    assert(n == 0);
    assert(buf[0] == '\0');

    n = utf8_truncate_safe(buf, emoji, 6);
    assert(n == 4);
    assert(strncmp(buf, "🎵", 4) == 0);

    /* Length-bounded form: test with large src_len and small destination */
    const char * long_str = "Long song title that goes on and on and on";
    char small_buf[8];
    size_t bounded_n = utf8_truncate_safe_bounded(small_buf, sizeof(small_buf), long_str, 1000000);
    assert(bounded_n == 7);
    assert(strcmp(small_buf, "Long so") == 0);

    /* Non-null-terminated buffer: ensure truncation never reads beyond src_len */
    char raw_chars[5] = { 'A', 'B', 'C', 'D', 'E' };
    char dst_arr[16];
    size_t raw_n = utf8_truncate_safe_bounded(dst_arr, sizeof(dst_arr), raw_chars, 5);
    assert(raw_n == 5);
    assert(strcmp(dst_arr, "ABCDE") == 0);

    /* Incomplete multibyte at end of exact bounded buffer: Cyrillic 'П' (0xD0 0x9F) + orphan lead (0xD1) */
    char incomplete_raw[3] = { (char)0xD0, (char)0x9F, (char)0xD1 };
    raw_n = utf8_truncate_safe_bounded(dst_arr, sizeof(dst_arr), incomplete_raw, 3);
    assert(raw_n == 2);
    assert(dst_arr[2] == '\0');
    assert(strncmp(dst_arr, "П", 2) == 0);

    printf("  -> UTF-8 safe boundary truncation passed.\n");
}

/* --- Test 2: Strict UTF-8 Sanitization --- */
static void test_utf8_sanitize(void) {
    printf("Testing strict UTF-8 sanitization (RFC 3629 / Unicode standard)...\n");

    /* Valid UTF-8 strings */
    char valid[] = "Hello World! 日本語 Привет 🎵";
    utf8_sanitize(valid);
    assert(strcmp(valid, "Hello World! 日本語 Привет 🎵") == 0);

    /* Overlong 2-byte NUL (0xC0 0x80) */
    char overlong_2byte[] = "Bad \xC0\x80 char";
    utf8_sanitize(overlong_2byte);
    assert(overlong_2byte[4] == '?');
    assert(overlong_2byte[5] == '?');

    /* Overlong 2-byte (0xC1 0xBF) */
    char overlong_c1[] = "Bad \xC1\xBF char";
    utf8_sanitize(overlong_c1);
    assert(overlong_c1[4] == '?');
    assert(overlong_c1[5] == '?');

    /* Overlong 3-byte (0xE0 0x80 0xAF) */
    char overlong_3byte[] = "Bad \xE0\x80\xAF char";
    utf8_sanitize(overlong_3byte);
    assert(overlong_3byte[4] == '?');

    /* UTF-16 surrogate (0xED 0xA0 0x80 = U+D800) */
    char surrogate_d800[] = "Bad \xED\xA0\x80 char";
    utf8_sanitize(surrogate_d800);
    assert(surrogate_d800[4] == '?');

    /* UTF-16 surrogate (0xED 0xBF 0xBF = U+DFFF) */
    char surrogate_dfff[] = "Bad \xED\xBF\xBF char";
    utf8_sanitize(surrogate_dfff);
    assert(surrogate_dfff[4] == '?');

    /* Overlong 4-byte (0xF0 0x80 0x80 0x80) */
    char overlong_4byte[] = "Bad \xF0\x80\x80\x80 char";
    utf8_sanitize(overlong_4byte);
    assert(overlong_4byte[4] == '?');

    /* Codepoint > U+10FFFF (0xF4 0x90 0x80 0x80) */
    char out_of_range[] = "Bad \xF4\x90\x80\x80 char";
    utf8_sanitize(out_of_range);
    assert(out_of_range[4] == '?');

    /* Invalid start bytes (0xF5..0xFF) */
    char invalid_lead[] = "Bad \xF5\x80\x80\x80 char";
    utf8_sanitize(invalid_lead);
    assert(invalid_lead[4] == '?');

    printf("  -> Strict UTF-8 sanitization passed.\n");
}

/* --- Test 3: TinyTTF Init Idempotency --- */
static void test_tiny_ttf_init_idempotency(void) {
    printf("Testing TinyTTF init idempotency...\n");

    lv_tiny_ttf_init();
    lv_tiny_ttf_init();
    lv_tiny_ttf_init();

    lv_tiny_ttf_deinit();
    lv_tiny_ttf_init();

    printf("  -> TinyTTF init idempotency passed.\n");
}

/* --- Test 4: TrueType Outline File Validation --- */
static void test_truetype_file_validation(void) {
    printf("Testing TrueType font validation...\n");

    assert(!fallback_font_validate_file("/tmp/non_existent_font_12345.ttf"));

    FILE * f = fopen("/tmp/test_tiny.ttf", "wb");
    uint8_t zero[100] = {0};
    fwrite(zero, 1, sizeof(zero), f);
    fclose(f);
    assert(!fallback_font_validate_file("/tmp/test_tiny.ttf"));
    unlink("/tmp/test_tiny.ttf");

    f = fopen("/tmp/test_cff.ttf", "wb");
    uint8_t cff_hdr[4096] = { 'O', 'T', 'T', 'O' };
    fwrite(cff_hdr, 1, sizeof(cff_hdr), f);
    fclose(f);
    assert(!fallback_font_validate_file("/tmp/test_cff.ttf"));
    unlink("/tmp/test_cff.ttf");

    printf("  -> TrueType font validation passed.\n");
}

/* --- Test 5: Settings Custom Font Persistence & Sanitization --- */
static void test_custom_font_settings(void) {
    printf("Testing custom font settings parsing and path traversal defense...\n");
    player_settings_t s;
    memset(&s, 0, sizeof(s));

    FILE * f = fopen(HOST_SETTINGS_TMP, "w");
    fprintf(f, "custom_font=../../etc/passwd\n");
    fclose(f);
    rename(HOST_SETTINGS_TMP, HOST_SETTINGS_FILE);

    bool loaded = settings_load(&s);
    assert(loaded);
    assert(s.custom_font[0] == '\0');

    snprintf(s.custom_font, sizeof(s.custom_font), "Roboto-Regular.ttf");
    settings_save(&s);

    memset(&s, 0, sizeof(s));
    settings_load(&s);
    assert(strcmp(s.custom_font, "Roboto-Regular.ttf") == 0);

    unlink(HOST_SETTINGS_FILE);
    printf("  -> Custom font settings persistence passed.\n");
}

/* --- Test 6: Fallback Font Metrics, app_font_20, and Discovery --- */
static void test_fallback_font_lifecycle(void) {
    printf("Testing fallback font lifecycle, app_font_20, and discovery...\n");

    fallback_font_init_early(0, 2);
    assert(app_font_16.line_height == lv_font_montserrat_16.line_height);
    assert(app_font_20.line_height == lv_font_montserrat_20.line_height);
    assert(app_font_22.line_height == lv_font_montserrat_22.line_height);
    assert(app_font_28.line_height == lv_font_montserrat_28.line_height);
    assert(app_font_lyrics.line_height == lv_font_montserrat_40.line_height);

    char names[MAX_CUSTOM_FONTS_DISCOVERED][64];
    int count = fallback_font_discover_custom(names, MAX_CUSTOM_FONTS_DISCOVERED);
    assert(count >= 0);

    const char * name = fallback_font_get_custom_name();
    assert(strcmp(name, "Default") == 0);

    printf("  -> Fallback font lifecycle passed.\n");
}

/* --- Test 7: Transactional Font Switching Safety --- */
static void test_transactional_font_switching(void) {
    printf("Testing transactional font switching rollback on failure...\n");

    fallback_font_init_early(0, 2);
    uint32_t orig_16_lh = app_font_16.line_height;
    uint32_t orig_20_lh = app_font_20.line_height;
    uint32_t orig_22_lh = app_font_22.line_height;
    uint32_t orig_28_lh = app_font_28.line_height;

    /* Attempt to apply a non-existent / invalid font */
    bool applied = fallback_font_apply_custom("non_existent_fake_font_123.ttf");
    assert(!applied);

    /* Assert that previous font handles are completely intact */
    assert(app_font_16.line_height == orig_16_lh);
    assert(app_font_20.line_height == orig_20_lh);
    assert(app_font_22.line_height == orig_22_lh);
    assert(app_font_28.line_height == orig_28_lh);
    assert(strcmp(fallback_font_get_custom_name(), "Default") == 0);

    /* Hotplug callback test with empty/default config */
    memset(&current_settings, 0, sizeof(current_settings));
    fallback_font_on_sd_mounted();
    assert(strcmp(fallback_font_get_custom_name(), "Default") == 0);

    printf("  -> Transactional font switching safety passed.\n");
}

int main(void) {
    printf("=== Starting Font Subsystem Self-Tests ===\n");
    test_utf8_truncate_safe();
    test_utf8_sanitize();
    test_tiny_ttf_init_idempotency();
    test_truetype_file_validation();
    test_custom_font_settings();
    test_fallback_font_lifecycle();
    test_transactional_font_switching();
    printf("=== ALL FONT SELF-TESTS PASSED ===\n");
    return 0;
}
