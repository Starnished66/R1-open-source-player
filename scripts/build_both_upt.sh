#!/usr/bin/env bash
# Build both supported firmware images from user-supplied base images.

set -euo pipefail

usage() {
    echo "Usage: $0 R1_BASE_UPT R3PROII_BASE_UPT OUTPUT_DIR" >&2
    echo "       writes OUTPUT_DIR/r1.upt and OUTPUT_DIR/r3proii.upt" >&2
    exit 2
}

[[ $# -eq 3 ]] || usage

repo=$(cd "$(dirname "$0")/.." && pwd)
r1_base=$(realpath "$1")
r3_base=$(realpath "$2")
output_dir=$(realpath -m "$3")

for base in "$r1_base" "$r3_base"; do
    [[ -s "$base" ]] || { echo "Missing or empty base image: $base" >&2; exit 1; }
done
[[ "$r1_base" != "$r3_base" ]] || {
    echo "R1 and R3 Pro II base images must be different files" >&2
    exit 1
}

for command in make mipsel-linux-musl-gcc mipsel-linux-musl-g++ \
    mipsel-linux-musl-strip 7z unsquashfs mksquashfs genisoimage \
    md5sum sha256sum split file; do
    command -v "$command" >/dev/null || {
        echo "Required command is unavailable: $command" >&2
        exit 1
    }
done

mkdir -p "$output_dir"
r1_output=$(realpath -m "$output_dir/r1.upt")
r3_output=$(realpath -m "$output_dir/r3proii.upt")
for output in "$r1_output" "$r3_output"; do
    [[ "$output" != "$r1_base" && "$output" != "$r3_base" ]] || {
        echo "Refusing to overwrite a base image: $output" >&2
        exit 1
    }
done

echo "Building R1 binaries..."
make -C "$repo" target bootloader BOARD=r1
echo "Building R3 Pro II binaries..."
make -C "$repo" target bootloader BOARD=r3proii

for binary in \
    "$repo/open_hiby_player_target" \
    "$repo/open_hiby_bootloader" \
    "$repo/open_hiby_player_target_r3proii" \
    "$repo/open_hiby_bootloader_r3proii"; do
    [[ -s "$binary" ]] || { echo "Build did not produce: $binary" >&2; exit 1; }
done

temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT

echo "Repacking R1 image..."
"$repo/scripts/repack_upt.sh" --board r1 "$r1_base" \
    "$repo/open_hiby_player_target" "$repo/open_hiby_bootloader" \
    "$temp_dir/r1.upt"
echo "Repacking R3 Pro II image..."
"$repo/scripts/repack_upt.sh" --board r3proii "$r3_base" \
    "$repo/open_hiby_player_target_r3proii" "$repo/open_hiby_bootloader_r3proii" \
    "$temp_dir/r3proii.upt"

mv -f "$temp_dir/r1.upt" "$r1_output"
mv -f "$temp_dir/r3proii.upt" "$r3_output"
echo "Created $r1_output and $r3_output"
