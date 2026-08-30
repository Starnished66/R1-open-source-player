#include "gui_text_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "assets.h"
#include "gui.h"
#include <ctype.h>

extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_secondary;
extern lv_style_t style_theme_row;
extern lv_style_t style_theme_text_muted;
extern lv_style_t style_theme_list_padding;
extern lv_style_t style_button_pressed;

extern void nav_remove_stack_slot(int depth);
extern void enable_gesture_bubble_recursive(lv_obj_t * parent);
extern void search_textarea_value_changed_cb(lv_event_t * e);
#define STATUS_BAR_CLEARANCE 40
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_reset_to_home(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void generic_back_cb(lv_event_t * e);
extern void plugin_manager_text_input_cancelled(void);

/* gui_font_role_t defined in gui_theme.h */

extern void plugin_text_entry_done_cb(const char * text, void * user_data);

static lv_obj_t * text_entry_screen;
static lv_obj_t * text_entry_title_label;
static lv_obj_t * text_entry_textarea;
static lv_obj_t * text_entry_keypad_group; /* wraps every T9 key -- reparented onto whichever screen has inline search open, see t9_keypad_attach()/t9_keypad_release() */
/* Enter's meaning while the keypad is on loan for inline search: dismiss
 * just the keypad (search stays active) instead of text_entry_commit()'s
 * normal nav_pop()-and-fire-callback modal flow -- see
 * text_entry_enter_click_cb(). */
static bool text_entry_inline_mode_active = false;
static lv_obj_t * text_entry_reveal_btn;
static text_entry_done_cb_t text_entry_on_done;
static void * text_entry_user_data;

/* ---- T9 keypad geometry: 5 columns x 4 rows of TEXT_ENTRY_KEY_SIZE keys,
 * centered under the 480px-wide screen and anchored to the bottom of the
 * 800px-tall screen (both hardcoded directly rather than shared from
 * SCREEN_WIDTH/SCREEN_HEIGHT constants -- main.c's copies aren't visible
 * here, same as everywhere else in this file that needs the screen's own
 * pixel size, e.g. COVER_ART_WIDTH above). Matches a real-device photo of
 * the stock keyboard exactly (not asset-name guessing -- an earlier 4x5
 * layout with a single cycling Mode key was wrong): column 0 holds three
 * always-visible mode-jump buttons (123/ABC/sym) stacked vertically rather
 * than one key that cycles between them; columns 1-3 hold the actual T9
 * 3x3 letter/digit/symbol pad; column 4 holds Del (row 0), the key-0/Shift
 * slot (row 1, see text_entry_key0_click_cb's own comment), and Enter
 * (rows 2-3, tall). Row 3's remaining cells are Left/Right and a wide
 * Space spanning columns 2-3. 5 columns at the native 94px key size only
 * just fits 480px wide (a 2px gap leaves a 1px margin each side) -- real
 * device photo confirms the stock keyboard packs this tight too. ---- */
#define TEXT_ENTRY_KEY_SIZE 94
#define TEXT_ENTRY_KEY_GAP 2
#define TEXT_ENTRY_GRID_COLS 5
#define TEXT_ENTRY_GRID_ROWS 4
#define TEXT_ENTRY_GRID_WIDTH (TEXT_ENTRY_GRID_COLS * TEXT_ENTRY_KEY_SIZE + (TEXT_ENTRY_GRID_COLS - 1) * TEXT_ENTRY_KEY_GAP)
#define TEXT_ENTRY_GRID_HEIGHT (TEXT_ENTRY_GRID_ROWS * TEXT_ENTRY_KEY_SIZE + (TEXT_ENTRY_GRID_ROWS - 1) * TEXT_ENTRY_KEY_GAP)
#define TEXT_ENTRY_BOTTOM_MARGIN 16
#define TEXT_ENTRY_GRID_X ((480 - TEXT_ENTRY_GRID_WIDTH) / 2)
#define TEXT_ENTRY_GRID_Y (800 - TEXT_ENTRY_GRID_HEIGHT - TEXT_ENTRY_BOTTOM_MARGIN)
#define TEXT_ENTRY_MULTITAP_MS 900

typedef enum {
    TEXT_ENTRY_KP_ABC = 0,
    TEXT_ENTRY_KP_NUM,
    TEXT_ENTRY_KP_SYM,
} text_entry_kp_mode_t;

/* Key index 10 is the numeric-lock-only "./-" key (T9_LETTER_GROUPS/
 * T9_SYMBOL_GROUPS below only cover 0-9, the real digit keys). NULL entries
 * mean "no letter group on this asset set" (keys 0 and 1) -- those always
 * insert their own literal digit in ABC mode, same as NUM mode does for
 * every key. */
static const char * const TEXT_ENTRY_LETTER_GROUPS[10] = {
    NULL, NULL, "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"
};
/* Read off symbolN.png's own rendered glyphs directly (contact-sheet
 * inspection, see this feature's own design notes) -- no declarative source
 * for these exists anywhere in the squashfs. */
static const char * const TEXT_ENTRY_SYMBOL_GROUPS[10] = {
    ".,", "!?", "()[]", "/\\|", "$%", "@#&", "+-*^", "_{}", ":;", "<=>"
};

static text_entry_kp_mode_t text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
/* Real-device bug report: Shift behaved as a persistent caps toggle (tap
 * once, every following letter stays capitalized until tapped again) --
 * every real T9/phone keypad instead treats these as two separate things:
 * Shift (the key-0 slot) capitalizes only the very next letter typed, then
 * reverts on its own, while tapping the ABC mode button AGAIN while it's
 * already the active mode is what toggles persistent caps ("Caps Lock").
 * text_entry_is_uppercase() below is the union of both -- everywhere that
 * used to read text_entry_shift directly for case now reads that instead. */
static bool text_entry_shift = false;     /* one-shot: consumed by the next letter (see text_entry_handle_digit_key) */
static bool text_entry_caps_lock = false; /* persistent: toggled by re-tapping the ABC mode button */
static bool text_entry_numeric_only = false;

static bool text_entry_is_uppercase(void) {
    return text_entry_caps_lock || text_entry_shift;
}

/* Case for a pending multi-tap cycle is pinned to whatever it was on that
 * cycle's FIRST tap (text_entry_handle_digit_key), not re-read live on every
 * repeat tap -- otherwise a one-shot Shift (consumed immediately after that
 * first tap, see below) would make the letter flip back to lowercase the
 * moment the user cycles to a second candidate for the SAME letter, even
 * though they're still resolving that one same keypress. */
static bool text_entry_pending_shift = false;

/* Multi-tap state: which key (0-10, or -1 = none) is mid-cycle and which
 * character of its cycle is currently showing in the textarea. A repeat tap
 * on the SAME key within TEXT_ENTRY_MULTITAP_MS deletes and re-inserts the
 * next character in its cycle; any other input (a different key, or the
 * timer simply elapsing) finalizes it, so the next tap on that key starts a
 * fresh cycle instead of continuing the old one. */
static int text_entry_pending_key = -1;
static int text_entry_pending_tap_index = 0;
static lv_timer_t * text_entry_multitap_timer;

/* The 10 T9 pad digit-slot keys, indexed by digit 0-9 -- kept so
 * text_entry_refresh_keys() can update their images/visibility in place
 * when the mode/shift/numeric-lock state changes, without rebuilding the
 * screen. Index 0 is special: it shares its screen position with Shift
 * (see text_entry_key0_click_cb's own comment). The numeric-lock "./-" key
 * and the three always-visible mode-jump buttons (123/ABC/sym) each need
 * their own visibility toggled independently of the digit-slot keys, so
 * they get their own named pointers rather than living in this array. */
static lv_obj_t * text_entry_key_img[10];
static lv_obj_t * text_entry_dotneg_key;
static lv_obj_t * text_entry_num_mode_key;
static lv_obj_t * text_entry_abc_mode_key;
static lv_obj_t * text_entry_sym_mode_key;

/* Bumped on every show_text_entry() call -- lets the READY handler below
 * tell whether the caller's done-callback chained straight into ANOTHER
 * show_text_entry() (Wi-Fi manual SSID entry is the one place that does:
 * SSID entered -> immediately prompts for the password) versus a callback
 * that just saved a value and returned. Real-device bug report: manual SSID
 * entry never actually asked for the password. Root cause was a race, not a
 * missing call -- the chained show_text_entry() DID run and DID push the
 * password screen, but this handler's own nav_pop() (issued for the SSID
 * screen, right before invoking the callback) is an ANIMATED transition
 * (screen_transition_slide(), ~NAV_ANIM_TIME_MS), and its completion
 * callback unconditionally lv_screen_load()s the screen nav_pop() was
 * originally headed to once that animation finishes -- landing well after
 * the password screen had already been pushed on top of it, and snapping
 * straight back to the Wi-Fi list before the user ever got a real chance at
 * it. nav_push()'s own dedup guard (same screen object already on top of
 * the stack -> just reload, don't grow the stack) means comparing nav_depth
 * before/after the callback can't detect this -- a chained call reuses this
 * same singleton text_entry_screen without changing nav_depth at all -- so
 * a dedicated generation counter is what actually distinguishes the two
 * cases. */
static uint32_t text_entry_generation = 0;

static void text_entry_reveal_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool now_masked = !lv_textarea_get_password_mode(text_entry_textarea);
    lv_textarea_set_password_mode(text_entry_textarea, now_masked);
    lv_image_set_src(text_entry_reveal_btn, asset_path(now_masked ? "keyboard/psk_show.png" : "keyboard/psk_hide.png"));
}

/* Extracted from what used to be lv_keyboard's own LV_EVENT_READY handler
 * (the T9 Enter key below just calls this directly) -- the chained-
 * show_text_entry()/nav_remove_stack_slot() logic is unrelated to the
 * keypad rewrite, so it's preserved verbatim rather than re-derived. */
static void text_entry_commit(void) {
    /* lv_textarea_get_text()'s buffer belongs to the textarea and would be
     * invalidated/reused the moment another screen starts editing it, so
     * copy it out before invoking the caller's callback (which may itself
     * push a screen that reuses this same textarea, e.g. entering the next
     * field). */
    char text_copy[256];
    snprintf(text_copy, sizeof(text_copy), "%s", lv_textarea_get_text(text_entry_textarea));
    text_entry_done_cb_t cb = text_entry_on_done;
    void * user_data = text_entry_user_data;
    uint32_t generation_before = text_entry_generation;
    int depth_before = gui_navigation_get_depth();
    /* Callback runs BEFORE nav_pop() specifically so the checks below can
     * see whether it navigated anywhere on its own -- see
     * text_entry_generation's own comment. Real-device bug report (Wi-Fi
     * manual SSID entry): the generation check alone wasn't enough -- it
     * only catches a callback that chains into ANOTHER show_text_entry()
     * call reusing this same singleton screen (nav_depth unchanged, dedup
     * guard in nav_push() eats the push). Wi-Fi's password-entered callback
     * instead calls start_wifi_connect(), which nav_push()es a DIFFERENT
     * screen (subsonic_downloading_screen, "Connecting to X...") straight
     * off this one -- nav_depth DOES grow there, since that push isn't a
     * dedup no-op. Confirmed live: the connecting screen flashed up then was
     * animated straight back to the password field by nav_pop() a moment
     * later, looking like nothing happened -- each repeat tap on the
     * keyboard's OK button (six of them, one real device test) genuinely
     * re-ran the whole callback, creating a fresh duplicate saved network
     * every time (wifi_control_connect() always add_network()s a new entry,
     * see its own comment).
     *
     * Simply skipping nav_pop() in that case (an earlier version of this
     * fix) stopped the yank-back, but left THIS screen's own stack slot
     * sitting there permanently, one level below wherever the callback's
     * own push actually landed -- confirmed live via the Subsonic download
     * path's identical shape (see poll_subsonic_download()'s own comment):
     * backing out of the destination screen afterward surfaced this
     * now-defunct form again instead of whatever was open before it, since
     * nothing had ever removed its slot. nav_remove_stack_slot() splices it
     * out after the fact once it's clear something else already took its
     * place. */
    if (cb) cb(text_copy, user_data);
    if (gui_navigation_get_depth() > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else if (text_entry_generation == generation_before) {
        nav_pop();
    }
    /* else (nav_depth == depth_before && generation changed): the callback
     * chained into another show_text_entry() call reusing this exact screen
     * object -- nav_push()'s own dedup guard already reloaded it in place,
     * nothing further to do here. */
}

/* Clears any in-progress multi-tap cycle without touching the textarea --
 * called at the top of every OTHER key's handler (anything that isn't a
 * repeat tap of the same cycling key) so the next tap on that key starts a
 * fresh cycle instead of continuing whatever was pending before. Also what
 * the timeout itself does when no further tap arrives in time. */
static void text_entry_finalize_pending(void) {
    text_entry_pending_key = -1;
    lv_timer_pause(text_entry_multitap_timer);
}

static void text_entry_multitap_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    text_entry_finalize_pending();
}

/* Every character (or character group) a given digit-slot key currently
 * produces, as a NUL-terminated string -- length 1 means "just insert this
 * one character, no cycling" (NUM mode, or ABC mode's key 0/1, or a
 * genuinely single-symbol group); length > 1 is what the multi-tap cycle in
 * text_entry_key_click_cb() steps through. key_index 10 is the
 * numeric-lock-only "./-" key, meaningful only when text_entry_numeric_only
 * is set (its caller already knows not to ask otherwise). out must be at
 * least 8 bytes -- the longest real group (TEXT_ENTRY_SYMBOL_GROUPS' 4-char
 * entries) plus the NUL. uppercase is passed in explicitly (rather than read
 * live off text_entry_is_uppercase()) so text_entry_handle_digit_key() can
 * pin a multi-tap cycle's case to whatever it was on that cycle's first tap
 * -- see text_entry_pending_shift's own comment. */
static void text_entry_key_chars_ex(int key_index, char * out, size_t out_size, bool uppercase) {
    if (key_index == 10) {
        snprintf(out, out_size, ".-");
        return;
    }
    if (text_entry_numeric_only || text_entry_kp_mode == TEXT_ENTRY_KP_NUM) {
        out[0] = (char) ('0' + key_index);
        out[1] = '\0';
        return;
    }
    if (text_entry_kp_mode == TEXT_ENTRY_KP_SYM) {
        snprintf(out, out_size, "%s", TEXT_ENTRY_SYMBOL_GROUPS[key_index]);
        return;
    }
    /* ABC mode. Key 1 has no letter group on this asset set -- real-device
     * photo confirmed the stock keyboard repurposes it as a punctuation
     * shortcut here specifically (char_l.png/char_u.png's own baked-in
     * glyph), not literal "1" the way NUM mode's key 1 is. */
    if (key_index == 1) {
        snprintf(out, out_size, "._@/#");
        return;
    }
    const char * letters = TEXT_ENTRY_LETTER_GROUPS[key_index];
    if (!letters) {
        out[0] = (char) ('0' + key_index);
        out[1] = '\0';
        return;
    }
    size_t n = strlen(letters);
    size_t i;
    for (i = 0; i < n && i + 1 < out_size; i++) {
        out[i] = uppercase ? (char) toupper((unsigned char) letters[i]) : letters[i];
    }
    out[i] = '\0';
}

/* asset_path() only needs relative_path for the duration of its own call
 * (it strdup()s its own "S:..." result -- see its doc comment), so handing
 * back a reused static buffer here is safe: every call site below both
 * builds and consumes the path synchronously, on the GUI thread, one at a
 * time. */
static const char * text_entry_key_image(int key_index) {
    static char buf[64];
    if (text_entry_numeric_only || text_entry_kp_mode == TEXT_ENTRY_KP_NUM) {
        snprintf(buf, sizeof(buf), "keyboard/%d.png", key_index);
    } else if (text_entry_kp_mode == TEXT_ENTRY_KP_SYM) {
        snprintf(buf, sizeof(buf), "keyboard/symbol%d.png", key_index);
    } else if (key_index == 1) {
        /* char_l.png/char_u.png are byte-identical (confirmed via md5sum),
         * so this always resolves to the same image regardless of case --
         * picking by case anyway just for symmetry with every other
         * letter-group key below, which does need it. */
        snprintf(buf, sizeof(buf), "keyboard/char_%c.png", text_entry_is_uppercase() ? 'u' : 'l');
    } else {
        const char * letters = TEXT_ENTRY_LETTER_GROUPS[key_index];
        if (!letters) {
            snprintf(buf, sizeof(buf), "keyboard/%d.png", key_index);
        } else {
            snprintf(buf, sizeof(buf), "keyboard/%s_%c.png", letters, text_entry_is_uppercase() ? 'u' : 'l');
        }
    }
    return buf;
}

/* Re-syncs every key's image and (for the mode buttons/numeric-lock "./-"
 * key) visibility with the current mode/shift/numeric-lock state -- called
 * once from show_text_entry() and again on every mode/Shift/digit-0 tap. */
static void text_entry_refresh_keys(void) {
    for (int i = 1; i < 10; i++) {
        lv_image_set_src(text_entry_key_img[i], asset_path(text_entry_key_image(i)));
    }
    /* Index 0's slot shows Shift while cycling letters (case has no other
     * meaning), and the literal digit-0/symbol-0 key otherwise -- see
     * text_entry_key0_click_cb's own comment for the input-side half of
     * this same split. */
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        lv_image_set_src(text_entry_key_img[0], asset_path("keyboard/upper.png"));
    } else {
        lv_image_set_src(text_entry_key_img[0], asset_path(text_entry_key_image(0)));
    }

    if (text_entry_numeric_only) {
        lv_obj_add_flag(text_entry_num_mode_key, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(text_entry_abc_mode_key, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(text_entry_sym_mode_key, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(text_entry_dotneg_key, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(text_entry_dotneg_key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(text_entry_num_mode_key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(text_entry_abc_mode_key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(text_entry_sym_mode_key, LV_OBJ_FLAG_HIDDEN);
}

/* Shared by every cycling key (the 9 digit-slot keys, the key-0/digit-0
 * slot when it isn't acting as Shift, and the numeric-lock "./-" key) --
 * pulled out of the click handler so text_entry_key0_click_cb below can
 * reuse it without duplicating the multi-tap bookkeeping. A tap on a key
 * whose current character group has more than one character, repeated on
 * the SAME key before text_entry_multitap_timer elapses, cycles to the next
 * character in that group instead of inserting a new one (classic phone-
 * keypad multi-tap: key 2 pressed twice quickly types "b", not "aa"). Any
 * other case (a new key, a single-character group, or the timer having
 * already elapsed) just inserts the group's first character fresh.
 *
 * Case is decided once, on the FIRST tap of a fresh cycle (same_key_pending
 * false), and then pinned in text_entry_pending_shift for any further taps
 * that just cycle through candidates for that SAME letter -- see that
 * variable's own comment. That first tap is also where a one-shot Shift
 * (text_entry_shift, as opposed to the persistent text_entry_caps_lock) gets
 * consumed: it already did its job baking uppercase into chars[] above, so
 * it's cleared right after, letting the very next DIFFERENT key fall back to
 * whatever text_entry_caps_lock alone says. */
static void text_entry_handle_digit_key(int key_index) {
    bool same_key_pending = (text_entry_pending_key == key_index);
    bool uppercase = same_key_pending ? text_entry_pending_shift : text_entry_is_uppercase();

    char chars[8];
    text_entry_key_chars_ex(key_index, chars, sizeof(chars), uppercase);
    size_t n = strlen(chars);

    if (n > 1 && same_key_pending) {
        lv_textarea_delete_char(text_entry_textarea);
        text_entry_pending_tap_index = (text_entry_pending_tap_index + 1) % (int) n;
    } else {
        text_entry_pending_key = n > 1 ? key_index : -1;
        text_entry_pending_tap_index = 0;
        text_entry_pending_shift = uppercase;
        if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC && text_entry_shift) {
            text_entry_shift = false;
            text_entry_refresh_keys();
        }
    }
    lv_textarea_add_char(text_entry_textarea, (uint32_t) chars[text_entry_pending_tap_index]);

    if (n > 1) {
        lv_timer_reset(text_entry_multitap_timer);
        lv_timer_resume(text_entry_multitap_timer);
    }
}

static void text_entry_key_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_handle_digit_key((int) (intptr_t) lv_event_get_user_data(e));
}

/* Key index 0's screen slot (column 4, row 1) does double duty, matching
 * the real stock keyboard's own layout: while cycling letters, tapping it
 * arms a one-shot Shift for the next letter typed (text_entry_is_uppercase()'s
 * own comment) -- there's no digit "0" needed there since ABC mode has no
 * use for one. In NUM/SYM mode (or numeric_only, which behaves like NUM mode
 * regardless of text_entry_kp_mode's stale value -- see text_entry_key_chars_ex()'s
 * own numeric_only check) it's a completely ordinary cycling digit-slot key
 * for index 0, same as any other. */
static void text_entry_key0_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        text_entry_finalize_pending();
        text_entry_shift = !text_entry_shift;
        text_entry_refresh_keys();
        return;
    }
    text_entry_handle_digit_key(0);
}

/* The three mode buttons stacked in column 0 (rows 0-2) are always visible
 * together and each jump straight to their own mode -- unlike the earlier
 * single cycling Mode key this replaces, matching a real-device photo of
 * the stock keyboard's own layout exactly. */
static void text_entry_mode_num_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    text_entry_kp_mode = TEXT_ENTRY_KP_NUM;
    text_entry_shift = false; /* one-shot Shift is meaningless outside ABC -- don't let it carry back in */
    text_entry_refresh_keys();
}

/* Real-device bug report: tapping ABC again while it was ALREADY the active
 * mode did nothing -- the real stock keyboard uses exactly that (re-tapping
 * the already-active mode button) as its persistent Caps Lock toggle,
 * distinct from Shift's one-shot-next-letter behavior on the key-0 slot
 * above. Switching INTO ABC from NUM/SYM just activates it, same as before;
 * text_entry_caps_lock deliberately isn't reset there, so a caps-lock
 * preference set earlier survives a detour through NUM/SYM and back. */
static void text_entry_mode_abc_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        text_entry_caps_lock = !text_entry_caps_lock;
        text_entry_shift = false; /* toggling caps lock supersedes any pending one-shot arm */
    } else {
        text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    }
    text_entry_refresh_keys();
}

static void text_entry_mode_sym_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    text_entry_kp_mode = TEXT_ENTRY_KP_SYM;
    text_entry_shift = false; /* one-shot Shift is meaningless outside ABC -- don't let it carry back in */
    text_entry_refresh_keys();
}

static void text_entry_del_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_delete_char(text_entry_textarea);
}

static void text_entry_left_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_cursor_left(text_entry_textarea);
}

static void text_entry_right_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_cursor_right(text_entry_textarea);
}

static void text_entry_space_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_add_char(text_entry_textarea, ' ');
}

/* Defined in the search-binding section below (dismisses whichever
 * library screen currently has the keypad on loan for inline search --
 * see text_entry_inline_mode_active's own comment). Forward-declared here
 * since it's this file's only reference to search state from within the
 * text-entry section, which comes first. */

static void text_entry_enter_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    if (text_entry_inline_mode_active) {
        t9_keypad_dismiss_only();
        return;
    }
    text_entry_commit();
}

/* Every key is a plain clickable image at its own top-left grid cell (col,
 * row) -- see the icon_tile_press_event_cb doc comment in screen_builders.c
 * for the precedent this follows (clickable lv_image, no wrapper container).
 * Taller/wider keys (Enter, Space) just have a native asset that extends
 * past one cell; their top-left anchor is computed the same way as every
 * other key. */
static lv_obj_t * text_entry_make_key(lv_obj_t * scr, int col, int row, lv_event_cb_t cb, void * user_data) {
    lv_obj_t * img = lv_image_create(scr);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT,
                 TEXT_ENTRY_GRID_X + col * (TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP),
                 TEXT_ENTRY_GRID_Y + row * (TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP));
    if (cb) lv_obj_add_event_cb(img, cb, LV_EVENT_CLICKED, user_data);
    return img;
}

/* T9 keypad: 5 columns x 4 rows (see TEXT_ENTRY_GRID_X/Y's own comment
 * above for the real-device-photo source of this layout). Row 0: Mode:123
 * / 1 / 2-ABC / 3-DEF / Del. Row 1: Mode:ABC / 4-GHI / 5-JKL / 6-MNO /
 * 0-or-Shift. Row 2: Mode:sym / 7-PQRS / 8-TUV / 9-WXYZ / Enter (spans
 * rows 2-3). Row 3: Left / Right / Space (spans cols 2-3) / Enter
 * continues. The numeric-lock "./-" key (numeric_only fields only)
 * occupies the same cell as Mode:123, since the three mode buttons are
 * hidden together whenever it's shown -- see text_entry_refresh_keys().
 *
 * Every key is built as a child of one `group` container (itself a plain
 * full-screen-sized, invisible object at (0,0)) rather than directly on
 * `parent`, so the whole keypad can be reparented in one
 * lv_obj_set_parent(group, ...) call -- see t9_keypad_attach()/
 * t9_keypad_release() -- instead of moving each key individually. Every
 * screen this ever attaches to is the same 480x800 full-screen size, so
 * TEXT_ENTRY_GRID_X/Y's absolute-pixel positioning (already fixed
 * relative to `group`'s own (0,0) origin) lands identically regardless of
 * which screen currently owns it. */
static lv_obj_t * build_t9_keypad_group(lv_obj_t * parent) {
    lv_obj_t * group = lv_obj_create(parent);
    lv_obj_set_size(group, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(group, 0, 0);
    lv_obj_set_style_bg_opa(group, 0, 0);
    lv_obj_set_style_border_width(group, 0, 0);
    /* lv_obj_create()'s default theme padding shifts every key's TOP_LEFT-
     * aligned TEXT_ENTRY_GRID_X/Y position (and the black backing rect
     * below) inward from group's true left/top edge -- real-device
     * feedback: the keypad's first column didn't actually reach the
     * screen's left border despite TEXT_ENTRY_GRID_X computing to ~1px.
     * Same root cause already documented on the search bar's own padding. */
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_CLICKABLE);

    /* Opaque black backing sized to the keypad's own footprint (plus a
     * small margin), created before the keys so it sits behind them --
     * real-device feedback: the keys themselves have gaps between them
     * (TEXT_ENTRY_KEY_GAP), and `group` above is fully transparent, so
     * without this whatever's on the target screen behind the keypad
     * (list rows, etc.) showed through those gaps instead of solid black. */
    lv_obj_t * backing = lv_obj_create(group);
    lv_obj_set_size(backing, lv_pct(100), TEXT_ENTRY_GRID_HEIGHT + TEXT_ENTRY_BOTTOM_MARGIN + 8);
    lv_obj_set_pos(backing, 0, TEXT_ENTRY_GRID_Y - 8);
    lv_obj_set_style_bg_opa(backing, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(backing, lv_color_black(), 0);
    lv_obj_set_style_border_width(backing, 0, 0);
    lv_obj_set_style_radius(backing, 0, 0);
    lv_obj_remove_flag(backing, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(backing, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 1; i <= 9; i++) {
        int col = 1 + (i - 1) % 3;
        int row = (i - 1) / 3;
        text_entry_key_img[i] = text_entry_make_key(group, col, row, text_entry_key_click_cb, (void *) (intptr_t) i);
    }
    text_entry_key_img[0] = text_entry_make_key(group, 4, 1, text_entry_key0_click_cb, NULL);

    text_entry_dotneg_key = text_entry_make_key(group, 0, 0, text_entry_key_click_cb, (void *) (intptr_t) 10);
    lv_image_set_src(text_entry_dotneg_key, asset_path("keyboard/dot.png"));

    text_entry_num_mode_key = text_entry_make_key(group, 0, 0, text_entry_mode_num_click_cb, NULL);
    lv_image_set_src(text_entry_num_mode_key, asset_path("keyboard/num.png"));
    text_entry_abc_mode_key = text_entry_make_key(group, 0, 1, text_entry_mode_abc_click_cb, NULL);
    lv_image_set_src(text_entry_abc_mode_key, asset_path("keyboard/char.png"));
    text_entry_sym_mode_key = text_entry_make_key(group, 0, 2, text_entry_mode_sym_click_cb, NULL);
    lv_image_set_src(text_entry_sym_mode_key, asset_path("keyboard/symbol.png"));

    lv_obj_t * del_key = text_entry_make_key(group, 4, 0, text_entry_del_click_cb, NULL);
    lv_image_set_src(del_key, asset_path("keyboard/del.png"));
    lv_obj_t * left_key = text_entry_make_key(group, 0, 3, text_entry_left_click_cb, NULL);
    lv_image_set_src(left_key, asset_path("keyboard/left.png"));
    lv_obj_t * right_key = text_entry_make_key(group, 1, 3, text_entry_right_click_cb, NULL);
    lv_image_set_src(right_key, asset_path("keyboard/right.png"));
    lv_obj_t * enter_key = text_entry_make_key(group, 4, 2, text_entry_enter_click_cb, NULL);
    lv_image_set_src(enter_key, asset_path("keyboard/enter.png"));
    lv_obj_t * space_key = text_entry_make_key(group, 2, 3, text_entry_space_click_cb, NULL);
    lv_image_set_src(space_key, asset_path("keyboard/space2.png"));

    text_entry_multitap_timer = lv_timer_create(text_entry_multitap_timeout_cb, TEXT_ENTRY_MULTITAP_MS, NULL);
    lv_timer_pause(text_entry_multitap_timer);

    /* Lets a swipe started on the keypad (inline search's Enter key sits
     * right where a right-swipe could plausibly start) still bubble up to
     * the target screen's own back-gesture handler -- built once here, so
     * it persists across every future t9_keypad_attach() reparenting
     * (flags are object properties, not derived from the parent). */
    lv_obj_add_flag(group, LV_OBJ_FLAG_GESTURE_BUBBLE);
    enable_gesture_bubble_recursive(group);

    return group;
}

/* Reparents the shared keypad_group + text_entry_textarea onto
 * target_screen for inline search -- see text_entry_inline_mode_active's
 * own comment. Resets mode/shift/caps-lock/numeric state and clears the
 * textarea the same way show_text_entry() resets it for a fresh modal
 * field, since this is the same shared state machine. Search never needs
 * the password reveal button -- it stays on text_entry_screen, harmless
 * since that screen isn't the active one while this is attached elsewhere. */
void t9_keypad_attach(lv_obj_t * target_screen, lv_obj_t * textarea_parent, int32_t textarea_x, int32_t textarea_y,
                              int32_t textarea_w) {
    lv_obj_set_parent(text_entry_keypad_group, target_screen);
    lv_obj_set_parent(text_entry_textarea, textarea_parent);
    /* lv_obj_align() (used to position this textarea for the modal flow,
     * see t9_keypad_release() below) sets a PERSISTENT align-mode style
     * property, not just a one-off position -- plain lv_obj_set_pos() here
     * left that mode at its build-time LV_ALIGN_TOP_MID, so textarea_x/y
     * were being applied as an offset from horizontal CENTER of whatever
     * the current parent is, not as an absolute top-left position.
     * Real-device symptom: the textarea rendered ~38px further right than
     * intended and visibly overlapped close_btn. lv_obj_align() with
     * TOP_LEFT here resets the mode so these coordinates are absolute. */
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_LEFT, textarea_x, textarea_y);
    lv_obj_set_width(text_entry_textarea, textarea_w);
    lv_textarea_set_password_mode(text_entry_textarea, false);
    lv_textarea_set_text(text_entry_textarea, "");

    text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    text_entry_shift = false;
    text_entry_caps_lock = false;
    text_entry_numeric_only = false;
    text_entry_finalize_pending();
    text_entry_refresh_keys();
    text_entry_generation++;
    text_entry_inline_mode_active = true;
}

/* text_entry_textarea's own resting height -- derived from gui_theme_font(GUI_FONT_ROLE_BODY)'s
 * real line height (bigger at the Medium/BlindMF font tiers) plus fixed
 * vertical padding, rather than a flat pixel value sized for the smallest
 * tier only. Recomputed on demand (not cached) because the stable
 * app_font_* descriptor can change metrics during a live tier switch;
 * reading it fresh costs nothing. Shared by both
 * text_entry_textarea's own creation site and t9_keypad_release()'s reset
 * below -- real-device bug report: text started scaling correctly but the
 * field's own box didn't, because the reset below still had the old flat
 * 50px baked in as a separate literal and silently overwrote the creation
 * site's own (correct) height on every single show_text_entry() call. */
static int32_t text_entry_field_height(void) {
    return lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) + 26;
}

/* Returns keypad_group + text_entry_textarea to text_entry_screen, their
 * resting parent when neither the modal flow nor inline search is using
 * them -- called both when inline search fully closes and defensively at
 * the start of show_text_entry() (see its own comment) so opening the
 * modal always reclaims them correctly regardless of which screen
 * currently holds them. */
void t9_keypad_release(void) {
    lv_obj_set_parent(text_entry_keypad_group, text_entry_screen);
    lv_obj_set_parent(text_entry_textarea, text_entry_screen);
    lv_obj_set_size(text_entry_textarea, lv_pct(78), text_entry_field_height());
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + 40);
    text_entry_inline_mode_active = false;
}

static void text_entry_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (text_entry_on_done == plugin_text_entry_done_cb) plugin_manager_text_input_cancelled();
    generic_back_cb(e);
}

static lv_obj_t * build_text_entry_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* lv_keyboard_create()'s built-in Close/X key used to be this screen's
     * only cancel path (see the old LV_EVENT_CANCEL branch this replaces --
     * generic_back_cb below is functionally identical, just nav_pop()).
     * Removing lv_keyboard for the T9 keypad below means that key is gone,
     * so this screen needs its own explicit back button now -- same
     * top-left 64x64/sub_back/btn_back.png pattern as build_files_screen()
     * and every other hand-built screen in this file. */
    lv_obj_t * back_btn = lv_obj_create(scr);
    lv_obj_set_size(back_btn, 64, 64);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 0, STATUS_BAR_CLEARANCE);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_remove_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, text_entry_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_arrow = lv_image_create(back_btn);
    lv_image_set_src(back_arrow, asset_path("sub_back/btn_back.png"));
    lv_obj_center(back_arrow);

    text_entry_title_label = lv_label_create(scr);
    lv_label_set_text(text_entry_title_label, "");
    lv_obj_align(text_entry_title_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + 8);
    lv_obj_add_style(text_entry_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(text_entry_title_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);

    text_entry_textarea = lv_textarea_create(scr);
    /* Real-device bug report: this field's typed text stayed at LVGL's own
     * unscaled default font regardless of Settings -> Font Size -- unlike
     * every other body-text label in the app, nothing here ever set an
     * explicit tier-aware font. gui_theme_font(GUI_FONT_ROLE_BODY) matches
     * most other body text and follows live general font-size changes;
     * see text_entry_field_height()'s own comment for the field's height. */
    lv_obj_set_style_text_font(text_entry_textarea, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_size(text_entry_textarea, lv_pct(78), text_entry_field_height());
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + 40);
    lv_textarea_set_one_line(text_entry_textarea, true);
    /* Built once here, so it persists across every future t9_keypad_attach()
     * reparenting onto a library screen's own search bar -- lets a swipe
     * started on the textarea itself still bubble up to that screen's
     * back-gesture handler (see search_close_if_active_for_screen()). */
    lv_obj_add_flag(text_entry_textarea, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* Fires on every keystroke (lv_textarea's own native behavior --
     * nothing listened for this before inline search existed, since every
     * other text_entry_textarea use is one-shot-on-Enter via
     * text_entry_commit()). search_textarea_value_changed_cb() itself
     * no-ops unless text_entry_inline_mode_active is set, so this is inert
     * for all 9 existing modal callers. */
    lv_obj_add_event_cb(text_entry_textarea, search_textarea_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Real-device bug report: no way to unmask a password field while
     * typing it. Hidden entirely for non-password fields by
     * show_text_entry() below -- the textarea is narrowed to make room for
     * it unconditionally so switching between password/non-password fields
     * doesn't reflow the textarea's width and position. */
    text_entry_reveal_btn = lv_image_create(scr);
    lv_image_set_src(text_entry_reveal_btn, asset_path("keyboard/psk_show.png"));
    lv_obj_align_to(text_entry_reveal_btn, text_entry_textarea, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_add_flag(text_entry_reveal_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(text_entry_reveal_btn, text_entry_reveal_btn_cb, LV_EVENT_CLICKED, NULL);

    text_entry_keypad_group = build_t9_keypad_group(scr);

    return scr;
}

/* Pushes the text entry screen pre-filled with initial_text (may be NULL
 * for empty) under `title`; on_done fires with whatever was typed when the
 * T9 keypad's Enter key is pressed (not called at all if the screen's own
 * back button is used instead -- callers shouldn't assume it always fires).
 * is_password masks the input the same way a real password field would.
 * numeric locks the keypad to plain-digit entry (see text_entry_numeric_
 * only's own comment) instead of the normal ABC/NUM/SYM T9 cycle --
 * everything peq.c's freq/gain/Q fields need (digits, a decimal point, and
 * a minus sign for negative gain), nothing else. */
void show_text_entry(const char * title, const char * initial_text, bool is_password, bool numeric,
                             text_entry_done_cb_t on_done, void * user_data) {
    if (text_entry_on_done == plugin_text_entry_done_cb && on_done != plugin_text_entry_done_cb) {
        plugin_manager_text_input_cancelled();
    }
    /* Defensive reclaim -- if inline search happened to be holding the
     * shared keypad/textarea when some other flow (e.g. a Wi-Fi password
     * prompt) opens the modal, this puts them back on text_entry_screen
     * (and restores the textarea's modal position/size) before anything
     * below touches them. Harmless no-op when they're already there. */
    t9_keypad_release();

    lv_label_set_text(text_entry_title_label, title);
    lv_textarea_set_text(text_entry_textarea, initial_text ? initial_text : "");
    lv_textarea_set_password_mode(text_entry_textarea, is_password);
    /* Always reset to masked/hidden -- otherwise a field left revealed by
     * the previous show_text_entry() call (e.g. Wi-Fi's chained SSID ->
     * password, which reuses this same singleton screen without an
     * intervening rebuild) would carry that state into an unrelated field. */
    lv_image_set_src(text_entry_reveal_btn, asset_path("keyboard/psk_show.png"));
    if (is_password) {
        lv_obj_remove_flag(text_entry_reveal_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(text_entry_reveal_btn, LV_OBJ_FLAG_HIDDEN);
    }
    text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    text_entry_shift = false;
    text_entry_caps_lock = false;
    text_entry_numeric_only = numeric;
    text_entry_finalize_pending();
    text_entry_refresh_keys();
    text_entry_on_done = on_done;
    text_entry_user_data = user_data;
    text_entry_generation++;
    nav_push(text_entry_screen);
}

bool gui_text_input_init(void) {
    text_entry_screen = build_text_entry_screen();
    return true;
}

/* For gui_reload.c's in-process UI reload -- deletes the text-entry screen
 * so gui_text_input_init() can rebuild it from a clean slate. Covers
 * text_entry_keypad_group too even when it's currently reparented onto some
 * other screen's inline search (t9_keypad_attach()) -- that other screen is
 * being torn down in the same reload pass regardless. */
void gui_text_input_teardown(void) {
    /* Unguarded lv_timer_create() at this screen's own build site -- same
     * leaked-old-timer hazard as gui_player_teardown()'s volume_popup_hide_
     * timer, see its own comment. Checked independently of text_entry_screen
     * below rather than under one shared early-return, so it's still
     * cleaned up even if the two ever get out of sync. */
    if (text_entry_multitap_timer) { lv_timer_del(text_entry_multitap_timer); text_entry_multitap_timer = NULL; }
    if (text_entry_screen) { lv_obj_del(text_entry_screen); text_entry_screen = NULL; }
}

lv_obj_t * gui_text_input_get_screen(void) {
    return text_entry_screen;
}

void t9_keypad_dismiss_only(void) {
    lv_obj_set_parent(text_entry_keypad_group, text_entry_screen);
    text_entry_inline_mode_active = false;
}

int32_t t9_keypad_get_grid_y(void) {
    return TEXT_ENTRY_GRID_Y;
}

bool t9_keypad_is_inline_active(void) {
    return text_entry_inline_mode_active;
}

const char * t9_keypad_get_text(void) {
    return lv_textarea_get_text(text_entry_textarea);
}
