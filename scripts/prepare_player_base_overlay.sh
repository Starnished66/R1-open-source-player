#!/usr/bin/env bash
# Separate runtime overlays let audio, Bluetooth and Wi-Fi be tested in order.
# Never installs into Test2 or changes the device.
set -euo pipefail
[[ $# == 1 ]] || { echo 'Usage: prepare_player_base_overlay.sh alsa|bluetooth|wifi' >&2; exit 2; }
component=$1
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
base="$repo/scratch/base-upgrade"
case "$component" in
    alsa) stage="$base/alsa-utils/stage" ;;
    bluetooth) stage="$base/bluez/stage" ;;
    wifi) stage="$base/wifi/stage" ;;
    *) echo "Unknown component: $component" >&2; exit 2 ;;
esac
[[ -d "$stage/usr" ]] || { echo "Build $component first: missing $stage" >&2; exit 1; }
mkdir -p "$base/player-overlays/$component"
run=$(mktemp -d "$base/player-overlays/$component/run.XXXXXX")
overlay="$run/root"
mkdir -p "$overlay/usr/bin" "$overlay/usr/lib" "$overlay/usr/share/licenses"
case "$component" in
    alsa)
        for binary in aplay arecord amixer; do
            cp -a "$stage/usr/bin/$binary" "$overlay/usr/bin/"
        done
        ;;
    bluetooth)
        mkdir -p "$overlay/usr/libexec/bluetooth"
        cp "$stage/usr/libexec/bluetooth/bluetoothd" "$overlay/usr/libexec/bluetooth/"
        # Keep vendor firmware/UART attachment tools and radio init scripts.
        # Only replace the daemon and its player-facing control/diagnostic tools.
        for binary in bluetoothctl hciconfig hcitool btmon; do
            cp "$stage/usr/bin/$binary" "$overlay/usr/bin/"
        done
        [[ -f "$repo/open_hiby_player_target" ]] || { echo 'Run make target first' >&2; exit 1; }
        cp "$repo/open_hiby_player_target" "$overlay/usr/bin/open_hiby_player"
        ;;
    wifi)
        mkdir -p "$overlay/usr/sbin"
        for binary in wpa_supplicant wpa_cli; do
            cp "$stage/usr/sbin/$binary" "$overlay/usr/sbin/"
        done
        for stem in libnl-3 libnl-genl-3; do
            [[ -e "$stage/usr/lib/$stem.so.200" ]] || { echo "Missing $stem" >&2; exit 1; }
            cp -a "$stage/usr/lib/$stem".so* "$overlay/usr/lib/"
        done
        cp "$stage/usr/lib/libwpa_client.so" "$overlay/usr/lib/"
        # Do not replace OpenSSL, saved networks, DHCP, drivers or firmware.
        ;;
esac
cp -a "$stage/usr/share/licenses/." "$overlay/usr/share/licenses/"
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
while IFS= read -r -d '' file; do
    file_info=$(file -b "$file")
    [[ $file_info == ELF* ]] || continue
    "${cross}strip" --strip-unneeded "$file"
    if readelf -d "$file" | grep -E '(RPATH|RUNPATH)' >/dev/null; then
        echo "Unexpected runtime search path: $file" >&2
        exit 1
    fi
done < <(find "$overlay/usr" -type f -print0)
(cd "$overlay" && find . \( -type f -o -type l \) -print | LC_ALL=C sort) > "$run/files.txt"
stable="$base/player-overlays/$component/current"
[[ ! -e "$stable" || -L "$stable" ]] || { echo 'current must be a symlink' >&2; exit 1; }
ln -sfnT "$overlay" "$stable"
printf '%s overlay: %s\nManifest: %s/files.txt\nNothing installed or flashed.\n' "$component" "$overlay" "$run"
