#!/usr/bin/env bash
# Cross-build GLib and target development files under scratch/base-upgrade/glib.
set -euo pipefail

readonly SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
readonly GLIB_VERSION=2.84.4
readonly GLIB_SHA256=8a9ea10943c36fc117e253f80c91e477b673525ae45762942858aef57631bb90
readonly FFI_VERSION=3.4.8
readonly FFI_SHA256=bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b
readonly PCRE2_VERSION=10.46
readonly PCRE2_SHA256=15fbc5aba6beee0b17aecb04602ae39432393aba1ebd8e39b7cabf7db883299f

compiler=${MIPS_GCC:-$REPO_ROOT/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-gcc}
build_dir=${BASE_GLIB_BUILD_DIR:-$REPO_ROOT/scratch/base-upgrade/glib}
zlib_stage=${BASE_ZLIB_STAGE:-$REPO_ROOT/scratch/base-upgrade/zlib/current}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}
die() { echo "build_base_glib.sh: $*" >&2; exit 1; }
usage() { echo "Usage: $0 [--compiler PATH] [--build-dir DIR] [--zlib-stage DIR] [--jobs N]" >&2; }

while (($#)); do
    case $1 in
        --compiler) (($# >= 2)) || { usage; exit 2; }; compiler=$2; shift 2 ;;
        --build-dir) (($# >= 2)) || { usage; exit 2; }; build_dir=$2; shift 2 ;;
        --zlib-stage) (($# >= 2)) || { usage; exit 2; }; zlib_stage=$2; shift 2 ;;
        --jobs) (($# >= 2)) || { usage; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; die "unknown option: $1" ;;
    esac
done
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "jobs must be a positive integer: $jobs"
[[ -x $compiler ]] || die "MIPS compiler is not executable: $compiler"
[[ -d $zlib_stage/usr/include && -f $zlib_stage/usr/lib/libz.so ]] || die "zlib developer stage is incomplete: $zlib_stage"
for tool in curl sha256sum tar make meson ninja pkg-config readelf mktemp cmake; do command -v "$tool" >/dev/null || die "$tool is required"; done

mkdir -p -- "$build_dir/downloads" "$build_dir/src"
build_dir=$(cd -- "$build_dir" && pwd)
[[ ! -L $build_dir ]] || die "build directory must not be a symlink: $build_dir"
readonly run_dir=$(mktemp -d "$build_dir/run.XXXXXX")
readonly source_root=$run_dir/src
readonly build_root=$run_dir/build
readonly stage=$run_dir/stage
mkdir -p "$source_root" "$build_root" "$stage"

fetch() {
    local url=$1 archive=$2 sha=$3 path="$build_dir/downloads/$2"
    [[ ! -L $path ]] || die "archive path must not be a symlink: $path"
    if [[ ! -f $path ]]; then
        curl --fail --location --proto '=https' --proto-redir '=https' --silent --show-error --retry 3 "$url" --output "$path.part"
        mv -- "$path.part" "$path"
    fi
    printf '%s  %s\n' "$sha" "$path" | sha256sum --check --status || die "SHA256 mismatch: $path"
    printf '%s\n' "$path"
}

glib_archive=$(fetch "https://download.gnome.org/sources/glib/${GLIB_VERSION%.*}/glib-$GLIB_VERSION.tar.xz" "glib-$GLIB_VERSION.tar.xz" "$GLIB_SHA256")
ffi_archive=$(fetch "https://github.com/libffi/libffi/releases/download/v$FFI_VERSION/libffi-$FFI_VERSION.tar.gz" "libffi-$FFI_VERSION.tar.gz" "$FFI_SHA256")
pcre_archive=$(fetch "https://github.com/PhilipHazel/pcre2/releases/download/pcre2-$PCRE2_VERSION/pcre2-$PCRE2_VERSION.tar.bz2" "pcre2-$PCRE2_VERSION.tar.bz2" "$PCRE2_SHA256")
tar -xf "$glib_archive" -C "$source_root"
tar -xzf "$ffi_archive" -C "$source_root"
tar -xjf "$pcre_archive" -C "$source_root"
readonly glib_src=$source_root/glib-$GLIB_VERSION
readonly ffi_src=$source_root/libffi-$FFI_VERSION
readonly pcre_src=$source_root/pcre2-$PCRE2_VERSION

readonly target_flags='-EL -mips32r2 -mabi=32 -mhard-float -mfp32'
readonly cc_flags="$target_flags -Os -fPIC"
readonly cross_prefix=${compiler%gcc}
readonly ar=${cross_prefix}ar
readonly ranlib=${cross_prefix}ranlib
readonly strip=${cross_prefix}strip
for tool in "$ar" "$ranlib" "$strip"; do [[ -x $tool ]] || die "missing cross tool: $tool"; done
export CC=$compiler CXX=${cross_prefix}g++ AR=$ar RANLIB=$ranlib STRIP=$strip CFLAGS=$cc_flags CXXFLAGS=$cc_flags CPPFLAGS="$target_flags" LDFLAGS="$target_flags"
export PKG_CONFIG_LIBDIR="$stage/usr/lib/pkgconfig:$zlib_stage/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR=$stage
unset PKG_CONFIG_PATH
mkdir -p "$stage/usr/lib/pkgconfig"
cp -a "$zlib_stage/usr/include" "$stage/usr/"
cp -a "$zlib_stage"/usr/lib/libz.so* "$stage/usr/lib/"
cp -a "$zlib_stage/usr/lib/pkgconfig/zlib.pc" "$stage/usr/lib/pkgconfig/"

pushd "$build_root" >/dev/null
env CFLAGS="$cc_flags" LDFLAGS="$target_flags" "$ffi_src/configure" --host=mips-linux-gnu --build="$(gcc -dumpmachine)" --prefix=/usr --libdir=/usr/lib --disable-static --enable-shared
make -j"$jobs"
make install DESTDIR="$stage"
popd >/dev/null

cmake -S "$pcre_src" -B "$build_root/pcre2" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_C_COMPILER="$compiler" -DCMAKE_AR="$ar" -DCMAKE_RANLIB="$ranlib" -DCMAKE_STRIP="$strip" -DCMAKE_C_FLAGS="$cc_flags" -DCMAKE_SHARED_LINKER_FLAGS="$target_flags" -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF -DPCRE2_BUILD_PCRE2GREP=OFF
cmake --build "$build_root/pcre2" --parallel "$jobs"
DESTDIR="$stage" cmake --install "$build_root/pcre2" --prefix /usr --strip

cross_file=$run_dir/mips-linux-gnu.ini
printf '%s\n' \
    '[binaries]' \
    "c = '$compiler'" "ar = '$ar'" "strip = '$strip'" "pkg-config = '/usr/bin/pkg-config'" \
    '' '[host_machine]' "system = 'linux'" "cpu_family = 'mips'" "cpu = 'mips32r2'" "endian = 'little'" \
    '' '[properties]' 'needs_exe_wrapper = true' \
    '' '[built-in options]' "c_args = ['-EL', '-mips32r2', '-mabi=32', '-mhard-float', '-mfp32', '-Os', '-fPIC']" "c_link_args = ['-EL', '-mips32r2', '-mabi=32', '-mhard-float', '-mfp32', '-L$stage/usr/lib', '-Wl,-rpath-link,$stage/usr/lib']" > "$cross_file"
meson setup "$build_root/glib" "$glib_src" --cross-file "$cross_file" --prefix=/usr --libdir=lib --buildtype=release --default-library=shared -Dtests=false -Dinstalled_tests=false -Dintrospection=disabled -Ddocumentation=false -Dman-pages=disabled -Dnls=disabled -Dselinux=disabled -Dlibmount=disabled -Dlibelf=disabled -Dxattr=false -Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled
meson compile -C "$build_root/glib" -j "$jobs"
DESTDIR="$stage" meson install -C "$build_root/glib"

glib_lib=$stage/usr/lib/libglib-2.0.so.0
[[ -L $glib_lib && -f $glib_lib ]] || die "GLib shared library was not staged"
for pc in glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0 gthread-2.0; do [[ -f "$stage/usr/lib/pkgconfig/$pc.pc" ]] || die "missing staged pkg-config file: $pc.pc"; done
readelf -h "$glib_lib" | rg 'Class:|Data:|Machine:|Flags:'
readelf -d "$glib_lib" | rg 'SONAME|NEEDED'
printf 'Staged GLib %s and target dependencies: %s\n' "$GLIB_VERSION" "$stage/usr"
printf 'Stage manifest: %s\n' "$stage/usr/lib/pkgconfig"
[[ ! -e "$build_dir/stage" || -L "$build_dir/stage" ]] || die 'stage must be a symlink'
ln -sfnT "$stage" "$build_dir/stage"
