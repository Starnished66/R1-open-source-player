#!/usr/bin/env bash
# Cross-build Expat for the vendor glibc 2.22 MIPS32R2 userspace.
# This script stages only under scratch/base-upgrade/expat; it never edits Test2.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${BASE_EXPAT_BUILD_DIR:-"$repo/scratch/base-upgrade/expat"}
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
stock=${BASE_EXPAT_STOCK:-/home/josegarita/Desktop/Test2/squashfs-root/usr/lib/libexpat.so.1.6.11}

if [[ $# -ne 0 ]]; then
    echo "usage: $0" >&2
    exit 2
fi

mkdir -p -- "$work"
work=$(cd -- "$work" && pwd)

version=2.8.4
archive="expat-${version}.tar.xz"
url="https://github.com/libexpat/libexpat/releases/download/R_${version//./_}/${archive}"
sha256=656ae1cc8da3b4ea513bb4e254f33e6243938084c0ec6239da873376b09985a7
src="$work/src/expat-${version}"
run=$(mktemp -d "$work/run.XXXXXX")
build="$run/build"
stage="$run/stage"

mkdir -p "$work/downloads" "$work/src" "$build" "$stage"
archive_path="$work/downloads/$archive"
if [[ ! -f "$archive_path" ]]; then
    curl --fail --location --retry 3 "$url" -o "$archive_path.part"
    mv -- "$archive_path.part" "$archive_path"
fi
printf '%s  %s\n' "$sha256" "$archive_path" | sha256sum --check --status

if [[ ! -d "$src" ]]; then
    tar -xf "$archive_path" -C "$work/src"
fi

cc="${cross}gcc"
ar="${cross}ar"
ranlib="${cross}ranlib"
strip="${cross}strip"
for tool in "$cc" "$ar" "$ranlib" "$strip"; do
    [[ -x "$tool" ]] || { echo "missing cross tool: $tool" >&2; exit 1; }
done
[[ -f "$stock" ]] || { echo "stock Expat library not found: $stock" >&2; exit 1; }

flags='-Os -EL -mips32r2 -mabi=32 -mhard-float -mfp32'
export CC="$cc" AR="$ar" RANLIB="$ranlib" STRIP="$strip"
export CFLAGS="$flags" LDFLAGS="$flags"
export PKG_CONFIG_LIBDIR="$stage/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$stage"
unset PKG_CONFIG_PATH

"$cc" --version | head -n 1
cmake -S "$src" -B "$build" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER="$cc" \
    -DCMAKE_AR="$ar" \
    -DCMAKE_RANLIB="$ranlib" \
    -DCMAKE_STRIP="$strip" \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=ON \
    -DEXPAT_BUILD_TESTS=OFF \
    -DEXPAT_BUILD_TOOLS=OFF \
    -DEXPAT_BUILD_EXAMPLES=OFF \
    -DEXPAT_BUILD_FUZZERS=OFF \
    -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build "$build" --parallel "${JOBS:-4}"
DESTDIR="$stage" cmake --install "$build" --prefix /usr --strip

lib="$stage/usr/lib/libexpat.so.1"
[[ -L "$lib" && -f "$lib" ]] || {
    echo "unexpected Expat shared-library layout under $stage/usr/lib" >&2
    find "$stage/usr/lib" -maxdepth 1 -name 'libexpat*' -print >&2
    exit 1
}

symbols() {
    readelf --dyn-syms --wide "$1" |
        awk '$7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK") && $8 ~ /^XML_/ { sub(/@@?[^ ]+$/, "", $8); print $8 }' |
        sort -u
}
comm -23 <(symbols "$stock") <(symbols "$lib") | tee "$work/missing-stock-symbols.txt"
[[ ! -s "$work/missing-stock-symbols.txt" ]] || {
    echo "new Expat is missing stock dynamic symbols" >&2
    exit 1
}

if readelf --version-info "$lib" 2>/dev/null | rg 'GLIBC_2\.(2[3-9]|[3-9][0-9])' >/dev/null; then
    echo "new Expat requires glibc newer than 2.22" >&2
    exit 1
fi
file "$lib"
readelf -h "$lib" | rg 'Class:|Data:|Machine:|Flags:'
readelf -d "$lib" | rg 'SONAME|NEEDED'
printf 'Staged Expat %s: %s\n' "$version" "$stage/usr/lib"
printf 'Stock dynamic symbols are a subset of the staged library; no Test2 files modified.\n'
[[ ! -e "$work/current" || -L "$work/current" ]] || { echo 'current must be a symlink' >&2; exit 1; }
ln -sfnT "$stage" "$work/current"
