/* Executing a player binary straight off the SD card's own block device
 * (this app's original design, and Stock's own long-standing one) maps that
 * binary's code pages from removable media. Pulling the card mid-run, or
 * even a quick reinsert cycle, can invalidate those mappings out from under
 * a still-running process -- a real, reproduced-on-device SIGBUS, not a
 * theoretical concern (see gui_library.c's own LIBRARY_RESCAN_THREAD_STACK_
 * SIZE comment for the investigation that first surfaced SD-media fragility
 * as a real crash source in this app, albeit via a different mechanism).
 * The fix here is structural rather than defensive: never map player code
 * from the SD card at all. SD_UPDATE_PLAYER_PATH (scanner.h) is now only
 * ever a SOURCE to copy from, once, into a durable file on the same
 * writable partition BOOT_PREF_PATH already lives on -- INSTALLED_PLAYER_
 * PATH, below -- and every boot after that runs from there or from the
 * always-present squashfs INTERNAL_PLAYER_PATH, never from removable media.
 *
 * The copy itself follows this project's own established durable-write
 * idiom (temp file, fsync, atomic rename -- see scanner_save_last_boot()'s
 * own doc comment, and albumart.c's albumart_store_rgb565() for the same
 * pattern in the main app): the SD source file is deleted only after the
 * new internal copy has been verified byte-for-byte and made durable, so a
 * power loss or SD removal at any point during the copy leaves the device
 * exactly as bootable as it was before this ran -- either the SD file is
 * still there to retry from, or the install already completed and the SD
 * file's removal is the only step left outstanding. */

#define _POSIX_C_SOURCE 200809L

#include "installer.h"
#include "fb_draw.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Same directory INSTALLED_PLAYER_PATH itself lives in -- statvfs()'d for
 * free space and fsync()'d as a plain directory fd once the rename below
 * has landed. */
#define INSTALL_DIR "/usr/data"

/* Fixed name, not mkstemp() -- this project already uses a fixed
 * ".tmp"-suffixed sibling for exactly this purpose (scanner_save_last_boot()).
 * Never opened for read by anything else in this file or by
 * installer_internal_player_path(), so a stale leftover from an interrupted
 * previous attempt is simply overwritten (O_TRUNC) the next time this runs,
 * never mistaken for an installed binary. */
#define INSTALL_TMP_PATH "/usr/data/.open_hiby_player.installing"

static bool path_is_executable_file(const char * path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* Plain, table-driven CRC-32 (the standard reflected 0xEDB88320 polynomial)
 * used only to confirm two files' CONTENT matches -- detecting a truncated
 * or corrupted copy (SD removed mid-read, a flipped bit, ENOSPC cutting a
 * write short), not authenticating the file against tampering. This
 * bootloader intentionally links nothing beyond tjpgd (see BOOTLOADER_SRCS'
 * own comment in the Makefile) -- pulling in mbedtls's MD5/SHA (already
 * vendored for the main player's own network/plugin code) into this small,
 * boot-critical binary for a corruption check alone isn't warranted. The
 * table is built once, lazily, into a function-local static: this file only
 * ever runs the check on the rare boot where an SD update is actually
 * present, never on the common no-update boot path. */
static uint32_t crc32_table[256];
static bool crc32_table_ready = false;

static void crc32_init(void) {
    if (crc32_table_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = true;
}

static bool file_crc32_and_size(const char * path, uint32_t * out_crc, off_t * out_size) {
    crc32_init();
    FILE * f = fopen(path, "rb");
    if (!f) return false;

    uint32_t crc = 0xFFFFFFFFu;
    unsigned char buf[65536];
    size_t n;
    off_t total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++) crc = crc32_table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
        total += (off_t) n;
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return false;

    *out_crc = crc ^ 0xFFFFFFFFu;
    *out_size = total;
    return true;
}

static bool fsync_path(const char * path, bool is_dir) {
    int fd = open(path, O_RDONLY | (is_dir ? O_DIRECTORY : 0));
    if (fd < 0) {
        fprintf(stderr, "installer: open(%s) for fsync failed: %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fsync(fd) == 0;
    if (!ok) fprintf(stderr, "installer: fsync(%s) failed: %s\n", path, strerror(errno));
    if (close(fd) != 0) ok = false;
    return ok;
}

/* Plain byte copy, src to dst (dst created/truncated fresh). Any short read
 * or short/failed write -- SD card pulled mid-copy, ENOSPC, an unrelated
 * I/O error -- reports failure; the caller then discards dst rather than
 * ever treating a partial copy as usable. */
static bool copy_file(const char * src_path, const char * dst_path) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "installer: open(%s) failed: %s\n", src_path, strerror(errno));
        return false;
    }
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        fprintf(stderr, "installer: open(%s) failed: %s\n", dst_path, strerror(errno));
        close(src_fd);
        return false;
    }

    unsigned char buf[65536];
    bool ok = true;
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "installer: read(%s) failed: %s\n", src_path, strerror(errno));
            ok = false;
            break;
        }
        if (n == 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(dst_fd, buf + off, (size_t) (n - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "installer: write(%s) failed: %s\n", dst_path, strerror(errno));
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok) break;
    }

    if (close(dst_fd) != 0) ok = false;
    close(src_fd);
    return ok;
}

static uint16_t read_u16le(const unsigned char * p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t read_u32le(const unsigned char * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* Confirms the copied file is a complete, loadable MIPS32LE executable ELF
 * -- a structural/truncation check independent of file_crc32_and_size()
 * above, which only proves the copy matches whatever bytes came off the SD
 * card: a source file truncated (or otherwise corrupted) to exactly the
 * right shorter length copies "accurately" just the same. This app's own
 * toolchain (mipsel-linux-musl-gcc, -static -no-pie) always produces a
 * fixed-size 52-byte ELFCLASS32/ELFDATA2LSB/ET_EXEC/EM_MIPS header
 * (confirmed against this project's own unstripped build output) followed
 * immediately by a 32-byte-entry program-header table -- both boards this
 * project currently supports (r1, r3proii) share the same X1600 MIPS SoC
 * family, so none of this is board-specific.
 *
 * Checking only those fixed-offset header bytes (as an earlier version of
 * this function did) still passes a file truncated to just past them: the
 * header alone doesn't prove anything about what should follow it. This
 * additionally requires the program-header table itself to fit within the
 * actual file size, and every PT_LOAD segment's [p_offset, p_offset +
 * p_filesz) range to fit within it too -- a real executable's loadable
 * code/data cannot lie beyond the file's own end. Deliberately not a full
 * ELF parse (no section headers, no relocation/symbol validation, no
 * segment CONTENT inspection): this bootloader intentionally avoids
 * pulling in a real ELF library (see this file's own top comment on why
 * mbedtls was skipped for the same reason) and this is not a security
 * boundary, only a guard against installing something that could never
 * have actually run. */
static bool validate_player_elf(const char * path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    uint64_t file_size = (uint64_t) st.st_size;

    enum { ELF32_EHDR_SIZE = 52, ELF32_PHDR_SIZE = 32 };
    if (file_size < ELF32_EHDR_SIZE) return false;

    FILE * f = fopen(path, "rb");
    if (!f) return false;

    unsigned char ehdr[ELF32_EHDR_SIZE];
    bool ok = fread(ehdr, 1, sizeof(ehdr), f) == sizeof(ehdr);

    if (ok && memcmp(ehdr, "\x7f"
                           "ELF",
                     4) != 0)
        ok = false;
    if (ok && ehdr[4] != 1) ok = false; /* ELFCLASS32 */
    if (ok && ehdr[5] != 1) ok = false; /* ELFDATA2LSB */

    uint32_t e_phoff = 0;
    uint16_t e_phentsize = 0, e_phnum = 0;
    if (ok) {
        uint16_t e_type = read_u16le(ehdr + 16);
        uint16_t e_machine = read_u16le(ehdr + 18);
        if (e_type != 2 /* ET_EXEC */) ok = false;
        if (e_machine != 8 /* EM_MIPS */) ok = false;
        e_phoff = read_u32le(ehdr + 28);
        e_phentsize = read_u16le(ehdr + 42);
        e_phnum = read_u16le(ehdr + 44);
    }

    /* The program-header table must itself lie entirely within the file --
     * a copy truncated right after a byte-for-byte-intact ELF header would
     * otherwise pass everything checked so far. */
    if (ok && (e_phnum == 0 || e_phentsize < ELF32_PHDR_SIZE)) ok = false;
    if (ok) {
        uint64_t phtable_end = (uint64_t) e_phoff + (uint64_t) e_phentsize * (uint64_t) e_phnum;
        if (phtable_end > file_size) ok = false;
    }

    bool saw_load_segment = false;
    for (uint16_t i = 0; ok && i < e_phnum; i++) {
        unsigned char phdr[ELF32_PHDR_SIZE];
        if (fseek(f, (long) (e_phoff + (uint64_t) i * e_phentsize), SEEK_SET) != 0) {
            ok = false;
            break;
        }
        if (fread(phdr, 1, sizeof(phdr), f) != sizeof(phdr)) {
            ok = false;
            break;
        }
        uint32_t p_type = read_u32le(phdr + 0);
        uint32_t p_offset = read_u32le(phdr + 4);
        uint32_t p_filesz = read_u32le(phdr + 16);
        if (p_type == 1 /* PT_LOAD */) {
            saw_load_segment = true;
            /* Every loadable segment's own file-backed bytes must fit
             * within the actual (not merely declared) file size -- this is
             * the check that actually catches a truncated copy: a short
             * file can still have a fully in-bounds, well-formed program-
             * header table sitting right after the (also intact) ELF
             * header while the LOAD segment(s) the table describes point
             * past where the real file data actually ends. */
            if ((uint64_t) p_offset + (uint64_t) p_filesz > file_size) ok = false;
        }
    }
    if (ok && !saw_load_segment) ok = false; /* no loadable segments at all -- not a real executable */

    fclose(f);
    return ok;
}

static void draw_updating_screen(void) {
    fb_restore_background(fb_rgb(0x12, 0x12, 0x12));
    fb_color_t white = fb_rgb(0xFF, 0xFF, 0xFF);
    const char * line1 = "UPDATING PLAYER";
    const char * line2 = "DO NOT REMOVE SD CARD";
    int th = fb_text_height();
    fb_draw_text((FB_WIDTH - fb_text_width(line1)) / 2, FB_HEIGHT / 2 - th, line1, white);
    fb_draw_text((FB_WIDTH - fb_text_width(line2)) / 2, FB_HEIGHT / 2 + th / 2, line2, white);
    fb_flush();
}

void installer_run(const scan_result_t * scan, bool fb_ready) {
    if (!scan->sd_update_present) return;

    uint32_t sd_crc;
    off_t sd_size;
    if (!file_crc32_and_size(SD_UPDATE_PLAYER_PATH, &sd_crc, &sd_size)) {
        fprintf(stderr, "installer: could not read %s -- leaving it for a later boot\n", SD_UPDATE_PLAYER_PATH);
        return;
    }

    /* Content-identical to what is already installed: either nothing has
     * actually changed, or a previous install completed but its own SD-file
     * cleanup below didn't. Either way, never repeat the copy/verify/install
     * work -- only retry the cleanup, so a redundant SD copy eventually
     * stops being carried around without ever being reinstalled needlessly. */
    if (path_is_executable_file(INSTALLED_PLAYER_PATH)) {
        uint32_t inst_crc;
        off_t inst_size;
        if (file_crc32_and_size(INSTALLED_PLAYER_PATH, &inst_crc, &inst_size) && inst_size == sd_size &&
            inst_crc == sd_crc) {
            /* Confirm the installed copy's directory entry is durable
             * before destroying the only other copy of it -- the SD file
             * is the sole recovery path if a power loss right after this
             * point turns out to have never made that entry durable in the
             * first place (e.g. an earlier boot's own post-rename fsync,
             * below, failed and was never retried until now). Not fatal:
             * just try again next boot, same as any other install failure. */
            if (!fsync_path(INSTALL_DIR, true)) {
                fprintf(stderr,
                        "installer: %s already installed but directory sync failed -- retrying its SD cleanup "
                        "later\n",
                        INSTALLED_PLAYER_PATH);
                return;
            }
            if (unlink(SD_UPDATE_PLAYER_PATH) != 0) {
                fprintf(stderr, "installer: %s is already installed; retrying its SD cleanup failed: %s\n",
                        INSTALLED_PLAYER_PATH, strerror(errno));
            } else {
                fsync_path(SD_ALT_DIR, true);
                fprintf(stderr, "installer: %s already installed; finished deferred SD cleanup\n",
                        INSTALLED_PLAYER_PATH);
            }
            return;
        }
    }

    /* Reclaim any leftover temp file from an interrupted previous attempt
     * BEFORE measuring free space -- statvfs() below sees this file's
     * space as already spoken for until it's actually gone, so on a
     * nearly-full partition a stale, never-cleaned-up .installing file
     * could fail every subsequent retry's space check even though the
     * space it holds would be enough once reclaimed. copy_file() below
     * would truncate this same path anyway, but that happens after the
     * check this exists to fix. */
    unlink(INSTALL_TMP_PATH);

    struct statvfs vfs;
    if (statvfs(INSTALL_DIR, &vfs) != 0) {
        fprintf(stderr, "installer: statvfs(%s) failed: %s -- leaving SD update for a later boot\n", INSTALL_DIR,
                strerror(errno));
        return;
    }
    if ((uint64_t) vfs.f_bavail * (uint64_t) vfs.f_bsize < (uint64_t) sd_size) {
        fprintf(stderr, "installer: not enough free space on %s for the SD update -- leaving it for a later boot\n",
                INSTALL_DIR);
        return;
    }

    if (fb_ready) draw_updating_screen();

    if (!copy_file(SD_UPDATE_PLAYER_PATH, INSTALL_TMP_PATH)) {
        fprintf(stderr, "installer: copy failed -- leaving SD update for a later boot\n");
        unlink(INSTALL_TMP_PATH);
        return;
    }

    uint32_t tmp_crc;
    off_t tmp_size;
    if (!file_crc32_and_size(INSTALL_TMP_PATH, &tmp_crc, &tmp_size) || tmp_size != sd_size || tmp_crc != sd_crc) {
        fprintf(stderr, "installer: copied file failed validation -- leaving SD update for a later boot\n");
        unlink(INSTALL_TMP_PATH);
        return;
    }

    if (!validate_player_elf(INSTALL_TMP_PATH)) {
        fprintf(stderr,
                "installer: %s is not a valid MIPS executable -- refusing to replace the installed player, leaving "
                "SD update for a later boot\n",
                SD_UPDATE_PLAYER_PATH);
        unlink(INSTALL_TMP_PATH);
        return;
    }

    if (chmod(INSTALL_TMP_PATH, 0755) != 0) {
        fprintf(stderr, "installer: chmod(%s) failed: %s -- leaving SD update for a later boot\n", INSTALL_TMP_PATH,
                strerror(errno));
        unlink(INSTALL_TMP_PATH);
        return;
    }

    if (!fsync_path(INSTALL_TMP_PATH, false)) {
        fprintf(stderr, "installer: leaving SD update for a later boot\n");
        unlink(INSTALL_TMP_PATH);
        return;
    }

    if (rename(INSTALL_TMP_PATH, INSTALLED_PLAYER_PATH) != 0) {
        fprintf(stderr, "installer: rename to %s failed: %s -- leaving SD update for a later boot\n",
                INSTALLED_PLAYER_PATH, strerror(errno));
        unlink(INSTALL_TMP_PATH);
        return;
    }
    /* The new binary is already in place and bootable either way from this
     * point on -- but its directory entry is not yet confirmed durable, and
     * the SD copy is the only other copy that exists. Require the fsync to
     * succeed before deleting that copy: a power interruption between an
     * unsynced rename and the next boot could otherwise lose the rename
     * (leaving no installed copy at all) at the same time as the SD source
     * (leaving nothing to reinstall from). A failed sync here just retries
     * -- both the sync and the SD cleanup -- next boot, via the
     * content-identical path above. */
    if (!fsync_path(INSTALL_DIR, true)) {
        fprintf(stderr, "installer: install completed but directory sync failed -- leaving SD update for a later "
                        "boot\n");
        return;
    }

    if (unlink(SD_UPDATE_PLAYER_PATH) != 0) {
        fprintf(stderr, "installer: install succeeded but SD cleanup failed: %s -- will retry next boot\n",
                strerror(errno));
    } else {
        fsync_path(SD_ALT_DIR, true);
    }

    fprintf(stderr, "installer: installed SD update to %s\n", INSTALLED_PLAYER_PATH);
}

const char * installer_internal_player_path(void) {
    return path_is_executable_file(INSTALLED_PLAYER_PATH) ? INSTALLED_PLAYER_PATH : INTERNAL_PLAYER_PATH;
}
