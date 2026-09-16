#!/usr/bin/env bash
# Isolated DBus 1.16.2 cross-build; output is only under scratch/base-upgrade/dbus.
set -euo pipefail

readonly SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
readonly SOURCE=${BASE_DBUS_SOURCE:-$REPO_ROOT/dbus}
work=${BASE_DBUS_BUILD_DIR:-$REPO_ROOT/scratch/base-upgrade/dbus}
readonly PREFIX=/usr
readonly CROSS_PREFIX=${BASE_CROSS_PREFIX:-$REPO_ROOT/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-}
readonly EXPAT_STAGE=${BASE_EXPAT_STAGE:-$REPO_ROOT/scratch/base-upgrade/expat/current}
readonly COMMIT=958bf9db2100553bcd2fe2a854e1ebb42e886054
readonly TARGET_FLAGS='-EL -mips32r2 -mabi=32 -mhard-float -mfp32'

die() { echo "build_base_dbus.sh: $*" >&2; exit 1; }

[[ $# -eq 0 ]] || die "usage: $0"
for tool in meson ninja git readelf file mktemp pkg-config rg; do
    command -v "$tool" >/dev/null || die "$tool is required"
done
[[ -d $SOURCE/.git ]] || die "DBus source is not a git checkout: $SOURCE"
[[ $(git -C "$SOURCE" rev-parse HEAD) == "$COMMIT" ]] || die "DBus checkout is not $COMMIT"
[[ -z $(git -C "$SOURCE" status --porcelain) ]] || die "DBus checkout is not clean"
[[ $(git -C "$SOURCE" describe --tags --exact-match) == dbus-1.16.2 ]] || die "DBus tag mismatch"
[[ -f $EXPAT_STAGE/usr/lib/pkgconfig/expat.pc ]] || die "missing staged Expat: $EXPAT_STAGE"

cc="${CROSS_PREFIX}gcc"; ar="${CROSS_PREFIX}ar"
ranlib="${CROSS_PREFIX}ranlib"; strip="${CROSS_PREFIX}strip"
for tool in "$cc" "$ar" "$ranlib" "$strip"; do
    [[ -x $tool ]] || die "missing cross tool: $tool"
done

mkdir -p -- "$work"
[[ ! -L $work ]] || die "build directory must not be a symlink: $work"
work=$(cd -- "$work" && pwd)
readonly RUN=$(mktemp -d "$work/run.XXXXXX")
readonly BUILD=$RUN/build
readonly STAGE=$RUN/stage
readonly CROSS_FILE=$RUN/cross-file.ini
readonly STABLE_STAGE=$work/stage
mkdir -p -- "$BUILD" "$STAGE"

cat >"$CROSS_FILE" <<EOF
[binaries]
c = '$cc'
ar = '$ar'
strip = '$strip'
ranlib = '$ranlib'
pkg-config = 'pkg-config'

[properties]
sys_root = '$EXPAT_STAGE'
needs_exe_wrapper = true

[host_machine]
system = 'linux'
subsystem = 'linux'
kernel = 'linux'
cpu_family = 'mips'
cpu = 'mips32r2'
endian = 'little'
EOF

export CC="$cc" AR="$ar" RANLIB="$ranlib" STRIP="$strip"
export CFLAGS="$TARGET_FLAGS -O2 -fPIC"
export CPPFLAGS="$TARGET_FLAGS -I$EXPAT_STAGE/usr/include"
export LDFLAGS="$TARGET_FLAGS -L$EXPAT_STAGE/usr/lib -Wl,-rpath-link,$EXPAT_STAGE/usr/lib"
export PKG_CONFIG_LIBDIR="$EXPAT_STAGE/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$EXPAT_STAGE"
unset PKG_CONFIG_PATH

"$cc" --version | head -n 1
meson setup "$BUILD" "$SOURCE" --cross-file "$CROSS_FILE" \
    --prefix="$PREFIX" --libdir=lib --buildtype=minsize \
    --default-library=shared --wrap-mode=nodownload -Db_staticpic=false \
    -Dmessage_bus=true -Dtools=true -Dsystemd=disabled -Dselinux=disabled \
    -Dapparmor=disabled -Dx11_autolaunch=disabled -Dmodular_tests=disabled \
    -Dinstalled_tests=false -Dxml_docs=disabled -Ddoxygen_docs=disabled \
    -Dducktype_docs=disabled -Dqt_help=disabled -Dvalgrind=disabled \
    -Dlaunchd=disabled -Dkqueue=disabled -Dinotify=enabled -Depoll=enabled \
    -Ddbus_user=dbus \
    -Drelocation=disabled -Druntime_dir=/run \
    -Dsystem_socket=/run/dbus/system_bus_socket -Dsystem_pid_file=/run/dbus/pid \
    -Ddbus_daemondir=/usr/bin
meson compile -C "$BUILD" -j "${JOBS:-4}"
DESTDIR="$STAGE" meson install -C "$BUILD" --no-rebuild

lib="$STAGE/usr/lib/libdbus-1.so.3"
[[ -L $lib && -f $lib ]] || die "missing libdbus SONAME link"
payload=$(find "$STAGE/usr/lib" -maxdepth 1 -type f -name 'libdbus-1.so.3.*' -print -quit)
[[ -n $payload ]] || die "missing libdbus payload"
[[ -f $STAGE/usr/lib/pkgconfig/dbus-1.pc ]] || die "missing dbus-1.pc"
[[ -f $STAGE/usr/bin/dbus-send && -f $STAGE/usr/bin/dbus-monitor ]] || die "missing CLI"
[[ -f $STAGE/usr/bin/dbus-daemon ]] || die "missing daemon"

for elf in "$payload" "$STAGE/usr/bin/"*; do
    [[ -f $elf ]] || continue
    file "$elf" | rg -q 'ELF 32-bit.*LSB.*MIPS' || die "wrong ELF: $elf"
    readelf -h "$elf" | rg -q 'Flags:.*mips32r2' || die "wrong MIPS ABI: $elf"
    readelf -A "$elf" | rg -q 'Tag_GNU_MIPS_ABI_FP:.*Hard float' || die "wrong FP ABI: $elf"
done
grep -Eq '^Libs:.*-ldbus-1' "$STAGE/usr/lib/pkgconfig/dbus-1.pc" || die "bad dbus-1.pc"
if rg -n '/home/|/usr/local|/lib/x86_64|/lib64' "$STAGE/usr/lib/pkgconfig/dbus-1.pc" >/dev/null; then
    die "host path leaked into dbus-1.pc"
fi

echo "Staged DBus 1.16.2: $STAGE"
echo "libdbus: $lib"
echo "CLI/daemon: $STAGE/usr/bin"
echo "DBus source commit: $COMMIT"
echo "No files outside scratch/base-upgrade/dbus were installed."
[[ ! -e "$STABLE_STAGE" || -L "$STABLE_STAGE" ]] || die 'stage must be a symlink'
ln -sfnT -- "$STAGE" "$STABLE_STAGE"
