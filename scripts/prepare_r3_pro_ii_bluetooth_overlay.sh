#!/usr/bin/env bash
# Build a self-contained R3 Pro II Bluetooth runtime overlay.
#
# The stock root is an input only.  The resulting overlay is intended to be
# merged by the image packager; vendor firmware, UART helpers and all other
# stock files therefore remain in the stock image and are never copied here.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly SCRIPT_DIR
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
readonly REPO_ROOT
readonly BASE_DIR=${BASE_UPGRADE_DIR:-$REPO_ROOT/scratch/base-upgrade}
readonly DEFAULT_STOCK='/home/josegarita/Desktop/Hiby R3 Pro II/squashfs-root'
stock=${BASE_STOCK_ROOT:-$DEFAULT_STOCK}
output=${R3_BT_OVERLAY_DIR:-$BASE_DIR/r3-pro-ii-bluetooth-overlay}
cross=${BASE_CROSS_PREFIX:-$REPO_ROOT/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-}

usage() {
    echo "Usage: $0 [--stock-root DIR] [--output DIR]" >&2
}
die() { echo "prepare_r3_pro_ii_bluetooth_overlay.sh: $*" >&2; exit 1; }

while (($#)); do
    case $1 in
        --stock-root) (($# >= 2)) || { usage; exit 2; }; stock=$2; shift 2 ;;
        --output) (($# >= 2)) || { usage; exit 2; }; output=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; die "unknown option: $1" ;;
    esac
done

[[ -d $stock/usr/lib && -d $stock/usr/bin ]] || die "stock root is incomplete: $stock"
[[ -d $BASE_DIR/bluez/stage/usr && -d $BASE_DIR/bluealsa/stage/usr ]] ||
    die "build BlueZ and BlueALSA first (missing stage under $BASE_DIR)"

bluez=$BASE_DIR/bluez/stage
bluez_lib=$BASE_DIR/bluez-library/stage
bluealsa=$BASE_DIR/bluealsa/stage
audio=$BASE_DIR/audio/stage
glib=$BASE_DIR/glib/stage
dbus=$BASE_DIR/dbus/stage
codecs=$BASE_DIR/codecs/stage
zlib=$BASE_DIR/zlib/current
expat=$BASE_DIR/expat/current

# Validate every stage before creating output, so a failed build cannot leave
# a plausible but incomplete overlay behind.
for stage in "$bluez_lib" "$bluealsa" "$audio" "$glib" "$dbus" "$codecs" "$zlib" "$expat"; do
    [[ -d $stage/usr/lib ]] || die "missing runtime stage: $stage"
done
[[ -d $bluez/usr/bin && -d $bluez/usr/libexec/bluetooth ]] || die "BlueZ stage is incomplete: $bluez"
for binary in bluetoothctl btattach btmon hciattach hciconfig hcitool sdptool gatttool; do
    [[ -x $bluez/usr/bin/$binary ]] || die "missing BlueZ tool: $bluez/usr/bin/$binary"
done
[[ -x $bluez/usr/libexec/bluetooth/bluetoothd ]] || die "missing staged bluetoothd"
for binary in bluealsad bluealsactl bluealsa-aplay; do
    [[ -x $bluealsa/usr/bin/$binary ]] || die "missing BlueALSA binary: $binary"
done
for module in "$bluealsa"/usr/lib/alsa-lib/libasound_module_*bluealsa*.so; do
    [[ -f $module ]] || die "missing BlueALSA ALSA plugin"
done
for script in bt_init bt_resume bt_suspend bluealsa_profile; do
    [[ -f $stock/usr/bin/$script ]] || die "missing stock script: $stock/usr/bin/$script"
done

run=$(mktemp -d "$BASE_DIR/r3-pro-ii-bluetooth-overlay.run.XXXXXX")
overlay=$run/root
mkdir -p "$overlay/usr/bin" "$overlay/usr/lib" "$overlay/usr/lib/alsa-lib" \
    "$overlay/usr/libexec/bluetooth" "$overlay/usr/share/alsa/alsa.conf.d" \
    "$overlay/etc/alsa/conf.d" "$overlay/etc/dbus-1/system.d" \
    "$overlay/usr/share/dbus-1/interfaces" "$overlay/etc/bluetooth"

copy_shared_libs() {
    local stage=$1 lib
    for lib in "$stage"/usr/lib/lib*.so*; do
        [[ -e $lib || -L $lib ]] || continue
        cp -a "$lib" "$overlay/usr/lib/"
    done
}

# These are the complete target-side dependency stages used by the builds.
# Copying all shared objects from each stage keeps SONAME symlinks and catches
# transitive dependencies without importing archives or host build metadata.
for stage in "$audio" "$bluez_lib" "$glib" "$dbus" "$codecs" "$zlib" "$expat"; do
    copy_shared_libs "$stage"
done

cp -a "$bluez/usr/libexec/bluetooth/bluetoothd" "$overlay/usr/libexec/bluetooth/"
for binary in "$bluez"/usr/bin/*; do
    [[ -f $binary && -x $binary ]] || continue
    cp -a "$binary" "$overlay/usr/bin/"
done
# BlueZ's policy is needed by the new daemon.  Keep the stock main.conf so
# device-specific settings and persistent paths remain under vendor control.
cp -a "$bluez/etc/dbus-1/dbus-1/system.d/bluetooth.conf" "$overlay/etc/dbus-1/system.d/"

# libdbus's private ABI changes with the staged build, so install its daemon
# and client tools as one unit.  The stock system bus policy and init wiring
# remain outside this overlay.
for binary in "$dbus"/usr/bin/*; do
    [[ -f $binary && -x $binary ]] || continue
    cp -a "$binary" "$overlay/usr/bin/"
done
[[ -f $dbus/usr/libexec/dbus-daemon-launch-helper ]] &&
    cp -a "$dbus/usr/libexec/dbus-daemon-launch-helper" "$overlay/usr/libexec/"
if [[ -e $stock/usr/libexec/dbus-daemon-launch-helper ]]; then
    chmod --reference="$stock/usr/libexec/dbus-daemon-launch-helper" \
        "$overlay/usr/libexec/dbus-daemon-launch-helper"
fi

for binary in bluealsad bluealsactl bluealsa-aplay; do cp -a "$bluealsa/usr/bin/$binary" "$overlay/usr/bin/"; done
cp -a "$bluealsa"/usr/lib/alsa-lib/libasound_module_*bluealsa*.so "$overlay/usr/lib/alsa-lib/"
cp -a "$bluealsa/usr/share/alsa/alsa.conf.d/20-bluealsa.conf" "$overlay/usr/share/alsa/alsa.conf.d/"
cp -a "$bluealsa/etc/dbus-1/system.d/org.bluealsa.conf" "$overlay/etc/dbus-1/system.d/"
cp -a "$bluealsa/usr/share/dbus-1/interfaces/org.bluealsa.xml" "$overlay/usr/share/dbus-1/interfaces/"
ln -s /usr/share/alsa/alsa.conf.d/20-bluealsa.conf "$overlay/etc/alsa/conf.d/20-bluealsa.conf"

# The speexrate module is built by the audio stage and is required for the
# advertised resampling behavior.  It is intentionally selected explicitly.
speex_module="$audio"/usr/lib/alsa-lib/libasound_module_rate_speexrate.so
[[ -f $speex_module ]] || die "missing Speex ALSA rate plugin: $speex_module"
cp -a "$audio"/usr/lib/alsa-lib/libasound_module_rate_speexrate*.so "$overlay/usr/lib/alsa-lib/"

# Carry source licenses with the redistributable runtime.  These are all
# local source/build inputs; no vendor firmware or stock proprietary files are
# included in the overlay.
licenses="$overlay/usr/share/licenses/r3-bluetooth"
copy_notice() {
    local package=$1 source
    shift
    mkdir -p "$licenses/$package"
    for source in "$@"; do
        [[ -e $source ]] || die "missing license source: $source"
        cp -a "$source" "$licenses/$package/"
    done
}
glib_src=$(dirname "$(readlink -f "$glib")")/src
copy_notice bluealsa "$REPO_ROOT/scratch/bluez-alsa-5.0.0/LICENSE" \
    "$REPO_ROOT/scratch/bluez-alsa-5.0.0/LICENSES"
copy_notice bluez "$BASE_DIR/bluez-5.87/COPYING" "$BASE_DIR/bluez-5.87/COPYING.LIB"
copy_notice bluez-library "$BASE_DIR/bluez-5.87/COPYING" "$BASE_DIR/bluez-5.87/COPYING.LIB"
copy_notice alsa "$BASE_DIR/audio/src/alsa-lib-1.2.16/COPYING"
copy_notice sbc "$BASE_DIR/audio/src/sbc-2.2/COPYING" "$BASE_DIR/audio/src/sbc-2.2/COPYING.LIB"
copy_notice speexdsp "$BASE_DIR/audio/src/speexdsp-1.2.1/COPYING"
copy_notice dbus "$REPO_ROOT/dbus/COPYING"
copy_notice glib "$glib_src/glib-2.84.4/COPYING" "$glib_src/glib-2.84.4/LICENSES"
copy_notice pcre2 "$glib_src/pcre2-10.46/COPYING"
copy_notice libffi "$glib_src/libffi-3.4.8/LICENSE"
copy_notice zlib "$(dirname "$(readlink -f "$zlib")")/src/zlib-1.3.2/LICENSE"
copy_notice expat "$BASE_DIR/expat/src/expat-2.8.4/COPYING"
copy_notice fdk-aac "$BASE_DIR/fdk-aac/NOTICE"
copy_notice libopenaptx "$BASE_DIR/libopenaptx/COPYING"

# Preserve exact vendor script bodies except for the executable daemon name.
# This keeps UART setup, firmware loading, and vendor helper invocation intact.
for script in bt_init bt_resume bt_suspend bluealsa_profile; do
    sed -e 's|/usr/bin/bluealsa\([[:space:]]\)|/usr/bin/bluealsad\1|g' \
        -e 's/[[:space:]]--a2dp-volume//g' \
        -e 's/pgrep "bluealsa"/pgrep "bluealsad"/g' \
        -e '/^killall/s/\bbluealsa\b/bluealsad/g' \
        "$stock/usr/bin/$script" > "$overlay/usr/bin/$script"
    # Match the player's default codec and 44.1 kHz transport preference at
    # boot, so it need not restart a daemon after a headset has connected.
    sed -i 's|/usr/bin/bluealsad -p a2dp-source |/usr/bin/bluealsad -p a2dp-source --all-codecs --a2dp-force-audio-cd |g' \
        "$overlay/usr/bin/$script"
    chmod --reference="$stock/usr/bin/$script" "$overlay/usr/bin/$script"
    sh -n "$overlay/usr/bin/$script"
done

# Strip only target ELF files.  No compiler or proprietary stock content is
# tracked in the repository; the toolchain is an explicit local prerequisite.
[[ -x ${cross}strip ]] || die "missing target strip: ${cross}strip"
sanitize_host_rpath() {
    local file=$1 bad
    bad=$(readelf -d "$file" | sed -n 's/.*Library rpath: \[\(.*\)\].*/\1/p; s/.*Library runpath: \[\(.*\)\].*/\1/p' | head -n 1)
    [[ -n $bad ]] || return 0
    [[ $bad == */home/* || $bad == */scratch/* || $bad == /usr/local/* ]] || return 0
    # Some older libtool-built staged plugins contain an absolute build-tree
    # RPATH.  Replace its string in the disposable overlay with /usr/lib,
    # retaining the ELF dynamic tag while removing the host path.  The source
    # stage is never modified.
    BAD_RPATH=$bad perl -0777 -pi -e '
        my $bad = $ENV{BAD_RPATH}; my $new = "/usr/lib";
        die "RPATH replacement is longer than source" if length($new) > length($bad);
        $new .= "\0" x (length($bad) - length($new));
        die "RPATH string not found" unless s/\Q$bad\E/$new/;
    ' "$file"
}
while IFS= read -r -d '' file; do
    [[ $(file -b "$file") == ELF* ]] || continue
    "${cross}strip" --strip-unneeded "$file"
    sanitize_host_rpath "$file"
    if readelf -d "$file" | grep -E '(RPATH|RUNPATH).*/(home|scratch|usr/local)' >/dev/null; then
        die "host runtime path in $file"
    fi
done < <(find "$overlay/usr" -type f -print0)

# The runtime overlay is applied after board-specific player files.  Keep this
# invariant explicit so a future stage cannot accidentally replace them.
[[ ! -e $overlay/usr/bin/compas_player &&
   ! -e $overlay/usr/bin/open_hiby_player &&
   ! -e $overlay/usr/bin/open_hiby_bootloader ]] ||
    die 'overlay must not contain board-specific player/bootloader binaries'

(cd "$overlay" && find . \( -type f -o -type l \) -print | LC_ALL=C sort) > "$run/files.txt"
[[ ! -e $output || -L $output ]] || die "output must be absent or a symlink: $output"
ln -sfnT "$overlay" "$output"
printf 'R3 Pro II Bluetooth overlay: %s\nManifest: %s\nStock root was not modified.\n' "$overlay" "$run/files.txt"
