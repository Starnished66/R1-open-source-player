/* Bootloader entry: decides which player binary to run, displays the boot
 * selection menu if applicable, and supervises player execution. Reboots
 * on unexpected exits for crash recovery, or powers off on clean exit. */

#include "scanner.h"
#include "installer.h"
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

extern char ** environ;

/* The Stock player framebuffer backend requires a physically-contiguous 8 MiB
 * DMA allocation via /dev/sa_hgl_dma. Holding the fd reserves this block during
 * the boot menu so that memory fragmentation does not prevent Stock from
 * allocating its framebuffer.
 *
 * The reservation fd remains open across fork and is released by O_CLOEXEC
 * on successful execve(), ensuring the block remains allocated until the new
 * process takes over. */
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
/* Vertical offset for the first card below the title/countdown area. */
#define CARDS_TOP 110
/* Bottom offset for cards relative to FB_HEIGHT. */
#define CARDS_BOTTOM (FB_HEIGHT - 120)
#define CARD_GAP 24
#define MAX_CARDS 2 /* Internal and SD stock entries. */
#define TITLE_Y 36
#define COUNTDOWN_Y 68
#define PROGRESS_BAR_Y 92
#define PROGRESS_BAR_HEIGHT 6
#define FOOTER_Y (FB_HEIGHT - 100)

/* Card background alpha blending level (~50%). */
#define CARD_BG_ALPHA 128

/* Background JPEG image from theme assets used for the bootloader screen. */
#define BOOTLOADER_BG_PATH "/usr/resource/litegui/theme2/boot_animation/en/0.jpg"

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
    const char * line3;
    int boot_entry; /* a BOOT_ENTRY_* value (scanner.h) -- which real choice this card represents */
} card_layout_t;

/* Distributes card positions evenly within the CARDS_TOP..CARDS_BOTTOM band. */
static void layout_cards(card_layout_t * cards, int count) {
    int height = (CARDS_BOTTOM - CARDS_TOP - CARD_GAP * (count - 1)) / count;
    for (int i = 0; i < count; i++) {
        cards[i].y = CARDS_TOP + i * (height + CARD_GAP);
        cards[i].height = height;
    }
}

static void draw_centered(int y, const char * text, fb_color_t color) {
    fb_draw_text_centered(y, text, color);
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
    if (card->line3[0]) fb_draw_text(CARD_MARGIN_X + 30, card->y + 105, card->line3, COLOR_MUTED);
}

static bool point_in_card(const card_layout_t * card, int x, int y) {
    return x >= CARD_MARGIN_X && x < CARD_MARGIN_X + CARD_WIDTH && y >= card->y && y < card->y + card->height;
}

/* Draws menu cards and optional countdown bar. Pass remaining_ms < 0 to hide countdown. */
static void draw_menu(const card_layout_t * cards, int count, int selected, int remaining_ms, int timeout_ms) {
    fb_restore_background(COLOR_BG);
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

/* Returns milliseconds elapsed since the specified start timestamp. */
static long elapsed_ms_since(const struct timespec * start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

/* Populates menu cards based on detected player installations. */
static int build_cards(const scan_result_t * scan, card_layout_t * cards) {
    int count = 0;
    cards[count] = (card_layout_t) {
        .line1 = "OPEN PLAYER",
        .line2 = "INTERNAL",
        .line3 = scan->internal_build_stamp[0] ? scan->internal_build_stamp : "",
        .boot_entry = BOOT_ENTRY_INTERNAL,
    };
    count++;
    if (scan->sd_stock_present) {
        cards[count] = (card_layout_t) {
            .line1 = "STOCK PLAYER",
            .line2 = "SD CARD",
            .line3 = "",
            .boot_entry = BOOT_ENTRY_SD_STOCK,
        };
        count++;
    }
    layout_cards(cards, count);
    return count;
}

static void cancel_countdown_and_redraw(bool * countdown_active, const card_layout_t * cards, int card_count,
                                        int selected, int timeout_ms) {
    *countdown_active = false;
    draw_menu(cards, card_count, selected, -1, timeout_ms);
}

static int run_menu(const scan_result_t * scan) {
    card_layout_t cards[MAX_CARDS];
    int card_count = build_cards(scan, cards);

    input_open();
    int selected = scan->default_entry;
    int timeout_ms = scan->timeout_seconds * 1000;
    /* Start with countdown active so default entry boots even if input is unavailable. */
    bool countdown_active = true;

    /* Track elapsed time against monotonic clock deadline. */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    draw_menu(cards, card_count, selected, timeout_ms, timeout_ms);

    for (;;) {
        int remaining_ms = timeout_ms;
        if (countdown_active) {
            remaining_ms = timeout_ms - (int) elapsed_ms_since(&start);
            if (remaining_ms <= 0) return selected; /* countdown reached zero -- confirm whatever is currently highlighted */
        }

        /* Cap poll timeout to next redraw interval while counting down, or wait indefinitely once cancelled. */
        int poll_timeout_ms;
        if (!countdown_active) poll_timeout_ms = -1;
        else poll_timeout_ms = remaining_ms < 100 ? remaining_ms : 100;

        /* Check if any input device is currently open before polling. */
        bl_input_event_t ev;
        if (input_any_open()) {
            ev = input_poll(poll_timeout_ms);
        } else if (!countdown_active) {
            /* If all inputs closed after countdown was cancelled, proceed with selected entry. */
            return selected;
        } else {
            /* Sleep briefly when countdown is active but no input devices are open. */
            usleep((useconds_t) (poll_timeout_ms > 0 ? poll_timeout_ms : 100) * 1000);
            ev = (bl_input_event_t) { BL_INPUT_NONE, 0, 0 };
        }

        if (ev.type == BL_INPUT_MOVE_UP || ev.type == BL_INPUT_MOVE_DOWN) {
            /* Cycles through however many cards are actually present, by
             * ARRAY POSITION, not by BOOT_ENTRY_* value -- kept
             * position-based rather than assuming the two always coincide,
             * since build_cards() still only adds a card when its own
             * presence flag is set. Find the current card's index, step
             * it, wrap around. */
            int idx = 0;
            for (int i = 0; i < card_count; i++) {
                if (cards[i].boot_entry == selected) { idx = i; break; }
            }
            idx = (ev.type == BL_INPUT_MOVE_DOWN) ? (idx + 1) % card_count : (idx - 1 + card_count) % card_count;
            selected = cards[idx].boot_entry;
            cancel_countdown_and_redraw(&countdown_active, cards, card_count, selected, timeout_ms);
        } else if (ev.type == BL_INPUT_CONFIRM) {
            return selected;
        } else if (ev.type == BL_INPUT_TOUCH_DOWN) {
            /* Cancels the countdown only -- selection/confirmation happens
             * on release (BL_INPUT_TOUCH_TAP below), same as before. This
             * just stops the deadline from expiring out from under a
             * finger already resting on a card (see BL_INPUT_TOUCH_DOWN's
             * own doc comment in input.h). */
            cancel_countdown_and_redraw(&countdown_active, cards, card_count, selected, timeout_ms);
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
            cancel_countdown_and_redraw(&countdown_active, cards, card_count, selected, timeout_ms);
        } else if (countdown_active) {
            draw_menu(cards, card_count, selected, remaining_ms, timeout_ms);
        }
    }
}

/* Supervises player execution. A clean exit status (0) triggers device poweroff,
 * while abnormal termination (signals or non-zero exit) triggers a reboot. */
static void reboot_device(void) {
    sleep(1);
    sync();
    reboot(RB_AUTOBOOT);
    /* reboot() is expected to terminate the process; exit if it returns. */
    _exit(1);
}

static void poweroff_device(void) {
    sync();
    reboot(RB_POWER_OFF);
    /* If poweroff syscall fails, pause indefinitely rather than rebooting. */
    perror("open_hiby_bootloader: poweroff syscall failed");
    for (;;) pause();
}

static void run_player_supervised(const char * player_path) {
    pid_t pid = fork();
    if (pid < 0) {
        /* If fork fails, attempt direct execve before rebooting. */
        perror("open_hiby_bootloader: fork failed, execve'ing directly (no reboot-on-crash this launch)");
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        perror("open_hiby_bootloader: execve failed");
        reboot_device();
    }

    if (pid == 0) {
        /* HGL reservation remains open in child until released via O_CLOEXEC on execve. */
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        /* Fall back to internal player if selected binary execve fails. */
        perror("open_hiby_bootloader: execve failed, falling back to internal player");
        if (strcmp(player_path, INTERNAL_PLAYER_PATH) != 0) {
            execve(INTERNAL_PLAYER_PATH, (char * []) { (char *) INTERNAL_PLAYER_PATH, NULL }, environ);
        }
        _exit(127);
    }

    /* Close supervisor's reference to HGL DMA reservation; child holds its own copy. */
    release_stock_hgl_dma();

    /* Wait for child process, retrying on EINTR. */
    int status;
    pid_t reaped;
    do {
        reaped = waitpid(pid, &status, 0);
    } while (reaped == -1 && errno == EINTR);

    if (reaped != pid) {
        /* Child wait failed unexpectedly; fall through to reboot. */
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
    /* Reserve Stock HGL DMA before other allocations. */
    reserve_stock_hgl_dma();

    /* Open framebuffer and paint splash background before waiting on SD card settle. */
    bool fb_ready = fb_open();
    if (fb_ready) {
        /* Paint cached background image or fill background on error. */
        if (!fb_draw_background_jpeg(BOOTLOADER_BG_PATH)) fb_fill(COLOR_BG);
        fb_flush();
    }

    scan_result_t scan;
    scanner_scan(&scan);

    /* Run installer if an SD player update is present. */
    bool sd_update_was_present = scan.sd_update_present;
    installer_run(&scan, fb_ready);

    const char * internal_path = installer_internal_player_path();
    if (strcmp(internal_path, INTERNAL_PLAYER_PATH) != 0) {
        /* Update build stamp if installed update overrides internal squashfs copy. */
        char stamp[BOOT_BUILD_STAMP_LEN + 1];
        if (scanner_read_build_stamp(internal_path, stamp, sizeof(stamp))) {
            memcpy(scan.internal_build_stamp, stamp, sizeof(stamp));
        } else {
            scan.internal_build_stamp[0] = '\0';
        }
    }

    const char * boot_path;

    if (!scan.sd_stock_present) {
        /* Boot internal player directly when no Stock player is present on SD. */
        boot_path = internal_path;
    } else {
        int chosen_entry;
        if (!fb_ready) {
            /* Boot default entry without showing menu if framebuffer failed. */
            fprintf(stderr, "open_hiby_bootloader: fb not available, booting default entry with no menu\n");
            chosen_entry = scan.default_entry;
        } else {
            chosen_entry = run_menu(&scan);
            input_close();
        }
        boot_path = (chosen_entry == BOOT_ENTRY_SD_STOCK) ? SD_STOCK_PLAYER_PATH : internal_path;
    }

    /* Drop SD update page cache before launching Stock player to free memory. */
    if (strcmp(boot_path, SD_STOCK_PLAYER_PATH) == 0 && sd_update_was_present) {
        scanner_drop_sd_update_cache();
    }
    if (fb_ready) fb_close();
    run_player_supervised(boot_path);
    return 1; /* unreachable -- run_player_supervised() never returns */
}
