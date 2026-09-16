#!/usr/bin/env bash
# Upgrade the PCM/mixer tools used by the player; retain device ALSA config.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${BASE_ALSA_UTILS_BUILD_DIR:-"$repo/scratch/base-upgrade/alsa-utils"}
alsa=${BASE_AUDIO_STAGE:-"$repo/scratch/base-upgrade/audio/stage"}
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
mkdir -p "$work/downloads"
work=$(cd -- "$work" && pwd)
archive="$work/downloads/alsa-utils-1.2.16.tar.bz2"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 \
        https://www.alsa-project.org/files/pub/utils/alsa-utils-1.2.16.tar.bz2 -o "$archive.part"
    mv -- "$archive.part" "$archive"
fi
printf '%s  %s\n' 092399d5e8749a1d5e188e393157521cec4b75693b60ebb79bbce728cff2232c "$archive" | sha256sum --check --status
run=$(mktemp -d "$work/run.XXXXXX")
tar -xf "$archive" -C "$run"
src="$run/alsa-utils-1.2.16"
stage="$run/stage"
mkdir -p "$run/build" "$stage/usr/bin" "$stage/usr/share/licenses/alsa-utils"
# Libtool's staged .la files inject workstation RPATHs. Use a private
# development root containing only headers, pkg-config and shared libraries.
deps="$run/deps"
mkdir -p "$deps/usr/lib/pkgconfig"
cp -a "$alsa/usr/include" "$deps/usr/"
cp -a "$alsa"/usr/lib/libasound.so* "$deps/usr/lib/"
cp -a "$alsa"/usr/lib/libatopology.so* "$deps/usr/lib/"
cp "$alsa/usr/lib/pkgconfig/alsa.pc" "$deps/usr/lib/pkgconfig/"
export CC="${cross}gcc" AR="${cross}ar" RANLIB="${cross}ranlib" STRIP="${cross}strip"
export CFLAGS='-Os -EL -mips32r2 -mabi=32 -mhard-float -mfp32'
export CPPFLAGS="-I$deps/usr/include"
export LDFLAGS="-EL -mips32r2 -mabi=32 -mhard-float -mfp32 -L$deps/usr/lib -Wl,-rpath-link,$deps/usr/lib"
export PKG_CONFIG_LIBDIR="$deps/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$deps"
unset PKG_CONFIG_PATH
(
    cd "$run/build"
    "$src/configure" --host=mips-linux-gnu --prefix=/usr --sysconfdir=/etc \
        --disable-nls --disable-alsamixer --disable-alsaconf --disable-alsaloop \
        --disable-bat --disable-nhlt --disable-xmlto --disable-rst2man
    # Do not install alsactl state restore, service units or generic sound
    # configuration into an image with a vendor-controlled hardware mixer.
    make -C include
    make -C aplay -j"${JOBS:-4}"
    make -C amixer -j"${JOBS:-4}"
)
cp "$run/build/aplay/aplay" "$stage/usr/bin/aplay"
ln -s aplay "$stage/usr/bin/arecord"
cp "$run/build/amixer/amixer" "$stage/usr/bin/amixer"
cp "$src/COPYING" "$stage/usr/share/licenses/alsa-utils/"
for binary in aplay amixer; do
    elf="$stage/usr/bin/$binary"
    "${cross}strip" --strip-unneeded "$elf"
    file "$elf"
    readelf -h "$elf" | grep -E 'Flags:.*mips32r2' >/dev/null
    readelf -A "$elf" | grep -F 'Hard float' >/dev/null
    if readelf -d "$elf" | grep -E '(RPATH|RUNPATH)' >/dev/null; then
        echo "Unexpected runtime search path: $elf" >&2
        exit 1
    fi
done
[[ ! -e "$work/stage" || -L "$work/stage" ]] || { echo 'stage must be a symlink' >&2; exit 1; }
ln -sfnT "$stage" "$work/stage"
printf 'ALSA utilities 1.2.16 stage: %s\nNo firmware files modified.\n' "$stage"
