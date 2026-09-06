#!/usr/bin/env bash
# Repack an approved R1 Staging Image with freshly built player binaries.
# The base remains external because it contains stock HiBy firmware assets.

set -euo pipefail

usage() {
    echo "Usage: $0 BASE_R1_UPT PLAYER_BINARY BOOTLOADER_BINARY OUTPUT_R1_UPT" >&2
    exit 2
}

[[ $# -eq 4 ]] || usage

base_upt=$(realpath "$1")
player=$(realpath "$2")
bootloader=$(realpath "$3")
output=$(realpath -m "$4")

for file in "$base_upt" "$player" "$bootloader"; do
    [[ -s "$file" ]] || { echo "Missing or empty input: $file" >&2; exit 1; }
done

for command in 7z unsquashfs mksquashfs genisoimage md5sum split; do
    command -v "$command" >/dev/null || {
        echo "Required command is unavailable: $command" >&2
        exit 1
    }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/iso" "$work/new-iso/ota_v0" "$(dirname "$output")"

7z x -y "$base_upt" -o"$work/iso" >/dev/null
ota_dir="$work/iso/ota_v0"
[[ -d "$ota_dir" ]] || { echo "Base OTA has no ota_v0 directory" >&2; exit 1; }

mapfile -t root_chunks < <(find "$ota_dir" -maxdepth 1 -type f \
    -name 'rootfs.squashfs.[0-9]*' -print | LC_ALL=C sort)
mapfile -t kernel_chunks < <(find "$ota_dir" -maxdepth 1 -type f \
    -name 'xImage.[0-9]*' -print | LC_ALL=C sort)
(( ${#root_chunks[@]} > 0 )) || { echo "Base OTA has no rootfs chunks" >&2; exit 1; }
(( ${#kernel_chunks[@]} > 0 )) || { echo "Base OTA has no xImage chunks" >&2; exit 1; }

cat "${root_chunks[@]}" > "$work/rootfs.squashfs"
cat "${kernel_chunks[@]}" > "$work/xImage"
unsquashfs -no-xattrs -d "$work/root" "$work/rootfs.squashfs" >/dev/null

# An approved Staging Image must already contain the bootloader handoff.
# Refuse an older public beta instead of quietly producing a firmware that
# bypasses the boot menu after the new bootloader binary is copied in.
wrapper="$work/root/usr/bin/hiby_player.sh"
if [[ ! -f "$wrapper" ]] || ! grep -q '/usr/bin/open_hiby_bootloader' "$wrapper"; then
    echo "Base OTA is not an approved Staging Image (bootloader wrapper missing)" >&2
    exit 1
fi

install -m 0755 "$player" "$work/root/usr/bin/open_hiby_player"
install -m 0755 "$bootloader" "$work/root/usr/bin/open_hiby_bootloader"

# Files we own that are not in stock, laid out as squashfs-root-relative
# paths under firmware/overlay/ (e.g. usr/share/udhcpc/default.script.d/
# ntpdate). Applied after unpack so a newer overlay wins over whatever the
# Staging Image already has -- or doesn't. cp -a keeps mode bits and
# relative symlinks (sync_ntp.sh).
overlay="$(cd "$(dirname "$0")/.." && pwd)/firmware/overlay"
if [[ -d "$overlay" ]]; then
    cp -a "$overlay"/. "$work/root/"
fi

mksquashfs "$work/root" "$work/new-rootfs.squashfs" \
    -comp lzo -all-root -noappend -no-xattrs >/dev/null

package_image() {
    local image=$1
    local name=$2
    local destination=$3
    local initial_md5 size chain previous part part_md5

    initial_md5=$(md5sum "$image" | awk '{print $1}')
    size=$(stat -c%s "$image")
    chain="$destination/ota_md5_${name}.${initial_md5}"
    : > "$chain"
    split -b 512k --numeric-suffixes=0 -a 4 "$image" "$destination/${name}."
    previous=$initial_md5
    for part in "$destination/${name}."[0-9]*; do
        part_md5=$(md5sum "$part" | awk '{print $1}')
        echo "$part_md5" >> "$chain"
        mv "$part" "$part.$previous"
        previous=$part_md5
    done

    printf '%s %s %s\n' "$name" "$size" "$initial_md5"
}

read -r root_name root_size root_md5 < <(
    package_image "$work/new-rootfs.squashfs" rootfs.squashfs "$work/new-iso/ota_v0"
)
read -r kernel_name kernel_size kernel_md5 < <(
    package_image "$work/xImage" xImage "$work/new-iso/ota_v0"
)

cat > "$work/new-iso/ota_v0/ota_update.in" <<EOF
ota_version=0
img_type=kernel
img_name=$kernel_name
img_size=$kernel_size
img_md5=$kernel_md5
img_type=rootfs
img_name=$root_name
img_size=$root_size
img_md5=$root_md5
EOF
: > "$work/new-iso/ota_v0/ota_v0.ok"
echo 'current_version=0' > "$work/new-iso/ota_config.in"

genisoimage -f -U -J -joliet-long -r -allow-lowercase -allow-multidot \
    -o "$output" "$work/new-iso" >/dev/null

# The device's update path has a practical 45 MiB firmware ceiling.
max_size=$((45 * 1024 * 1024))
actual_size=$(stat -c%s "$output")
if (( actual_size > max_size )); then
    echo "Repacked OTA is too large: $actual_size bytes (limit $max_size)" >&2
    exit 1
fi

echo "Created $output ($actual_size bytes)"
sha256sum "$output"
