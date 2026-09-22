#include "sd_fsck.h"

#include <stdio.h>
#include <string.h>

static bool unescape_field(const char * in, size_t length, char * out, size_t out_size) {
    size_t written = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char value;
        if (in[i] == '\\' && i + 3 < length &&
            in[i + 1] >= '0' && in[i + 1] <= '7' &&
            in[i + 2] >= '0' && in[i + 2] <= '7' &&
            in[i + 3] >= '0' && in[i + 3] <= '7') {
            value = (unsigned char) ((in[i + 1] - '0') * 64 + (in[i + 2] - '0') * 8 + (in[i + 3] - '0'));
            i += 3;
        } else {
            value = (unsigned char) in[i];
        }
        if (written + 1 >= out_size) return false;
        out[written++] = (char) value;
    }
    out[written] = '\0';
    return true;
}

static bool option_token(const char * options, const char * token) {
    size_t token_len = strlen(token);
    const char * cursor = options;
    while (*cursor) {
        const char * comma = strchr(cursor, ',');
        size_t length = comma ? (size_t) (comma - cursor) : strlen(cursor);
        if (length == token_len && memcmp(cursor, token, token_len) == 0) return true;
        if (!comma) break;
        cursor = comma + 1;
    }
    return false;
}

static bool sd_mount_point(const char * path) {
    return strcmp(path, "/data/mnt/sd_0") == 0 || strcmp(path, "/usr/data/mnt/sd_0") == 0;
}

sd_fs_kind_t sd_fsck_kind_from_type(const char * fstype) {
    if (!fstype || !fstype[0]) return SD_FS_KIND_UNKNOWN;
    if (strcmp(fstype, "vfat") == 0 || strcmp(fstype, "msdos") == 0 || strcmp(fstype, "fat") == 0)
        return SD_FS_KIND_VFAT;
    if (strcmp(fstype, "exfat") == 0 || strcmp(fstype, "fuse.exfat") == 0) return SD_FS_KIND_EXFAT;
    if (strcmp(fstype, "ntfs") == 0 || strcmp(fstype, "ntfs3") == 0 || strcmp(fstype, "fuseblk") == 0 ||
        strcmp(fstype, "fuse.ntfs-3g") == 0 || strcmp(fstype, "ntfs-3g") == 0)
        return SD_FS_KIND_NTFS;
    return SD_FS_KIND_UNKNOWN;
}

bool sd_fsck_device_allowed(const char * device) {
    return device && (strcmp(device, "/dev/mmcblk0") == 0 || strcmp(device, "/dev/mmcblk0p1") == 0);
}

bool sd_fsck_parse_mounts(const char * mounts_text, sd_mount_info_t * out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!mounts_text) return false;

    const char * line = mounts_text;
    while (*line) {
        const char * next = strchr(line, '\n');
        size_t line_len = next ? (size_t) (next - line) : strlen(line);
        const char * fields[4];
        size_t field_len[4];
        size_t count = 0;
        size_t start = 0;
        for (size_t i = 0; i <= line_len && count < 4; i++) {
            bool boundary = i == line_len || line[i] == ' ' || line[i] == '\t';
            if (!boundary) continue;
            if (i > start) {
                fields[count] = line + start;
                field_len[count] = i - start;
                count++;
            }
            start = i + 1;
        }
        char device[SD_FSCK_DEVICE_BYTES];
        char mount_point[SD_FSCK_MOUNT_BYTES];
        char fstype[SD_FSCK_FSTYPE_BYTES];
        char options[512];
        if (count == 4 &&
            unescape_field(fields[0], field_len[0], device, sizeof(device)) &&
            unescape_field(fields[1], field_len[1], mount_point, sizeof(mount_point)) &&
            unescape_field(fields[2], field_len[2], fstype, sizeof(fstype)) &&
            unescape_field(fields[3], field_len[3], options, sizeof(options)) &&
            sd_mount_point(mount_point)) {
            out->found = true;
            out->readonly = option_token(options, "ro") && !option_token(options, "rw");
            out->kind = sd_fsck_kind_from_type(fstype);
            snprintf(out->device, sizeof(out->device), "%s", device);
            snprintf(out->mount_point, sizeof(out->mount_point), "%s", mount_point);
            snprintf(out->fstype, sizeof(out->fstype), "%s", fstype);
        }
        if (!next) break;
        line = next + 1;
    }
    return out->found;
}

static const char * const vfat_tools[] = {
    "/usr/sbin/fsck.vfat", "/usr/sbin/fsck.fat", "/sbin/fsck.vfat", NULL
};
static const char * const exfat_tools[] = { "/usr/sbin/fsck.exfat", "/sbin/fsck.exfat", NULL };
static const char * const ntfs_tools[] = { "/usr/sbin/ntfsfix", "/usr/bin/ntfsfix", NULL };

const char * const * sd_fsck_tool_candidates(sd_fs_kind_t kind) {
    if (kind == SD_FS_KIND_VFAT) return vfat_tools;
    if (kind == SD_FS_KIND_EXFAT) return exfat_tools;
    if (kind == SD_FS_KIND_NTFS) return ntfs_tools;
    return NULL;
}

const char * sd_fsck_select_tool(sd_fs_kind_t kind, bool (* exists)(const char * path, void * ctx), void * ctx) {
    const char * const * candidates = sd_fsck_tool_candidates(kind);
    if (!candidates || !exists) return NULL;
    for (size_t i = 0; candidates[i]; i++) {
        if (exists(candidates[i], ctx)) return candidates[i];
    }
    return NULL;
}

bool sd_fsck_plan(sd_fs_kind_t kind, const char * tool, const char * device, sd_fsck_plan_t * out) {
    if (!out || !tool || !tool[0] || !sd_fsck_device_allowed(device)) return false;
    if (kind != SD_FS_KIND_VFAT && kind != SD_FS_KIND_EXFAT && kind != SD_FS_KIND_NTFS) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->tool, sizeof(out->tool), "%s", tool);
    snprintf(out->device, sizeof(out->device), "%s", device);
    out->argv[0] = out->tool;
    if (kind == SD_FS_KIND_VFAT) {
        snprintf(out->arg1, sizeof(out->arg1), "-a");
        snprintf(out->arg2, sizeof(out->arg2), "-w");
        out->argv[1] = out->arg1;
        out->argv[2] = out->arg2;
        out->argv[3] = out->device;
        out->argv[4] = NULL;
    } else if (kind == SD_FS_KIND_EXFAT) {
        snprintf(out->arg1, sizeof(out->arg1), "-y");
        out->argv[1] = out->arg1;
        out->argv[2] = out->device;
        out->argv[3] = NULL;
    } else {
        out->argv[1] = out->device;
        out->argv[2] = NULL;
    }
    return true;
}

bool sd_fsck_exit_usable(int exit_code) {
    return exit_code >= 0 && exit_code <= 255 && (exit_code & ~0x3) == 0;
}

void sd_repair_attempt_key(const char * device, const char * cid, char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (cid && cid[0]) snprintf(out, out_size, "cid:%s", cid);
    else snprintf(out, out_size, "dev:%s", device ? device : "");
}
