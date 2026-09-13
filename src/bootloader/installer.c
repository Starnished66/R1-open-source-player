/* Copies updates from SD storage to internal storage (INSTALLED_PLAYER_PATH)
 * rather than executing directly from removable media, avoiding invalid
 * memory mappings if the SD card is removed.
 *
 * Writes to a temporary file, verifies integrity, fsyncs, and atomically
 * renames before removing the source SD file. */

#define _POSIX_C_SOURCE 200809L

#include "installer.h"
#include "scanner.h"
#include "fb_draw.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Target installation directory, checked for free space and fsynced after rename. */
#define INSTALL_DIR "/usr/data"

/* Temporary path used during installation before atomic rename. */
#define INSTALL_TMP_PATH "/usr/data/.open_hiby_player.installing"

/* Table-driven CRC-32 (standard reflected 0xEDB88320 polynomial) used to
 * verify copy integrity and detect truncation or file corruption.
 * The table is lazily initialized on first use. */
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

/* Copies bytes from src to dst. Returns false on short read or write failure. */
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

/* Confirms the copied file is a complete, loadable MIPS32LE executable ELF.
 * Validates ELF header fields, ensures the program header table fits within
 * the file, and verifies that all PT_LOAD segment ranges are bounded by the
 * actual file size to guard against truncated binaries. */
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

    /* The program-header table must lie entirely within the file. */
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
            /* Ensure loadable segment bytes fit within actual file size. */
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
    fb_draw_text_centered(FB_HEIGHT / 2 - th, line1, white);
    fb_draw_text_centered(FB_HEIGHT / 2 + th / 2, line2, white);
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

    /* If the SD update is byte-identical to the installed player, skip
     * installation and complete any pending cleanup of the SD update file. */
    if (scanner_path_is_executable(INSTALLED_PLAYER_PATH)) {
        uint32_t inst_crc;
        off_t inst_size;
        if (file_crc32_and_size(INSTALLED_PLAYER_PATH, &inst_crc, &inst_size) && inst_size == sd_size &&
            inst_crc == sd_crc) {
            /* Ensure installed directory entry is durable before removing SD file. */
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

    /* Remove any leftover temporary file before checking available space. */
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
    /* Ensure directory entry is durable before deleting the SD update file. */
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
    return scanner_path_is_executable(INSTALLED_PLAYER_PATH) ? INSTALLED_PLAYER_PATH : INTERNAL_PLAYER_PATH;
}
