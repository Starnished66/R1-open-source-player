#!/usr/bin/env bash
# Repack a supported HiBy firmware image with freshly built player binaries.
# The base remains external because it contains stock HiBy firmware assets.

set -euo pipefail

usage() {
    echo "Usage: $0 [--board r1|r3proii] BASE_UPT PLAYER_BINARY BOOTLOADER_BINARY OUTPUT_UPT" >&2
    echo "       BOARD=r3proii $0 BASE_UPT PLAYER_BINARY BOOTLOADER_BINARY OUTPUT_UPT" >&2
    exit 2
}

board=${BOARD:-r1}
if [[ ${1:-} == --board ]]; then
    [[ $# -ge 2 ]] || usage
    board=$2
    shift 2
fi
[[ $board == r1 || $board == r3proii ]] || {
    echo "Unsupported board '$board' (expected r1 or r3proii)" >&2
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

for command in 7z unsquashfs mksquashfs genisoimage md5sum split file; do
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

if [[ $board == r3proii ]]; then
    stock_player="$work/root/usr/bin/hiby_player"
    if [[ ! -s "$stock_player" ]] || ! grep -aFq 'R3PROII' "$stock_player"; then
        echo "Base OTA does not contain an R3 Pro II stock player; refusing a cross-board image" >&2
        exit 1
    fi
fi

wrapper="$work/root/usr/bin/hiby_player.sh"
if [[ ! -f "$wrapper" ]]; then
    echo "Base OTA is missing /usr/bin/hiby_player.sh" >&2
    exit 1
fi

if [[ $board == r1 ]]; then
    # An approved R1 Staging Image must already contain the bootloader handoff.
    # Refuse an older public beta instead of quietly producing a firmware that
    # bypasses the boot menu after the new bootloader binary is copied in.
    grep -q '/usr/bin/open_hiby_bootloader' "$wrapper" || {
        echo "Base OTA is not an approved R1 Staging Image (bootloader wrapper missing)" >&2
        exit 1
    }
else
    # R3 Pro II stock firmware starts the stock player directly. Replace only
    # the standalone command, preserving /usr/bin/hiby_player as the stock
    # player that the bootloader can launch when selected from the SD card.
    if ! grep -q '/usr/bin/open_hiby_bootloader' "$wrapper"; then
        grep -Eq '^[[:space:]]*/usr/bin/hiby_player[[:space:]]*$' "$wrapper" || {
            echo "R3 OTA has no standalone /usr/bin/hiby_player launcher to patch" >&2
            exit 1
        }
        sed -i 's|^[[:space:]]*/usr/bin/hiby_player[[:space:]]*$|/usr/bin/open_hiby_bootloader|' "$wrapper"
    fi
    grep -q '/usr/bin/open_hiby_bootloader' "$wrapper" || {
        echo "Failed to patch R3 /usr/bin/hiby_player.sh" >&2
        exit 1
    }
fi

# A previous Compás image may have left the old standalone name behind.
# Remove it before installing the renamed player so the update has one
# unambiguous standalone binary and does not waste space in the rootfs.
rm -f "$work/root/usr/bin/open_hiby_player"
install -m 0755 "$player" "$work/root/usr/bin/compas_player"
install -m 0755 "$bootloader" "$work/root/usr/bin/open_hiby_bootloader"

# The R1 stock boot scripts start the A2DP source daemon without the encoder
# arguments the player's default "auto" codec preference expects. Keep this
# established R1 fix; R3 stock uses a different bluealsa command line and its
# Bluetooth scripts are deliberately left untouched.
if [[ $board == r1 ]]; then
for bt_script in bt_init bt_resume; do
    bt_script_path="$work/root/usr/bin/$bt_script"
    [[ -f "$bt_script_path" ]] || {
        echo "Base OTA is missing /usr/bin/$bt_script" >&2
        exit 1
    }
    grep -q 'bluealsad -p a2dp-source' "$bt_script_path" || {
        echo "Base OTA /usr/bin/$bt_script does not start bluealsad as an A2DP source" >&2
        exit 1
    }
    sed -i '/--all-codecs/!s|bluealsad -p a2dp-source|bluealsad -p a2dp-source --all-codecs|g' \
        "$bt_script_path"
    # Negotiate 44.1 kHz rather than BlueALSA's default pick of the highest
    # rate up to 48 kHz: resampling only happens when a track's rate differs
    # from the transport's, and CD-derived 44.1 kHz material is the bulk of a
    # typical music library.
    sed -i '/--a2dp-force-audio-cd/!s|bluealsad -p a2dp-source|bluealsad -p a2dp-source --a2dp-force-audio-cd|g' \
        "$bt_script_path"
    for bt_arg in --all-codecs --a2dp-force-audio-cd; do
        grep -q -- "$bt_arg" "$bt_script_path" || {
            echo "Failed to add $bt_arg to /usr/bin/$bt_script" >&2
            exit 1
        }
    done
    sh -n "$bt_script_path"
done
fi

repo="$(cd "$(dirname "$0")/.." && pwd)"

# These boot images are part of each board's identity. Require the exact
# tracked files and panel dimensions before building a firmware package.
require_boot_image() {
    local path=$1 type=$2 width=$3 height=$4 description
    git -C "$repo" ls-files --error-unmatch "$path" >/dev/null 2>&1 || {
        echo "Boot image is not tracked in git: $path" >&2
        exit 1
    }
    [[ -s "$repo/$path" ]] || { echo "Missing boot image: $path" >&2; exit 1; }
    description=$(file -b "$repo/$path")
    [[ $description == *"$type image data"* &&
       $description =~ (^|[^0-9])${width}[[:space:]]*x[[:space:]]*${height}([^0-9]|$) ]] || {
        echo "Wrong boot image format or size ($type ${width}x${height} required): $path" >&2
        exit 1
    }
}
if [[ $board == r1 ]]; then
    require_boot_image assets/theme2/boot_animation/en/0.jpg JPEG 480 800
    require_boot_image assets/theme2/boot_animation/en/0.png PNG 480 800
    require_boot_image assets/r1/etc/logo1.jpeg JPEG 480 800
else
    require_boot_image assets/r3proii/theme2/boot_animation/en/0.jpg JPEG 480 720
    require_boot_image assets/r3proii/theme2/boot_animation/en/0.png PNG 480 720
    require_boot_image assets/r3proii/etc/logo1.jpeg JPEG 480 720
fi

# UI assets and fonts we own, kept under assets/ in the tree the device itself
# uses: assets/theme2/<dir>/<file> lands at /usr/resource/litegui/theme2/, and
# assets/fonts/ at /usr/resource/fonts/. Copied after unpack so ours win over
# whatever the Staging Image already has -- or doesn't. Anything not copied
# here reaches the device only if the base image already carried it.
# Only tracked files are copied, so "tracked in git" and "shipped on the
# device" mean the same thing. That also keeps a contributor's own local
# stock-firmware dump (ignored, see .gitignore) out of the image -- stock
# assets already come from the base image.
copy_tracked_assets() {
    local src="$1" dest="$2" f rel
    [[ -d "$repo/$src" ]] || return 0
    while IFS= read -r f; do
        rel="${f#"$src"/}"
        mkdir -p "$dest/$(dirname "$rel")"
        cp -a "$repo/$f" "$dest/$rel"
    done < <(git -C "$repo" ls-files "$src")
}
if [[ $board == r1 ]]; then
    copy_tracked_assets assets/theme2 "$work/root/usr/resource/litegui/theme2"
    copy_tracked_assets assets/r1/etc "$work/root/etc"
else
    copy_tracked_assets assets/r3proii/theme2 "$work/root/usr/resource/litegui/theme2"
    copy_tracked_assets assets/r3proii/etc "$work/root/etc"
fi
copy_tracked_assets assets/fonts  "$work/root/usr/resource/fonts"

# Non-asset files we own that are not in stock, laid out as squashfs-root-
# relative paths under firmware/overlay/ (e.g. usr/share/udhcpc/
# default.script.d/ntpdate). cp -a keeps mode bits and relative symlinks
# (sync_ntp.sh).
overlay="$repo/firmware/overlay"
if [[ -d "$overlay" ]]; then
    cp -a "$overlay"/. "$work/root/"
fi

# A local R3 runtime overlay is opt-in. CI supplies an already-upgraded base
# image and leaves this unset; never discover or source ignored scratch output
# implicitly. The overlay is copied before the release gates so local builds
# can be checked against the same BlueALSA/BlueZ contract as CI.
if [[ $board == r3proii && -n ${R3_RUNTIME_OVERLAY:-} ]]; then
    [[ -d "$R3_RUNTIME_OVERLAY" ]] || {
        echo "R3_RUNTIME_OVERLAY is not a directory: $R3_RUNTIME_OVERLAY" >&2
        exit 1
    }
    runtime_overlay=$(realpath "$R3_RUNTIME_OVERLAY")
    cp -a "$runtime_overlay"/. "$work/root/"
fi

# The player promises the Speex rate converter, which is built from the
# pinned, redistributable SpeexDSP/alsa-plugins sources by the base-image
# process. Do not silently publish a base image that falls back to the
# lower-quality alsa-lib converter: the approved R1 and R3 base images must
# already contain the plugin and its runtime library.
speex_plugin="$work/root/usr/lib/alsa-lib/libasound_module_rate_speexrate.so"
[[ -e "$speex_plugin" ]] || {
    echo "${board} base OTA is missing the Speex ALSA rate plugin required by the player" >&2
    exit 1
}
[[ -e "$work/root/usr/lib/libspeexdsp.so.1" ]] || {
    echo "${board} base OTA is missing the SpeexDSP runtime library required by the ALSA plugin" >&2
    exit 1
}

if [[ $board == r3proii ]]; then
    # R3 releases must use the updated BlueALSA 5/BlueZ runtime. Stock R3
    # images contain only the legacy bluealsa daemon, so fail before packaging
    # unless CI's upgraded base or an explicitly selected local overlay has
    # supplied every player-facing runtime component.
    r3_runtime_paths=(
        /usr/bin/bluealsad
        /usr/bin/bluealsactl
        /usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so
        /usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so
        /usr/libexec/bluetooth/bluetoothd
    )
    for runtime_path in "${r3_runtime_paths[@]}"; do
        [[ -e "$work/root$runtime_path" ]] || {
            echo "R3 Pro II runtime gate failed: missing $runtime_path (use an updated staging base or R3_RUNTIME_OVERLAY)" >&2
            exit 1
        }
    done
    for bt_script in bt_init bt_resume; do
        bt_script_path="$work/root/usr/bin/$bt_script"
        [[ -f "$bt_script_path" ]] || {
            echo "R3 Pro II runtime gate failed: missing /usr/bin/$bt_script" >&2
            exit 1
        }
        grep -Eq '(^|[[:space:]/])bluealsad([[:space:]]|$)' "$bt_script_path" || {
            echo "R3 Pro II runtime gate failed: /usr/bin/$bt_script does not invoke bluealsad" >&2
            exit 1
        }
        for bt_arg in --all-codecs --a2dp-force-audio-cd; do
            grep -Fq -- "$bt_arg" "$bt_script_path" || {
                echo "R3 Pro II runtime gate failed: /usr/bin/$bt_script is missing $bt_arg" >&2
                exit 1
            }
        done
        sh -n "$bt_script_path"
    done
    # These scripts are optional across firmware revisions, but when present
    # they must manage the BlueALSA 5 daemon rather than the legacy daemon.
    for bt_script in bt_suspend bluealsa_profile; do
        bt_script_path="$work/root/usr/bin/$bt_script"
        [[ -f "$bt_script_path" ]] || continue
        grep -Fq -- 'bluealsad' "$bt_script_path" || {
            echo "R3 Pro II runtime gate failed: /usr/bin/$bt_script does not manage bluealsad" >&2
            exit 1
        }
        sh -n "$bt_script_path"
    done
fi

# Keep the two board packages on the same known-good font set. The manifest
# contains hashes extracted from the approved R1 package and is checked after
# every stock asset and overlay has been applied.
font_manifest="$repo/scripts/r1_firmware_fonts.sha256"
[[ -s "$font_manifest" ]] || {
    echo "Missing font parity manifest: $font_manifest" >&2
    exit 1
}
if ! (cd "$work/root" && sha256sum -c "$font_manifest"); then
    echo "Firmware font parity check failed; R1 and R3 packages must use the same font files" >&2
    exit 1
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
