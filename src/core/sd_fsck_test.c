#include "sd_fsck.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(int condition, const char * what) {
    if (condition) return;
    fprintf(stderr, "sd fsck: %s\n", what);
    failures++;
}

static bool only_overlay_tools(const char * path, void * ctx) {
    (void) ctx;
    return strcmp(path, "/usr/sbin/fsck.vfat") == 0 || strcmp(path, "/usr/sbin/fsck.exfat") == 0 ||
           strcmp(path, "/usr/sbin/ntfsfix") == 0;
}

int main(void) {
    sd_mount_info_t info;
    const char * writable =
        "/dev/mmcblk0p1 /data/mnt/sd_0 vfat rw,relatime,fmask=0022,errors=remount-ro 0 0\n";
    expect(sd_fsck_parse_mounts(writable, &info), "writable line found");
    expect(info.found && !info.readonly && info.kind == SD_FS_KIND_VFAT, "errors=remount-ro is not a read-only mount");
    expect(strcmp(info.device, "/dev/mmcblk0p1") == 0, "partition device");

    const char * readonly =
        "proc /proc proc rw,nosuid 0 0\n"
        "/dev/mmcblk0p1 /usr/data/mnt/sd_0 vfat ro,relatime,errors=remount-ro 0 0\n";
    expect(sd_fsck_parse_mounts(readonly, &info) && info.readonly, "alias mount is read-only");
    expect(strcmp(info.mount_point, "/usr/data/mnt/sd_0") == 0, "alias path kept");

    const char * replaced =
        "/dev/mmcblk0p1 /data/mnt/sd_0 vfat ro,relatime 0 0\n"
        "/dev/mmcblk0 /data/mnt/sd_0 exfat rw,relatime 0 0\n";
    expect(sd_fsck_parse_mounts(replaced, &info) && !info.readonly && info.kind == SD_FS_KIND_EXFAT, "last mount wins");
    expect(strcmp(info.device, "/dev/mmcblk0") == 0, "whole-disk device");

    const char * ntfs =
        "/dev/mmcblk0p1 /data/mnt/sd_0 fuseblk ro,relatime,user_id=0 0 0\n";
    expect(sd_fsck_parse_mounts(ntfs, &info) && info.readonly && info.kind == SD_FS_KIND_NTFS, "ntfs-3g fuse mount");

    const char * escaped =
        "/dev/mmcblk0p1 /data/mnt/sd\\0400 exfat ro 0 0\n"
        "/dev/mmcblk0p1 /data/mnt/sd_0 exfat ro,relatime 0 0\n";
    expect(sd_fsck_parse_mounts(escaped, &info) && info.found && info.kind == SD_FS_KIND_EXFAT, "escaped unrelated path ignored");

    expect(!sd_fsck_parse_mounts("tmpfs /tmp tmpfs rw 0 0\n", &info) && !info.found, "other mounts are ignored");
    expect(!sd_fsck_device_allowed("/dev/sda1") && sd_fsck_device_allowed("/dev/mmcblk0"), "device allow list");

    expect(strcmp(sd_fsck_select_tool(SD_FS_KIND_VFAT, only_overlay_tools, NULL), "/usr/sbin/fsck.vfat") == 0, "vfat tool");
    expect(sd_fsck_select_tool(SD_FS_KIND_UNKNOWN, only_overlay_tools, NULL) == NULL, "unknown tool");

    sd_fsck_plan_t plan;
    expect(sd_fsck_plan(SD_FS_KIND_VFAT, "/usr/sbin/fsck.vfat", "/dev/mmcblk0p1", &plan), "vfat plan");
    expect(strcmp(plan.argv[1], "-a") == 0 && strcmp(plan.argv[2], "-w") == 0 &&
               strcmp(plan.argv[3], "/dev/mmcblk0p1") == 0 && plan.argv[4] == NULL,
           "vfat repairs without questions");
    expect(sd_fsck_plan(SD_FS_KIND_EXFAT, "/usr/sbin/fsck.exfat", "/dev/mmcblk0", &plan), "exfat plan");
    expect(strcmp(plan.argv[1], "-y") == 0 && strcmp(plan.argv[2], "/dev/mmcblk0") == 0 && plan.argv[3] == NULL,
           "exfat answers yes");
    expect(sd_fsck_plan(SD_FS_KIND_NTFS, "/usr/sbin/ntfsfix", "/dev/mmcblk0p1", &plan), "ntfs plan");
    expect(strcmp(plan.argv[1], "/dev/mmcblk0p1") == 0 && plan.argv[2] == NULL, "ntfsfix has no interactive flag");
    expect(!sd_fsck_plan(SD_FS_KIND_VFAT, "/usr/sbin/fsck.vfat", "/dev/sda1", &plan), "foreign device rejected");

    expect(sd_fsck_exit_usable(0) && sd_fsck_exit_usable(1) && sd_fsck_exit_usable(2) && sd_fsck_exit_usable(3),
           "corrected filesystems are usable");
    expect(!sd_fsck_exit_usable(4) && !sd_fsck_exit_usable(8) && !sd_fsck_exit_usable(-1) && !sd_fsck_exit_usable(127),
           "uncorrected and operational failures are not usable");

    char key[SD_FSCK_ATTEMPT_BYTES];
    sd_repair_attempt_key("/dev/mmcblk0p1", "abcd", key, sizeof(key));
    expect(strcmp(key, "cid:abcd") == 0, "card id identifies an attempt");
    sd_repair_attempt_key("/dev/mmcblk0p1", "", key, sizeof(key));
    expect(strcmp(key, "dev:/dev/mmcblk0p1") == 0, "block node identifies an attempt without a card id");

    if (failures) {
        fprintf(stderr, "sd fsck: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("sd fsck: PASS");
    return 0;
}
