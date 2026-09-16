#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
readonly ZLIB_URL='https://zlib.net/fossils/zlib-1.3.2.tar.gz'
readonly ZLIB_SHA256='bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16'
readonly ZLIB_VERSION='1.3.2'

compiler=${MIPS_GCC:-$REPO_ROOT/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-gcc}
build_dir=${BASE_ZLIB_BUILD_DIR:-$REPO_ROOT/scratch/base-upgrade/zlib}
stock_zlib=${STOCK_ZLIB:-}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}

usage() {
    cat >&2 <<EOF
Usage: $0 [--compiler PATH] [--build-dir DIR] [--stock-zlib PATH] [--jobs N]

Build zlib $ZLIB_VERSION as an isolated MIPS32r2 hard-float shared library.
Defaults: compiler=$compiler
          build-dir=$build_dir
EOF
}

while (($#)); do
    case $1 in
        --compiler) (($# >= 2)) || { usage; exit 2; }; compiler=$2; shift 2 ;;
        --build-dir) (($# >= 2)) || { usage; exit 2; }; build_dir=$2; shift 2 ;;
        --stock-zlib) (($# >= 2)) || { usage; exit 2; }; stock_zlib=$2; shift 2 ;;
        --jobs) (($# >= 2)) || { usage; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage 2>&1; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

die() { echo "build_base_zlib.sh: $*" >&2; exit 1; }

[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "jobs must be a positive integer: $jobs"
[[ -x $compiler ]] || die "MIPS compiler is not executable: $compiler"
for tool in curl sha256sum tar make readelf mktemp; do
    command -v "$tool" >/dev/null || die "$tool is required"
done

if [[ -z $stock_zlib ]]; then
    for candidate in \
        /home/josegarita/Desktop/Test2/squashfs-root/usr/lib/libz.so.1.2.11; do
        if [[ -f $candidate ]]; then stock_zlib=$candidate; break; fi
    done
fi
[[ -f $stock_zlib ]] || die "stock zlib is required; pass --stock-zlib PATH"

mkdir -p -- "$build_dir/src" "$build_dir/downloads"
build_dir=$(cd -- "$build_dir" && pwd)
[[ ! -L $build_dir ]] || die "build directory must not be a symlink: $build_dir"
readonly archive=$build_dir/downloads/zlib-$ZLIB_VERSION.tar.gz
readonly run_dir=$(mktemp -d "$build_dir/run.XXXXXX")
readonly source_dir=$run_dir/src/zlib-$ZLIB_VERSION
readonly object_dir=$run_dir/build
readonly stage_dir=$run_dir/stage
readonly output_lib=$stage_dir/usr/lib/libz.so.1.3.2

[[ ! -L $archive ]] || die "archive path must not be a symlink: $archive"
if [[ ! -f $archive ]]; then
    curl --fail --location --proto '=https' --proto-redir '=https' --silent --show-error \
        "$ZLIB_URL" --output "$archive"
fi
echo "$ZLIB_SHA256  $archive" | sha256sum --check --status \
    || die "SHA256 mismatch for $archive"

mkdir -p -- "$run_dir/src"
tar --extract --gzip --file="$archive" --directory="$run_dir/src" \
    --no-same-owner --no-same-permissions
[[ -f $source_dir/configure ]] || die "archive did not contain zlib-$ZLIB_VERSION/configure"

readonly target_flags='-EL -mips32r2 -mabi=32 -mhard-float -mfp32'
readonly cc_flags="$target_flags -O2 -fPIC"
readonly ld_flags="$target_flags"
mkdir -p -- "$object_dir"
pushd "$object_dir" >/dev/null
CC="$compiler" CFLAGS="$cc_flags" SFLAGS="$cc_flags" \
    LDSHARED="$compiler $ld_flags -shared -Wl,-EL,-soname,libz.so.1,--version-script,$source_dir/zlib.map" \
    CHOST=mips-linux-gnu "$source_dir/configure" --prefix=/usr --shared \
    --libdir=/usr/lib --includedir=/usr/include
make -j"$jobs"
make install DESTDIR="$stage_dir"
popd >/dev/null

[[ -f $output_lib ]] || die "expected staged library is missing: $output_lib"

readelf_field() {
    readelf -h "$1" | awk -F: -v key="$2" '$1 ~ "^[[:space:]]*" key "[[:space:]]*$" {sub(/^[[:space:]]+/, "", $2); print $2}'
}
readelf_soname() { readelf -d "$1" | awk -F'[][]' '/SONAME/ {print $2}'; }
readelf_needed() { readelf -d "$1" | awk -F'[][]' '/NEEDED/ {print $2}' | sort; }
readelf_exported_symbols() {
    readelf --dyn-syms --wide "$1" | awk '
        ($5 == "GLOBAL" || $5 == "WEAK") && $6 == "DEFAULT" && $7 != "UND" && $8 != "" {
            symbol = $8
            sub(/@@?.*$/, "", symbol)
            if (symbol !~ /^ZLIB_[0-9]/ && symbol !~ /^(_init|_fini|_edata|_end|__bss_start)$/) print symbol
        }' | sort -u
}

for field in Class Data Machine; do
    [[ $(readelf_field "$output_lib" "$field") == "$(readelf_field "$stock_zlib" "$field")" ]] \
        || die "ELF $field differs from stock"
done
new_flags=$(readelf -h "$output_lib" | awk -F: '/Flags:/ {sub(/^[[:space:]]+/, "", $2); print $2}')
[[ $new_flags == *o32* && $new_flags == *mips32r2* ]] || die "not MIPS32r2/o32: $new_flags"
new_fp=$(readelf -A "$output_lib" | awk -F: '/Tag_GNU_MIPS_ABI_FP/ {sub(/^[[:space:]]+/, "", $2); print $2}')
[[ $new_fp == Hard\ float* ]] || die "not hard-float: $new_fp"

[[ $(readelf_soname "$output_lib") == 'libz.so.1' ]] || die "unexpected new SONAME"
[[ $(readelf_soname "$stock_zlib") == 'libz.so.1' ]] || die "stock SONAME is not libz.so.1"
[[ $(readelf_needed "$output_lib") == "$(readelf_needed "$stock_zlib")" ]] \
    || die "dynamic dependencies differ from stock"

stock_symbols=$(readelf_exported_symbols "$stock_zlib")
new_symbols=$(readelf_exported_symbols "$output_lib")
while read -r symbol; do
    [[ -z $symbol ]] && continue
    grep -Fxq "$symbol" <<<"$new_symbols" || die "stock export missing from new library: $symbol"
done <<<"$stock_symbols"

stock_versions=$(readelf --version-info "$stock_zlib" 2>/dev/null | awk '/Name:/ {for (i = 1; i <= NF; i++) if ($i ~ /^GLIBC_[0-9]/) print $i}' | sort -u)
new_versions=$(readelf --version-info "$output_lib" 2>/dev/null | awk '/Name:/ {for (i = 1; i <= NF; i++) if ($i ~ /^GLIBC_[0-9]/) print $i}' | sort -u)
while read -r version; do
    [[ -z $version ]] && continue
    grep -Fxq "$version" <<<"$stock_versions" || die "new GLIBC requirement absent from stock: $version"
done <<<"$new_versions"

echo "Built and verified: $output_lib"
echo "Compiler: $compiler"
echo "Stock ABI reference: $stock_zlib"
echo "SHA256: $(sha256sum "$output_lib" | awk '{print $1}')"
[[ ! -e "$build_dir/current" || -L "$build_dir/current" ]] || die 'current must be a symlink'
ln -sfnT "$stage_dir" "$build_dir/current"
