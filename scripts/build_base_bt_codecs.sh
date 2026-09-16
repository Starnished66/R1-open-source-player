#!/usr/bin/env bash
# Rebuild AAC/aptX; retain the device's working LDAC encoder/ABR/decoder trio.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
base="$repo/scratch/base-upgrade"
work=${BASE_CODECS_BUILD_DIR:-"$base/codecs"}
stock=${BASE_STOCK_ROOT:-/home/josegarita/Desktop/Test2/squashfs-root}
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
check_source() {
    [[ $(git -C "$1" rev-parse HEAD) == "$2" ]] || { echo "Source revision mismatch: $1" >&2; exit 1; }
    git -C "$1" diff --quiet
    git -C "$1" diff --cached --quiet
}
check_source "$base/fdk-aac" 716f4394641d53f0d79c9ddac3fa93b03a49f278
check_source "$base/libopenaptx" 2459ed4686eaef0a19dfa3f330a960813c5f60de
check_source "$base/ldacBT" 6579bd585a618f2e1612b3c1650d2b7fcfb1d43f
check_source "$base/ldacBT/libldac" 82b6a1abee84787b8fa167efe20290073f60db2d
mkdir -p "$work"
work=$(cd -- "$work" && pwd)
run=$(mktemp -d "$work/run.XXXXXX")
stage="$run/stage"
mkdir -p "$stage/usr/lib/pkgconfig" "$stage/usr/include/ldac" "$run/aptx"
flags='-Os -EL -mips32r2 -mabi=32 -mhard-float -mfp32'
cmake -S "$base/fdk-aac" -B "$run/aac" -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER="${cross}gcc" -DCMAKE_CXX_COMPILER="${cross}g++" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$flags -Wl,--no-undefined" \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DBUILD_SHARED_LIBS=ON -DBUILD_PROGRAMS=OFF
cmake --build "$run/aac" --parallel "${JOBS:-4}"
DESTDIR="$stage" cmake --install "$run/aac"
# Build from an isolated copy; do not dirty the pinned source checkout.
git -C "$base/libopenaptx" archive HEAD | tar -x -C "$run/aptx"
make -C "$run/aptx" -j"${JOBS:-4}" CC="${cross}gcc" AR="${cross}ar" \
    CFLAGS="$flags" LDFLAGS="$flags" PREFIX=/usr
make -C "$run/aptx" CC="${cross}gcc" AR="${cross}ar" \
    CFLAGS="$flags" LDFLAGS="$flags" PREFIX=/usr DESTDIR="$stage" install
# pkg-config prefixes this upstream rpath with the build sysroot, leaking
# a workstation path into consumers. The normal target loader finds /usr/lib.
sed -i 's/-Wl,-rpath=${libdir} //g' "$stage/usr/lib/pkgconfig/libopenaptx.pc"
for library in libldacBT_enc.so.2 libldacBT_abr.so.2 libldacdec.so.1; do
    cp -L "$stock/usr/lib/$library" "$stage/usr/lib/$library"
done
ln -s libldacBT_enc.so.2 "$stage/usr/lib/libldacBT_enc.so"
ln -s libldacBT_abr.so.2 "$stage/usr/lib/libldacBT_abr.so"
ln -s libldacdec.so.1 "$stage/usr/lib/libldacdec.so"
cp "$base/ldacBT/libldac/inc/ldacBT.h" "$stage/usr/include/ldac/"
cp "$base/ldacBT/libldac/abr/inc/ldacBT_abr.h" "$stage/usr/include/ldac/"
# Local developer metadata for the retained 2.x LDAC ABI, not a decoder
# software release/version claim. No vendor binary is redistributed in git.
for part in enc abr dec; do
    library="ldacBT_$part"
    [[ $part != dec ]] || library=ldacdec
    printf 'prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include/ldac\nName: ldacBT-%s\nDescription: retained HiBy LDAC ABI\nVersion: 2.0.0\nLibs: -L${libdir} -l%s\nCflags: -I${includedir}\n' \
        "$part" "$library" > "$stage/usr/lib/pkgconfig/ldacBT-$part.pc"
done
[[ ! -e "$work/stage" || -L "$work/stage" ]] || { echo 'stage must be a symlink' >&2; exit 1; }
ln -sfnT "$stage" "$work/stage"
printf 'Codec developer stage: %s\nLDAC runtime retained unchanged.\n' "$stage"
