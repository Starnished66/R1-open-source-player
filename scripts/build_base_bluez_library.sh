#!/usr/bin/env bash
# Build only BlueZ's three-source public library. The vendor daemon stays intact.
# This is the lib_libbluetooth_la_SOURCES target from upstream Makefile.am;
# building it directly avoids pulling daemon-only GLib/DBus dependencies here.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${BASE_BLUEZ_BUILD_DIR:-"$repo/scratch/base-upgrade/bluez-library"}
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
mkdir -p "$work/downloads"
work=$(cd -- "$work" && pwd)
archive="$work/downloads/bluez-5.87.tar.xz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 https://www.kernel.org/pub/linux/bluetooth/bluez-5.87.tar.xz -o "$archive.part"
    mv -- "$archive.part" "$archive"
fi
printf '%s  %s\n' 26bdcf2cebd7310c6f598850606b037ef0c515fe6608ebc54d22c50c4c32b35f "$archive" | sha256sum --check --status
run=$(mktemp -d "$work/run.XXXXXX")
tar -xf "$archive" -C "$run"
src="$run/bluez-5.87"
stage="$run/stage"
mkdir -p "$stage/usr/lib/pkgconfig" "$stage/usr/include/bluetooth" "$stage/usr/share/licenses/bluez-library"
"${cross}gcc" -Os -fPIC -EL -mips32r2 -mabi=32 -mhard-float -mfp32 \
    -shared -Wl,-soname,libbluetooth.so.3 -Wl,--no-undefined \
    -I"$src/lib" -I"$src/lib/bluetooth" \
    "$src/lib/bluetooth/bluetooth.c" "$src/lib/bluetooth/hci.c" "$src/lib/bluetooth/sdp.c" \
    -o "$stage/usr/lib/libbluetooth.so.3.19.16"
ln -s libbluetooth.so.3.19.16 "$stage/usr/lib/libbluetooth.so.3"
ln -s libbluetooth.so.3 "$stage/usr/lib/libbluetooth.so"
for header in bluetooth hci hci_lib sco l2cap sdp sdp_lib rfcomm bnep cmtp hidp; do
    cp "$src/lib/bluetooth/$header.h" "$stage/usr/include/bluetooth/"
done
sed -e 's|@prefix@|/usr|g' -e 's|@exec_prefix@|/usr|g' \
    -e 's|@libdir@|/usr/lib|g' -e 's|@includedir@|/usr/include|g' \
    -e 's|@VERSION@|5.87|g' "$src/lib/bluez.pc.in" > "$stage/usr/lib/pkgconfig/bluez.pc"
cp "$src/COPYING" "$src/COPYING.LIB" "$stage/usr/share/licenses/bluez-library/"
[[ ! -e "$work/stage" || -L "$work/stage" ]] || { echo 'stage must be a symlink' >&2; exit 1; }
ln -sfnT "$stage" "$work/stage"
readelf -h "$stage/usr/lib/libbluetooth.so.3" | sed -n '1,22p'
readelf -d "$stage/usr/lib/libbluetooth.so.3" | sed -n '1,12p'
printf 'BlueZ public library stage (not bluetoothd): %s\n' "$stage"
