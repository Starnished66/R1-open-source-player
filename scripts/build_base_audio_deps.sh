#!/usr/bin/env bash
# Build ABI-compatible ALSA/SBC libraries; never install into the firmware tree.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${BASE_AUDIO_BUILD_DIR:-"$repo/scratch/base-upgrade/audio"}
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
mkdir -p "$work/downloads" "$work/src" "$work/build" "$work/stage"
work=$(cd -- "$work" && pwd)
export CC="${cross}gcc" AR="${cross}ar" RANLIB="${cross}ranlib" STRIP="${cross}strip"
export NM="${cross}nm" LD="${cross}ld" OBJDUMP="${cross}objdump"
export CFLAGS='-Os -EL -mips32r2 -mabi=32 -mhard-float -mfp32'
export LDFLAGS='-EL -mips32r2 -mabi=32 -mhard-float -mfp32'
# Do not discover host libraries when configuring target packages.
export PKG_CONFIG_LIBDIR="$work/stage/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$work/stage"
unset PKG_CONFIG_PATH
"$CC" --version
fetch() {
    local package=$1 url=$2 checksum=$3 archive="$work/downloads/${2##*/}"
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --retry 3 "$url" -o "$archive.part"
        mv -- "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
    if [[ ! -d "$work/src/$package" ]]; then
        tar -xf "$archive" -C "$work/src"
    fi
}
build_autoconf() {
    local package=$1
    shift
    mkdir -p "$work/build/$package"
    (
        cd "$work/build/$package"
        "$work/src/$package/configure" --host=mips-linux-gnu --prefix=/usr \
            --libdir=/usr/lib --sysconfdir=/etc --disable-static "$@"
        make -j"${JOBS:-4}"
        make DESTDIR="$work/stage" install
    )
}
fetch sbc-2.2 https://www.kernel.org/pub/linux/bluetooth/sbc-2.2.tar.xz \
    a1ada76ef35e5af9c2fbd063754dc9e37a8d989417c6eb1ecebb089b1383ae9e
build_autoconf sbc-2.2 --disable-tools --disable-tester
fetch alsa-lib-1.2.16 https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.16.tar.bz2 \
    122b1e3166d55fe19bcde656535d7a36f2ab10e66c72c6ad2f43f20ffded0a96
# The vendor libasound exports unversioned symbols, including plugin helpers.
build_autoconf alsa-lib-1.2.16 --disable-python --without-versioned
printf 'Staged audio libraries: %s\n' "$work/stage"
printf 'Not installed into Test2; retain device ALSA configuration during integration.\n'
