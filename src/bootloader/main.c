/* open_hiby_bootloader -- see the design doc this was built from for the
 * full rationale. Short version: hiby_player.sh execs straight into this
 * binary now (see that script's own updated comment); this decides which
 * player to run, optionally shows a boot-selector menu, then takes over
 * hiby_player.sh's previous job of supervising that player. Unexpected
 * exits still reboot for crash recovery, while a clean exit completes the
 * player's intentional power-off request -- see run_player_supervised(). */

#include "scanner.h"
#include "fb_draw.h"
#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char ** environ; /* not declared by <unistd.h> under this project's
                          * feature-test macro settings -- see this file's
                          * own Makefile entry, and no other file in this
                          * codebase has needed it before now. */

/* The Stock player's proprietary framebuffer backend opens
 * /dev/sa_hgl_dma and asks its driver for one physically-contiguous 8 MiB
 * allocation (confirmed from a live order-11 allocation failure in
 * sahd_open()). Merely having more than 8 MiB free is not sufficient: by
 * the time the boot selector has decoded its background, allocated its
 * buffers, scanned the SD card, and waited for a choice, the small 56 MiB
 * system can have enough total free RAM but no order-11 buddy block left.
 * Stock then continues without a framebuffer and the last boot-menu frame
 * appears frozen even though the rest of Stock is alive.
 *
 * Reserve that exact driver allocation before this process performs any
 * substantial allocation. Holding the fd keeps the contiguous block out
 * of the general allocator throughout the menu. It deliberately remains
 * open across fork: the child inherits the same open-file reference, the
 * parent then closes its copy, and O_CLOEXEC releases the child's final
 * reference inside a successful execve(). This is materially later than
 * closing before fork -- real-device testing showed that the freshly freed
 * order-11 block could otherwise be split by fork/ELF-loader allocations
 * before Stock opened the driver itself. If execve() fails, CLOEXEC has not
 * fired and the fallback exec remains protected. Open Player does not use
 * this device, but follows the same handoff for consistent fd hygiene. A
 * failed reservation is logged but never blocks boot; Stock then retains
 * its existing best-effort behavior. */
#define HGL_DMA_DEVICE "/dev/sa_hgl_dma"

static int hgl_dma_reservation_fd = -1;

static void reserve_stock_hgl_dma(void) {
    hgl_dma_reservation_fd = open(HGL_DMA_DEVICE, O_RDWR | O_CLOEXEC);
    if (hgl_dma_reservation_fd < 0) {
        perror("open_hiby_bootloader: failed to reserve Stock HGL DMA memory");
    }
}

static void release_stock_hgl_dma(void) {
    if (hgl_dma_reservation_fd < 0) return;
    if (close(hgl_dma_reservation_fd) != 0) {
        perror("open_hiby_bootloader: failed to release Stock HGL DMA reservation");
    }
    hgl_dma_reservation_fd = -1;
}

#define CARD_MARGIN_X 40
#define CARD_WIDTH (FB_WIDTH - 2 * CARD_MARGIN_X)
/* Tight enough that the first card sits right under the title/countdown
 * area with no dead space, even when the countdown itself isn't currently
 * shown (cancelled -- see draw_menu()'s own remaining_ms < 0 case) --
 * that's the specific case that looked like wasted space before this. */
#define CARDS_TOP 110
#define CARDS_BOTTOM 680
#define CARD_GAP 24
#define MAX_CARDS 3 /* Internal + SD stock + SD update -- see scanner.h's own BOOT_ENTRY_* */
#define TITLE_Y 36
#define COUNTDOWN_Y 68
#define PROGRESS_BAR_Y 92
#define PROGRESS_BAR_HEIGHT 6
#define FOOTER_Y 700

/* Card backgrounds are alpha-blended over the artwork (see draw_card()),
 * not opaque -- 128/255 reads as roughly 50% without being exactly
 * transposed through 8-bit math out to 3 significant figures, which
 * would be a false precision no one could actually see the difference of
 * on a 5/6/5-bit panel. */
#define CARD_BG_ALPHA 128

/* NOT /etc/logo1.jpeg directly, even though that's this device's own real
 * boot splash asset (confirmed exactly FB_WIDTH x FB_HEIGHT already) --
 * confirmed on-device (JDR_FMT3 from jd_prepare(), then verified directly
 * by walking the file's own JPEG markers) that it's encoded as progressive
 * DCT (SOF marker 0xC2), which tjpgd -- a baseline-only decoder, by
 * design, not a bug in this bootloader's own use of it -- cannot decode
 * at all. This is a baseline re-encode of that exact same image (same
 * pixels, `convert /etc/logo1.jpeg -interlace none`). Ships as part of the
 * squashfs image itself (alongside this binary, see the Phase 3 repack
 * process), not on the writable partition -- unlike the Phase 2 on-device
 * test path this replaced, this must not depend on /usr/data already
 * having been populated by hand; a factory-reset or first-ever-flashed
 * device still needs the background to work. */
#define BOOTLOADER_BG_PATH "/etc/bootloader_bg.jpg"

#define COLOR_BG fb_rgb(0x12, 0x12, 0x12)
#define COLOR_TEXT fb_rgb(0xFF, 0xFF, 0xFF)
#define COLOR_MUTED fb_rgb(0x88, 0x88, 0x88)
#define COLOR_ACCENT fb_rgb(0x21, 0x96, 0xF3)
#define COLOR_BORDER_MUTED fb_rgb(0x40, 0x40, 0x40)

typedef struct {
    int y;
    int height;
    const char * line1;
    const char * line2;
    int boot_entry; /* a BOOT_ENTRY_* value (scanner.h) -- which real choice this card represents */
} card_layout_t;

/* Splits the fixed CARDS_TOP..CARDS_BOTTOM band evenly across however many
 * cards are actually present (2, when only one SD alternate exists
 * alongside Internal, or 3, when both do) -- rather than a fixed height
 * that only ever fit exactly two, since either SD alternate can now be
 * present independently of the other (see scanner.h's own doc comment on
 * sd_stock_present/sd_update_present). */
static void layout_cards(card_layout_t * cards, int count) {
    int height = (CARDS_BOTTOM - CARDS_TOP - CARD_GAP * (count - 1)) / count;
    for (int i = 0; i < count; i++) {
        cards[i].y = CARDS_TOP + i * (height + CARD_GAP);
        cards[i].height = height;
    }
}

static void draw_centered(int y, const char * text, fb_color_t color) {
    int w = fb_text_width(text);
    fb_draw_text((FB_WIDTH - w) / 2, y, text, color);
}

static void draw_card(const card_layout_t * card, bool selected) {
    /* Semi-transparent over the artwork, not opaque -- border and text
     * stay fully opaque either way (CARD_BG_ALPHA only applies to the
     * fill), or the selection highlight and labels would wash out right
     * along with the background. */
    fb_fill_rect_alpha(CARD_MARGIN_X, card->y, CARD_WIDTH, card->height, fb_rgb(0x1E, 0x1E, 0x1E), CARD_BG_ALPHA);
    fb_draw_rect_border(CARD_MARGIN_X, card->y, CARD_WIDTH, card->height, selected ? 4 : 1,
                        selected ? COLOR_ACCENT : COLOR_BORDER_MUTED);
    fb_draw_text(CARD_MARGIN_X + 30, card->y + 35, card->line1, COLOR_TEXT);
    fb_draw_text(CARD_MARGIN_X + 30, card->y + 75, card->line2, COLOR_MUTED);
}

static bool point_in_card(const card_layout_t * card, int x, int y) {
    return x >= CARD_MARGIN_X && x < CARD_MARGIN_X + CARD_WIDTH && y >= card->y && y < card->y + card->height;
}

/* remaining_ms < 0 hides the countdown text/bar entirely -- used once a
 * key/touch input has cancelled the timeout (see main()'s own doc comment
 * on why cancelling must still leave a menu the user can act on, not blank
 * the screen). */
static void draw_menu(const card_layout_t * cards, int count, int selected, int remaining_ms, int timeout_ms) {
    fb_restore_background(COLOR_BG); /* fast cached blit, not a re-decode -- see fb_draw.h's own doc comment */
    draw_centered(TITLE_Y, "SELECT PLAYER", COLOR_TEXT);

    if (remaining_ms >= 0) {
        char line[32];
        snprintf(line, sizeof(line), "AUTO BOOT IN %d", (remaining_ms + 999) / 1000);
        draw_centered(COUNTDOWN_Y, line, COLOR_MUTED);

        int bar_w = CARD_WIDTH * remaining_ms / (timeout_ms > 0 ? timeout_ms : 1);
        fb_fill_rect(CARD_MARGIN_X, PROGRESS_BAR_Y, CARD_WIDTH, PROGRESS_BAR_HEIGHT, COLOR_BORDER_MUTED);
        fb_fill_rect(CARD_MARGIN_X, PROGRESS_BAR_Y, bar_w, PROGRESS_BAR_HEIGHT, COLOR_ACCENT);
    }

    for (int i = 0; i < count; i++) draw_card(&cards[i], cards[i].boot_entry == selected);

    draw_centered(FOOTER_Y, "VOL MOVE   PLAY OK", COLOR_MUTED);
    fb_flush();
}

/* Runs the actual selector: input/countdown loop, returns the BOOT_ENTRY_*
 * the user picked or the timeout confirmed. Only called when scanner_scan()
 * has already established there IS a real choice to make (sd_stock_present)
 * -- the no-alternate and newer-update cases in main() never reach this at
 * all, matching the "instant boot, no delay" objective for those. */
static long elapsed_ms_since(const struct timespec * start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

/* Builds however many cards actually apply -- Internal is always present;
 * SD stock/update are each independently optional (see scanner.h's own
 * sd_stock_present/sd_update_present) and BOTH can be present on the same
 * card at once. "OPEN PLAYER" is deliberately reused as SD update's own
 * title too (same app, just a different copy) -- the "SD CARD" subtitle
 * is what actually distinguishes it from Internal's own card, the same
 * way "STOCK PLAYER" is distinguished from it by title alone. */
static int build_cards(const scan_result_t * scan, card_layout_t * cards) {
    int count = 0;
    cards[count++] = (card_layout_t) { 0, 0, "OPEN PLAYER", "INTERNAL", BOOT_ENTRY_INTERNAL };
    if (scan->sd_update_present) {
        cards[count++] = (card_layout_t) { 0, 0, "OPEN PLAYER", "SD CARD", BOOT_ENTRY_SD_UPDATE };
    }
    if (scan->sd_stock_present) {
        cards[count++] = (card_layout_t) { 0, 0, "STOCK PLAYER", "SD CARD", BOOT_ENTRY_SD_STOCK };
    }
    layout_cards(cards, count);
    return count;
}

static int run_menu(const scan_result_t * scan) {
    card_layout_t cards[MAX_CARDS];
    int card_count = build_cards(scan, cards);

    input_open(); /* whether anything actually opened is re-checked live via input_any_open() below, not captured once here */
    int selected = scan->default_entry;
    int timeout_ms = scan->timeout_seconds * 1000;
    /* Always starts true, regardless of whether any input device is open --
     * with none open, nothing can ever cancel it, but it still has to
     * actually count down to confirm the default and boot. Previously
     * this was seeded from input_open()'s own one-time return value, which
     * meant a device with every input fd unopenable (permissions, hardware
     * fault) hung here forever: the loop's exit condition required the
     * countdown to be both active AND expired, but starting it disabled
     * left neither ever true. */
    bool countdown_active = true;

    /* CLOCK_MONOTONIC deadline, not a fixed per-iteration decrement -- a
     * loop that subtracted a flat tick_ms per iteration regardless of how
     * long poll() actually blocked could shorten the countdown for real:
     * any evdev read that returns quickly with data poll() doesn't
     * recognize as a menu event (a touch-move update card, an intermediate
     * SYN_REPORT, anything drain_fd() doesn't map to a bl_input_type_t)
     * still ends that iteration fast, and the loop would charge it a full
     * tick's worth of assumed elapsed time regardless. */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    draw_menu(cards, card_count, selected, timeout_ms, timeout_ms);

    for (;;) {
        int remaining_ms = timeout_ms;
        if (countdown_active) {
            remaining_ms = timeout_ms - (int) elapsed_ms_since(&start);
            if (remaining_ms <= 0) return selected; /* countdown reached zero -- confirm whatever is currently highlighted */
        }

        /* Poll only up to the next redraw tick (or the exact remaining
         * time, if less) while the countdown is live, so the displayed
         * number/bar still updates smoothly; block indefinitely (-1, valid
         * per poll()) once cancelled -- nothing left to time, and it costs
         * nothing to wait for the user's actual decision instead of
         * waking up every 100ms for no reason. */
        int poll_timeout_ms;
        if (!countdown_active) poll_timeout_ms = -1;
        else poll_timeout_ms = remaining_ms < 100 ? remaining_ms : 100;

        /* Re-checked every iteration, not just once up front -- input_poll()
         * itself closes and drops any fd that reports POLLHUP/POLLERR/
         * POLLNVAL (a disconnected/errored device). Without re-checking,
         * a device that starts out fine and later wedges would leave this
         * loop still trying to call input_poll(-1) (indefinite block) with
         * zero fds actually open, which returns immediately every time
         * instead of blocking -- a 100%-CPU busy loop once the countdown
         * is cancelled, not a hang exactly, but just as stuck in practice. */
        bl_input_event_t ev;
        if (input_any_open()) {
            ev = input_poll(poll_timeout_ms);
        } else if (!countdown_active) {
            /* No input left at all, AND the countdown was already
             * cancelled by a real, earlier user action -- there is no
             * deadline left to eventually expire (that top-of-loop check
             * is skipped entirely while !countdown_active) and now no way
             * to ever receive the confirmation that action was waiting
             * for either. Waiting any longer here hangs forever: paced at
             * 100ms instead of busy-spinning, but just as stuck in
             * practice. Resolve with whatever was last selected rather
             * than sit here indefinitely. */
            return selected;
        } else {
            /* Countdown still active, just no input devices to poll (none
             * ever opened, or all have since errored out) -- nothing to
             * poll, but this iteration must still take real wall-clock
             * time; the top-of-loop deadline check above is what actually
             * ends the countdown in this case, this just paces how often
             * it's re-checked. */
            usleep((useconds_t) (poll_timeout_ms > 0 ? poll_timeout_ms : 100) * 1000);
            ev = (bl_input_event_t) { BL_INPUT_NONE, 0, 0 };
        }

        if (ev.type == BL_INPUT_MOVE_UP || ev.type == BL_INPUT_MOVE_DOWN) {
            /* Cycles through however many cards are actually present, by
             * ARRAY POSITION, not by BOOT_ENTRY_* value -- those two no
             * longer coincide now that either SD alternate can be absent
             * independently (e.g. with only SD update present, the array
             * is [Internal, SdUpdate], positions 0 and 1, while
             * BOOT_ENTRY_SD_UPDATE is 2). Find the current card's index,
             * step it, wrap around. */
            int idx = 0;
            for (int i = 0; i < card_count; i++) {
                if (cards[i].boot_entry == selected) { idx = i; break; }
            }
            idx = (ev.type == BL_INPUT_MOVE_DOWN) ? (idx + 1) % card_count : (idx - 1 + card_count) % card_count;
            selected = cards[idx].boot_entry;
            countdown_active = false;
            draw_menu(cards, card_count, selected, -1, timeout_ms);
        } else if (ev.type == BL_INPUT_CONFIRM) {
            return selected;
        } else if (ev.type == BL_INPUT_TOUCH_DOWN) {
            /* Cancels the countdown only -- selection/confirmation happens
             * on release (BL_INPUT_TOUCH_TAP below), same as before. This
             * just stops the deadline from expiring out from under a
             * finger already resting on a card (see BL_INPUT_TOUCH_DOWN's
             * own doc comment in input.h). */
            countdown_active = false;
            draw_menu(cards, card_count, selected, -1, timeout_ms);
        } else if (ev.type == BL_INPUT_TOUCH_TAP) {
            bool hit_a_card = false;
            for (int i = 0; i < card_count; i++) {
                if (point_in_card(&cards[i], ev.x, ev.y)) {
                    hit_a_card = true;
                    selected = cards[i].boot_entry;
                    break;
                }
            }
            if (hit_a_card) return selected;
            /* Tap outside every card -- cancels the countdown without
             * changing the selection, same as a nav key press, rather
             * than being silently ignored. */
            countdown_active = false;
            draw_menu(cards, card_count, selected, -1, timeout_ms);
        } else if (countdown_active) {
            draw_menu(cards, card_count, selected, remaining_ms, timeout_ms);
        }
    }
}

/* Takes over hiby_player.sh's crash-supervision job. A pure execve() chain
 * with nothing left supervising would silently drop reboot-on-crash, but
 * treating EVERY child exit as a crash is also wrong: both players replace
 * themselves with /sbin/poweroff for an intentional shutdown, and that
 * helper exits successfully once it has handed shutdown to init. Rebooting
 * one second later races and defeats that shutdown. Therefore a confirmed
 * exit status of zero means "complete power-off"; a signal, nonzero exit,
 * wait failure, fork/exec failure, or any other abnormal outcome retains
 * the established crash-recovery reboot. */
static void reboot_device(void) {
    sleep(1);
    sync();
    reboot(RB_AUTOBOOT);
    /* reboot() not returning is the expected outcome; if it somehow does,
     * there is nothing left for this process to usefully do. */
    _exit(1);
}

static void poweroff_device(void) {
    sync();
    reboot(RB_POWER_OFF);
    /* Never turn a failed power-off into a reboot. Remaining alive is the
     * safer failure mode: init's already-requested shutdown may still
     * finish, and the user can still use the hardware power button. */
    perror("open_hiby_bootloader: poweroff syscall failed");
    for (;;) pause();
}

static void run_player_supervised(const char * player_path) {
    pid_t pid = fork();
    if (pid < 0) {
        /* Can't fork at all -- an embedded device in this state has bigger
         * problems than losing the reboot-supervisor for one launch.
         * execve() replaces this process outright; O_CLOEXEC releases the
         * HGL reservation only once that exec succeeds. If exec also fails,
         * there is no child to wait for either way, so go straight to the
         * same reboot the normal path would have ended in. */
        perror("open_hiby_bootloader: fork failed, execve'ing directly (no reboot-on-crash this launch)");
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        perror("open_hiby_bootloader: execve failed");
        reboot_device();
    }

    if (pid == 0) {
        /* Do not close the HGL reservation here. Its O_CLOEXEC flag releases
         * the final inherited reference inside a successful execve(), later
         * than an explicit close followed by ELF loading. If this exec
         * fails, retaining it protects the internal-player fallback below. */
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        /* Only reached if execve() itself failed (bad binary, ENOENT,
         * etc.) -- fall back to the always-present internal player rather
         * than leaving a blank screen, matching the original design's own
         * fallback intent, then give up for real if even that fails. */
        perror("open_hiby_bootloader: execve failed, falling back to internal player");
        if (strcmp(player_path, INTERNAL_PLAYER_PATH) != 0) {
            execve(INTERNAL_PLAYER_PATH, (char * []) { (char *) INTERNAL_PLAYER_PATH, NULL }, environ);
        }
        _exit(127);
    }

    /* The child now holds the same open-file reference until its successful
     * exec processes O_CLOEXEC. Drop only the supervisor's copy: closing it
     * cannot free the contiguous block prematurely while the child reference
     * remains alive, but ensures the long-lived waitpid parent pins no RAM
     * after the player has taken over. */
    release_stock_hgl_dma();

    /* Retry on EINTR rather than treating any waitpid() return as "the
     * child exited" -- an unrelated signal interrupting this call left
     * `status` unset and pid still running; proceeding straight to reboot
     * in that case would reboot the device out from under a player that
     * is still fine, not recovering from anything. Only a return of
     * exactly `pid` means it was actually reaped. */
    int status;
    pid_t reaped;
    do {
        reaped = waitpid(pid, &status, 0);
    } while (reaped == -1 && errno == EINTR);

    if (reaped != pid) {
        /* Shouldn't happen (this is the exact pid this process just
         * forked, not some untracked/already-reaped child) -- but if it
         * ever does, there is no more useful state to report than the
         * fact that the wait itself failed; still fall through to reboot
         * rather than looping on an error that will not resolve itself. */
        perror("open_hiby_bootloader: waitpid failed unexpectedly");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "open_hiby_bootloader: %s exited cleanly -- powering off\n", player_path);
        poweroff_device();
    } else {
        fprintf(stderr, "open_hiby_bootloader: %s exited abnormally (status=0x%x) -- rebooting\n",
                player_path, (unsigned) status);
    }
    reboot_device();
}

int main(void) {
    /* Must remain the first resource acquisition in main(): see
     * reserve_stock_hgl_dma() for why reserving after fb_open() or the SD
     * scan is already too late on this memory-constrained device. */
    reserve_stock_hgl_dma();

    /* Opened and drawn to BEFORE scanner_scan() (which performs the SD
     * card settle wait, up to ~2.5s -- see scanner.c's own doc comment),
     * not after. Real-device regression this specifically fixes: the
     * previous hiby_player.sh -> open_hiby_player chain already paid this
     * exact settle cost, but inside the PLAYER's own main(), called
     * "immediately after painting the splash, so this bounded wait
     * overlaps its existing 3-second minimum display time" (that
     * function's own comment) -- i.e. the user saw a splash the whole
     * time. Running the settle wait here, one process earlier, before any
     * frame had ever been drawn, would turn that same already-accepted
     * cost into up to ~2.5s of new, blank-screen latency instead -- same
     * number of seconds, but a materially different, worse experience,
     * not "moving an existing cost." Drawing a placeholder here first
     * makes it overlap something on-screen again, the same way the
     * player's own splash already did. */
    bool fb_ready = fb_open();
    if (fb_ready) {
        /* See BOOTLOADER_BG_PATH's own doc comment for why this isn't
         * /etc/logo1.jpeg directly. Decoded exactly once, here, before
         * anything else touches the screen; draw_menu()'s own redraws
         * reuse the cache via fb_restore_background() rather than
         * re-decoding this on every countdown tick. Falls back to a plain
         * fill if the asset is ever missing/unreadable/wrong-sized -- this
         * is cosmetic, not load-bearing, and must never block boot. */
        if (!fb_draw_background_jpeg(BOOTLOADER_BG_PATH)) fb_fill(COLOR_BG);
        draw_centered(FB_HEIGHT / 2 - fb_text_height() / 2, "STARTUP", COLOR_TEXT);
        fb_flush();
    }

    scan_result_t scan;
    scanner_scan(&scan);

    const char * boot_path;

    if (scan.sd_update_is_newer) {
        /* Auto-adopt path -- see scanner.h's own doc comment. No menu:
         * this is meant to feel like the internal player itself got
         * newer, not like a boot chooser appeared. Not a dual-boot
         * preference decision -- never touches the persisted default
         * below. */
        boot_path = SD_UPDATE_PLAYER_PATH;
    } else if (!scan.sd_stock_present && !scan.sd_update_present) {
        /* Nothing to choose between. Also not a preference decision: if
         * the SD card is only temporarily missing, this must not clobber
         * a previously-remembered non-Internal default just because
         * neither alternate was reachable this one boot. */
        boot_path = INTERNAL_PLAYER_PATH;
    } else if (scan.sd_update_present && !scan.sd_stock_present) {
        /* Real-device bug report: an SD update present but NOT stock
         * still showed the full menu/countdown, even though there is
         * nothing genuinely different to choose between -- Internal vs.
         * the same app copied to SD isn't a real dual-boot decision the
         * way Internal-vs-Stock is, and scanner_scan() already makes this
         * the default entry unconditionally whenever it's present (see
         * its own comment). Waiting through a countdown to confirm a
         * choice that was never actually in doubt is pure friction, not
         * caution -- boot it directly, same as the sd_update_is_newer
         * case above, regardless of whether it happens to be newer.
         * Distinct from that branch only in that boot_path is chosen for
         * a different reason (presence, not recency) -- also not a
         * preference decision, same reasoning as the branch above. */
        boot_path = SD_UPDATE_PLAYER_PATH;
    } else {
        int chosen_entry;
        if (!fb_ready) {
            /* Can't draw a menu at all -- still boot something rather than
             * sitting on a dead screen forever. Falls back to whatever the
             * persisted default was, without ever having shown a menu. */
            fprintf(stderr, "open_hiby_bootloader: fb not available, booting default entry with no menu\n");
            chosen_entry = scan.default_entry;
        } else {
            chosen_entry = run_menu(&scan);
            input_close();
        }
        switch (chosen_entry) {
            case BOOT_ENTRY_SD_STOCK: boot_path = SD_STOCK_PLAYER_PATH; break;
            case BOOT_ENTRY_SD_UPDATE: boot_path = SD_UPDATE_PLAYER_PATH; break;
            default: boot_path = INTERNAL_PLAYER_PATH; break;
        }
        /* Only persisted here -- this is the one branch where chosen_entry
         * reflects an actual (live or re-affirmed) dual-boot preference,
         * not a forced/automatic outcome. */
        scanner_save_last_boot(chosen_entry);
    }

    if (fb_ready) fb_close();
    run_player_supervised(boot_path);
    return 1; /* unreachable -- run_player_supervised() never returns */
}
