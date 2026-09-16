#!/usr/bin/env bash
# Build an isolated, ABI-compatible Wi-Fi userspace for the R1.
#
# This stages only wpa_supplicant, wpa_cli, libwpa_client, and libnl under
# scratch/base-upgrade/wifi. It never writes the firmware extraction tree.
# OpenSSL 1.1.1f, readline, and ncurses are link/runtime inputs from the
# stock image; they are intentionally not rebuilt or replaced here.
set -euo pipefail

readonly SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
readonly WPA_VERSION=2.12
readonly WPA_SHA256=08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6
readonly LIBNL_VERSION=3.12.0
readonly LIBNL_SHA256=fc51ca7196f1a3f5fdf6ffd3864b50f4f9c02333be28be4eeca057e103c0dd18
readonly OPENSSL_VERSION=1.1.1f
readonly OPENSSL_SHA256=186c6bfe6ecfba7a5b48c47f8a1673d0f3b0e5ba2e25602dd23b629975da3f35
readonly READLINE_SHA256=e339f51971478d369f8a053a330a190781acb9864cf4c541060f12078948e461
readonly TARGET_FLAGS='-EL -mips32r2 -mabi=32 -mhard-float -mfp32'
readonly PREFIX=/usr

compiler=${MIPS_GCC:-$REPO_ROOT/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-gcc}
work=${BASE_WIFI_BUILD_DIR:-$REPO_ROOT/scratch/base-upgrade/wifi}
stock_root=${BASE_STOCK_ROOT:-/home/josegarita/Desktop/Test2/squashfs-root}
openssl_include=${OPENSSL_INCLUDE_DIR:-}
jobs=${JOBS:-4}

usage() {
    cat >&2 <<EOF
Usage: $0 [--compiler PATH] [--build-dir DIR] [--stock-root DIR]
          [--openssl-include DIR] [--jobs N]

Build wpa_supplicant $WPA_VERSION, wpa_cli, libwpa_client, and libnl
$LIBNL_VERSION for MIPS32r2 hard-float glibc 2.22.
Defaults: compiler=$compiler
          build-dir=$work
          stock-root=$stock_root
EOF
}

while (($#)); do
    case $1 in
        --compiler) (($# >= 2)) || { usage; exit 2; }; compiler=$2; shift 2 ;;
        --build-dir) (($# >= 2)) || { usage; exit 2; }; work=$2; shift 2 ;;
        --stock-root) (($# >= 2)) || { usage; exit 2; }; stock_root=$2; shift 2 ;;
        --openssl-include) (($# >= 2)) || { usage; exit 2; }; openssl_include=$2; shift 2 ;;
        --jobs) (($# >= 2)) || { usage; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage 2>&1; exit 0 ;;
        *) echo "build_base_wifi.sh: unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

die() { echo "build_base_wifi.sh: $*" >&2; exit 1; }

[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "jobs must be a positive integer: $jobs"
[[ -x $compiler ]] || die "MIPS compiler is not executable: $compiler"
[[ -d $stock_root ]] || die "stock root is not a directory: $stock_root"
for tool in curl sha256sum tar make readelf file objdump mktemp awk sed rg pkg-config; do
    command -v "$tool" >/dev/null || die "$tool is required"
done

readonly stock_wpa=$stock_root/usr/sbin/wpa_supplicant
readonly stock_cli=$stock_root/usr/sbin/wpa_cli
readonly stock_client=$stock_root/usr/lib/libwpa_client.so
readonly stock_ssl=$stock_root/usr/lib/libssl.so.1.1
readonly stock_crypto=$stock_root/usr/lib/libcrypto.so.1.1
readonly stock_nl=$stock_root/usr/lib/libnl-3.so.200
readonly stock_nl_genl=$stock_root/usr/lib/libnl-genl-3.so.200
readonly stock_readline=$stock_root/usr/lib/libreadline.so.8
readonly stock_ncurses=$stock_root/usr/lib/libncurses.so.6
for input in "$stock_wpa" "$stock_cli" "$stock_client" "$stock_ssl" "$stock_crypto" \
    "$stock_nl" "$stock_nl_genl" "$stock_readline" "$stock_ncurses"; do
    [[ -f $input || -L $input ]] || die "required stock input is missing: $input"
done

mkdir -p -- "$work/downloads"
[[ ! -L $work ]] || die "build directory must not be a symlink: $work"
work=$(cd -- "$work" && pwd)
readonly run_dir=$(mktemp -d "$work/run.XXXXXX")
readonly source_root=$run_dir/src
readonly build_root=$run_dir/build
readonly stage=$run_dir/stage
readonly sysroot=$run_dir/sysroot
readonly stable_stage=$work/stage
mkdir -p -- "$source_root" "$build_root" "$stage/usr/sbin" "$stage/usr/lib" \
    "$stage/usr/include" "$stage/usr/share/licenses/wifi" "$sysroot/usr/lib"

fetch() {
    local url=$1 archive=$2 sha=$3 path="$work/downloads/$2"
    [[ ! -L $path ]] || die "archive path must not be a symlink: $path"
    if [[ ! -f $path ]]; then
        curl --fail --location --proto '=https' --proto-redir '=https' --silent --show-error \
            --retry 3 "$url" --output "$path.part"
        mv -- "$path.part" "$path"
    fi
    printf '%s  %s\n' "$sha" "$path" | sha256sum --check --status \
        || die "SHA256 mismatch: $path"
    printf '%s\n' "$path"
}

readonly libnl_archive=$(fetch \
    "https://github.com/thom311/libnl/releases/download/libnl3_12_0/libnl-$LIBNL_VERSION.tar.gz" \
    "libnl-$LIBNL_VERSION.tar.gz" "$LIBNL_SHA256")
readonly wpa_archive=$(fetch \
    "https://w1.fi/releases/wpa_supplicant-$WPA_VERSION.tar.gz" \
    "wpa_supplicant-$WPA_VERSION.tar.gz" "$WPA_SHA256")
readonly openssl_archive=$(fetch \
    "https://www.openssl.org/source/old/1.1.1/openssl-$OPENSSL_VERSION.tar.gz" \
    "openssl-$OPENSSL_VERSION.tar.gz" "$OPENSSL_SHA256")
readline_archive=$(fetch https://ftp.gnu.org/gnu/readline/readline-8.0.tar.gz \
    readline-8.0.tar.gz "$READLINE_SHA256")
tar -xzf "$libnl_archive" -C "$source_root"
tar -xzf "$wpa_archive" -C "$source_root"
tar -xzf "$openssl_archive" -C "$source_root"
tar -xzf "$readline_archive" -C "$source_root"
readonly libnl_src=$source_root/libnl-$LIBNL_VERSION
readonly wpa_src=$source_root/wpa_supplicant-$WPA_VERSION
readonly openssl_src=$source_root/openssl-$OPENSSL_VERSION
if [[ -z $openssl_include ]]; then
    openssl_include=$openssl_src/include
fi
[[ -f $openssl_include/openssl/ssl.h ]] || die "OpenSSL headers not found under $openssl_include"
[[ -f $libnl_src/configure ]] || die "libnl archive layout is unexpected"
[[ -f $wpa_src/wpa_supplicant/defconfig ]] || die "wpa archive layout is unexpected"

readonly cross_prefix=${compiler%gcc}
readonly cc=$compiler
readonly ar=${cross_prefix}ar
readonly ranlib=${cross_prefix}ranlib
readonly strip=${cross_prefix}strip
readonly nm=${cross_prefix}nm
readonly ld=${cross_prefix}ld
for tool in "$cc" "$ar" "$ranlib" "$strip" "$nm" "$ld"; do
    [[ -x $tool ]] || die "missing cross tool: $tool"
done

echo "Compiler: $($cc --version | head -n 1)"
echo "Target flags: $TARGET_FLAGS"
echo "Stock OpenSSL dependency: $stock_ssl / $stock_crypto"
echo "Stock CLI dependencies retained: $stock_readline / $stock_ncurses"

# Generate only the target-specific public configuration header. No OpenSSL
# library is compiled or installed: consumers retain the stock runtime ABI.
if [[ ! -f "$openssl_include/openssl/opensslconf.h" ]]; then
    (
        cd "$openssl_src"
        ./Configure linux-mips32 no-asm --cross-compile-prefix="$cross_prefix"
        make include/openssl/opensslconf.h
    )
fi

# Build libnl first. The SONAMEs remain libnl-3.so.200 and libnl-genl-3.so.200,
# preserving the ABI expected by nl80211 and the vendor's old kernel/userspace.
libnl_build=$build_root/libnl
libnl_stage=$run_dir/libnl-stage
mkdir -p -- "$libnl_build" "$libnl_stage"
(
    cd "$libnl_build"
    CC="$cc" CXX="${cross_prefix}g++" AR="$ar" RANLIB="$ranlib" STRIP="$strip" \
    CFLAGS="$TARGET_FLAGS -Os -fPIC" CXXFLAGS="$TARGET_FLAGS -Os -fPIC" CPPFLAGS="$TARGET_FLAGS" LDFLAGS="$TARGET_FLAGS" \
    "$libnl_src/configure" --host=mips-linux-gnu --build="$(gcc -dumpmachine)" \
        --prefix="$PREFIX" --libdir=/usr/lib --disable-static --enable-cli=no \
        --disable-doxygen
    # Wi-Fi needs only core/generic netlink. Routing/NF modules would add
    # unrelated dependencies on Linux UAPI headers newer than this SDK.
    make -j"$jobs" lib/libnl-3.la lib/libnl-genl-3.la
    # Use libtool's install/relink step: copying uninstalled .libs binaries
    # directly can retain a build-directory RPATH in libnl-genl.
    make DESTDIR="$libnl_stage" install-libLTLIBRARIES \
        lib_LTLIBRARIES='lib/libnl-3.la lib/libnl-genl-3.la'
)
mkdir -p "$libnl_stage/usr/lib/pkgconfig" "$libnl_stage/usr/include/libnl3"
cp "$libnl_build/libnl-3.0.pc" "$libnl_build/libnl-genl-3.0.pc" "$libnl_stage/usr/lib/pkgconfig/"
cp -a "$libnl_src/include/netlink" "$libnl_stage/usr/include/libnl3/"
cp "$libnl_build/include/netlink/version.h" "$libnl_stage/usr/include/libnl3/netlink/"
[[ -f $libnl_stage/usr/lib/libnl-3.so.200 ]] || die "libnl core library was not staged"
[[ -f $libnl_stage/usr/lib/libnl-genl-3.so.200 ]] || die "libnl genl library was not staged"
[[ -f $libnl_stage/usr/lib/pkgconfig/libnl-3.0.pc ]] || die "libnl pkg-config file missing"
sed -i -E 's#-Wl,-rpath(=[^ ]+| [^ ]+)##g' "$libnl_stage/usr/lib/pkgconfig/"*.pc

# Assemble a link-only target sysroot. The stock OpenSSL/readline/ncurses
# copies are inputs, never outputs; no existing firmware file is modified.
cp -a "$libnl_stage/usr/." "$sysroot/usr/"
cp -a "$libnl_stage/usr/." "$stage/usr/"
for library in \
    "$stock_ssl" "$stock_crypto" "$stock_readline" "$stock_ncurses"; do
    cp -aL "$library" "$sysroot/usr/lib/"
done
ln -s libssl.so.1.1 "$sysroot/usr/lib/libssl.so"
ln -s libcrypto.so.1.1 "$sysroot/usr/lib/libcrypto.so"
ln -s libreadline.so.8 "$sysroot/usr/lib/libreadline.so"
ln -s libncurses.so.6 "$sysroot/usr/lib/libncurses.so"
cp -a "$openssl_include/openssl" "$sysroot/usr/include/"
mkdir -p "$sysroot/usr/include/readline"
cp "$source_root/readline-8.0/"*.h "$sysroot/usr/include/readline/"

export CC="$cc" AR="$ar" RANLIB="$ranlib" STRIP="$strip" NM="$nm" LD="$ld"
export CFLAGS="$TARGET_FLAGS -Os -fPIC -I$sysroot/usr/include"
export CPPFLAGS="$TARGET_FLAGS -I$sysroot/usr/include -I$openssl_include"
export LDFLAGS="$TARGET_FLAGS -L$sysroot/usr/lib -Wl,-rpath-link,$sysroot/usr/lib -Wl,--no-undefined"
export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"
unset PKG_CONFIG_PATH

for package in libnl-3.0 libnl-genl-3.0; do
    pkg-config --modversion "$package" >/dev/null || die "target pkg-config package missing: $package"
done

# Use the reviewed station configuration, not upstream's expanding collection
# of AP, MACsec, D-Bus and P2P defaults.
cp "$SCRIPT_DIR/base_image/wpa.config" "$build_root/wpa.config"
cp "$build_root/wpa.config" "$wpa_src/wpa_supplicant/.config"

(
    cd "$wpa_src/wpa_supplicant"
    make -j"$jobs" V=1 wpa_supplicant wpa_cli
)

for binary in wpa_supplicant wpa_cli; do
    [[ -f $wpa_src/wpa_supplicant/$binary ]] || die "missing built binary: $binary"
    cp "$wpa_src/wpa_supplicant/$binary" "$stage/usr/sbin/"
done

# CONFIG_BUILD_WPA_CLIENT_SO is the upstream target used by existing stock
# consumers. Build it from the same configured tree when supported by 2.12.
(
    cd "$wpa_src/wpa_supplicant"
    make libwpa_client.so -j"$jobs" V=1 LDFLAGS="$LDFLAGS -Wl,-soname,libwpa_client.so"
)
[[ -f $wpa_src/wpa_supplicant/libwpa_client.so ]] || die "libwpa_client.so was not built"
cp "$wpa_src/wpa_supplicant/libwpa_client.so" "$stage/usr/lib/"
cp "$wpa_src/COPYING" "$stage/usr/share/licenses/wifi/"
cp "$wpa_src/wpa_supplicant/.config" "$stage/usr/share/licenses/wifi/build.config"
cp "$libnl_src/COPYING" "$stage/usr/share/licenses/wifi/libnl-COPYING"

# Make the stage self-describing for later overlay integration.
cat > "$stage/usr/share/licenses/wifi/BUILD-INFO" <<EOF
wpa_supplicant=$WPA_VERSION
libnl=$LIBNL_VERSION
wpa_sha256=$WPA_SHA256
libnl_sha256=$LIBNL_SHA256
target=mips-linux-gnu MIPS32r2 o32 hard-float glibc-2.22
drivers=nl80211,wext
ctrl_interface=/var/run/wpa_supplicant
tls=OpenSSL 1.1.1f runtime from stock image (not rebuilt)
EOF

for elf in "$stage/usr/sbin/wpa_supplicant" "$stage/usr/sbin/wpa_cli" "$stage/usr/lib/libwpa_client.so" \
           "$stage/usr/lib/libnl-3.so.200" "$stage/usr/lib/libnl-genl-3.so.200"; do
    "$strip" --strip-unneeded "$(readlink -f "$elf")"
    file -L "$elf" | rg 'ELF 32-bit.*LSB.*MIPS' >/dev/null || die "wrong ELF class/endianness: $elf"
    readelf -h "$elf" | rg 'Flags:.*mips32r2' >/dev/null || die "not MIPS32r2/o32: $elf"
    readelf -A "$elf" | rg 'Tag_GNU_MIPS_ABI_FP:.*Hard float' >/dev/null || die "not hard-float: $elf"
    if readelf -d "$elf" | rg '(RPATH|RUNPATH)' >/dev/null; then
        die "host runtime path leaked into $elf"
    fi
done
[[ $(readelf -d "$stage/usr/lib/libwpa_client.so" | awk -F'[][]' '/SONAME/ {print $2}') == libwpa_client.so* ]] \
    || die "libwpa_client SONAME is missing or unexpected"
for binary in wpa_supplicant; do
    readelf -d "$stage/usr/sbin/$binary" | rg 'Shared library: \[libssl.so.1.1\]' >/dev/null \
        || die "$binary is not linked to stock OpenSSL 1.1 ABI"
    readelf -d "$stage/usr/sbin/$binary" | rg 'Shared library: \[libcrypto.so.1.1\]' >/dev/null \
        || die "$binary is not linked to stock libcrypto 1.1 ABI"
done
readelf -d "$stage/usr/sbin/wpa_supplicant" | rg 'Shared library: \[libnl-3.so.200\]' >/dev/null \
    || die 'wpa_supplicant is not linked to libnl-3.so.200'
readelf -d "$stage/usr/sbin/wpa_supplicant" | rg 'Shared library: \[libnl-genl-3.so.200\]' >/dev/null \
    || die 'wpa_supplicant is not linked to libnl-genl-3.so.200'

command -v qemu-mipsel >/dev/null || die 'qemu-mipsel is required'
qemu=(qemu-mipsel -L "$stock_root" -E LD_BIND_NOW=1 \
    -E "LD_LIBRARY_PATH=$stage/usr/lib:$stock_root/usr/lib:$stock_root/lib")
"${qemu[@]}" "$stage/usr/sbin/wpa_supplicant" -v
"${qemu[@]}" "$stage/usr/sbin/wpa_cli" -v
"${qemu[@]}" "$stage/usr/sbin/wpa_supplicant" -h > "$run_dir/wpa-help.txt"
for driver in nl80211 wext wired none; do
    grep -E "^  $driver =" "$run_dir/wpa-help.txt" >/dev/null || die "missing driver: $driver"
done

# Do not advance the stable pointer until every build and ABI check passed.
[[ ! -e $stable_stage || -L $stable_stage ]] || die "stable stage must be a symlink: $stable_stage"
ln -sfnT -- "$stage" "$stable_stage"
echo "Built and verified Wi-Fi stage: $stage"
echo "Stable pointer: $stable_stage"
echo "Outputs: $stage/usr/sbin/{wpa_supplicant,wpa_cli}, $stage/usr/lib/libwpa_client.so"
echo "SHA256 wpa_supplicant: $(sha256sum "$stage/usr/sbin/wpa_supplicant" | awk '{print $1}')"
echo "SHA256 wpa_cli: $(sha256sum "$stage/usr/sbin/wpa_cli" | awk '{print $1}')"
echo "SHA256 libwpa_client: $(sha256sum "$stage/usr/lib/libwpa_client.so" | awk '{print $1}')"
