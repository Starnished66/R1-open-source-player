#!/usr/bin/env bash
# Non-hardware loader/CLI smoke tests against an extracted candidate rootfs.
set -euo pipefail
[[ $# == 1 && -d "$1/usr/lib" ]] || { echo "Usage: $0 ROOTFS" >&2; exit 2; }
root=$(cd -- "$1" && pwd)
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
runner=(qemu-mipsel -L "$root" -E LD_BIND_NOW=1 \
    -E "LD_LIBRARY_PATH=$root/usr/lib:$root/lib" \
    -E "DBUS_SYSTEM_BUS_ADDRESS=unix:path=$root/no-test-bus")
for binary in bluealsad bluealsactl bluealsa-aplay dbus-daemon \
    dbus-launch dbus-run-session dbus-uuidgen \
    bluetoothctl aplay; do
    echo "Checking $binary"
    "${runner[@]}" "$root/usr/bin/$binary" --version
done
# The stock amixer prints --version but returns 1; --help returns success.
for binary in dbus-monitor amixer; do
    "${runner[@]}" "$root/usr/bin/$binary" --help
done
# dbus-send requires argc >= 3 even when asking for help.
"${runner[@]}" "$root/usr/bin/dbus-send" --session --help
"${runner[@]}" "$root/usr/libexec/bluetooth/bluetoothd" --version
"${runner[@]}" "$root/usr/sbin/wpa_supplicant" -v
"${runner[@]}" "$root/usr/sbin/wpa_cli" -v
# Ensure the older tools left in the image still resolve their dependencies.
"${runner[@]}" "$root/usr/bin/bluealsa" --version
"${runner[@]}" "$root/usr/bin/bluealsa-cli" --version
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
base="$repo/scratch/base-upgrade"
test_dir=$(mktemp -d "$base/smoke.XXXXXX")
"${cross}gcc" -EL -mips32r2 -mabi=32 -mhard-float -mfp32 \
    -I"$base/audio/stage/usr/include" -I"$base/zlib/current/usr/include" \
    -I"$base/expat/current/usr/include" \
    "$repo/scripts/base_image/library_smoke.c" \
    -L"$root/usr/lib" -Wl,-rpath-link,"$root/usr/lib" \
    -lz -lexpat -lasound -lsbc -ldl -o "$test_dir/library_smoke"
plugins=("$root/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so"
         "$root/usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so")
# New core/genl libraries must also keep the retained routing/NF libraries
# loadable. Eager binding catches missing symbols that --version can miss.
for library in libwpa_client.so libnl-route-3.so.200 libnl-nf-3.so.200 \
               libnl-idiag-3.so.200 libnl-xfrm-3.so.200; do
    [[ ! -e "$root/usr/lib/$library" ]] || plugins+=("$root/usr/lib/$library")
done
"${runner[@]}" "$test_dir/library_smoke" "${plugins[@]}"
echo 'Base image loader/CLI smoke tests passed (not a hardware playback test).'
