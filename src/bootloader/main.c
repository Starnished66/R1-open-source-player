/* Bootloader entry: installs a pending SD update, then supervises the internal
 * Compás player. Reboots on unexpected exits for crash recovery, or powers
 * off on a clean exit. */

#include "scanner.h"
#include "installer.h"
#include "fb_draw.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <unistd.h>

extern char ** environ;

/* Background JPEG image from theme assets used for the boot splash. */
#define BOOTLOADER_BG_PATH "/usr/resource/litegui/theme2/boot_animation/en/0.jpg"

#define COLOR_BG fb_rgb(0x12, 0x12, 0x12)

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
    perror("compas_bootloader: poweroff syscall failed");
    for (;;) pause();
}

static void run_player_supervised(const char * player_path) {
    pid_t pid = fork();
    if (pid < 0) {
        /* If fork fails, attempt direct execve before rebooting. */
        perror("compas_bootloader: fork failed, execve'ing directly (no reboot-on-crash this launch)");
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        perror("compas_bootloader: execve failed");
        reboot_device();
    }

    if (pid == 0) {
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        /* Fall back to the packaged internal player if the installed copy fails. */
        perror("compas_bootloader: execve failed, falling back to packaged player");
        if (strcmp(player_path, INTERNAL_PLAYER_PATH) != 0) {
            execve(INTERNAL_PLAYER_PATH, (char * []) { (char *) INTERNAL_PLAYER_PATH, NULL }, environ);
        }
        _exit(127);
    }

    /* Wait for child process, retrying on EINTR. */
    int status;
    pid_t reaped;
    do {
        reaped = waitpid(pid, &status, 0);
    } while (reaped == -1 && errno == EINTR);

    if (reaped != pid) {
        /* Child wait failed unexpectedly; fall through to reboot. */
        perror("compas_bootloader: waitpid failed unexpectedly");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "compas_bootloader: %s exited cleanly -- powering off\n", player_path);
        poweroff_device();
    } else {
        fprintf(stderr, "compas_bootloader: %s exited abnormally (status=0x%x) -- rebooting\n",
                player_path, (unsigned) status);
    }
    reboot_device();
}

int main(void) {
    /* Open framebuffer and paint the splash before waiting for the SD card. */
    bool fb_ready = fb_open();
    if (fb_ready) {
        if (!fb_draw_background_jpeg(BOOTLOADER_BG_PATH)) fb_fill(COLOR_BG);
        fb_flush();
    }

    scan_result_t scan;
    scanner_scan(&scan);

    /* Install a pending SD update before choosing the internal player copy. */
    installer_run(&scan, fb_ready);

    const char * internal_path = installer_internal_player_path();
    if (fb_ready) fb_close();
    run_player_supervised(internal_path);
    return 1; /* unreachable -- run_player_supervised() never returns */
}
