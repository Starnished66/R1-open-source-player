#!/usr/bin/env bash
# Build the static SD filesystem checkers installed into firmware/overlay.
# The stock image's BusyBox fsck only dispatches to fsck.<type>; it does not
# contain fsck.vfat, fsck.exfat, or ntfsfix. Both player images copy
# firmware/overlay, so one build serves the R1 and the R3 Pro II.
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cross_bin=${SD_FSCK_CROSS_BIN:-}
if [[ -z "$cross_bin" ]]; then
    if command -v mipsel-linux-musl-gcc >/dev/null 2>&1; then
        cross_bin=$(dirname -- "$(command -v mipsel-linux-musl-gcc)")
    else
        cross_bin="$HOME/opt/mipsel-linux-musl-cross/bin"
    fi
fi
export PATH="$cross_bin:$PATH"
command -v mipsel-linux-musl-gcc >/dev/null

work=${SD_FSCK_BUILD_DIR:-"$repo/scratch/sd-fsck"}
stage="$work/stage"
overlay_bin="$repo/firmware/overlay/usr/sbin"
license_dir="$repo/firmware/overlay/usr/share/licenses/sd-fsck"
mkdir -p "$work/downloads" "$work/src" "$stage" "$overlay_bin" "$license_dir"
work=$(cd -- "$work" && pwd)

fetch() {
    local package=$1
    local url=$2
    local checksum=$3
    local archive="$work/downloads/${url##*/}"
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --retry 3 --output "$archive.part" "$url"
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
    rm -rf "$work/build/$package"
    mkdir -p "$work/build/$package"
    (
        cd "$work/build/$package"
        # musl is not in every package's config.sub. The musl compiler is
        # selected explicitly, and the generic mipsel-linux triple only
        # satisfies autoconf's host check.
        CC=mipsel-linux-musl-gcc AR=mipsel-linux-musl-ar RANLIB=mipsel-linux-musl-ranlib \
            CFLAGS='-Os -no-pie' LDFLAGS='-static -no-pie' \
            "$work/src/$package/configure" --host=mipsel-linux --prefix=/usr \
            --disable-shared --enable-static "$@"
        make -j"${JOBS:-4}"
        make DESTDIR="$stage" install
    )
}

require_static_mips() {
    local path=$1
    file "$path" | grep -q 'ELF 32-bit LSB executable, MIPS'
    file "$path" | grep -q 'statically linked'
    mipsel-linux-musl-strip -s "$path"
}

# libtool treats -static as "prefer static archives" and still links musl's
# dynamic loader, which this firmware does not have. Link the three checkers
# again, the same way the player binary is linked.
relink() {
    local output=$1
    shift
    mipsel-linux-musl-gcc -static -no-pie -Os -o "$output" "$@"
}

fetch dosfstools-4.2 \
    https://github.com/dosfstools/dosfstools/releases/download/v4.2/dosfstools-4.2.tar.gz \
    64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527
build_autoconf dosfstools-4.2 --enable-compat-symlinks

fetch exfatprogs-1.2.6 \
    https://github.com/exfatprogs/exfatprogs/releases/download/1.2.6/exfatprogs-1.2.6.tar.gz \
    53b74148a1f8c4b67a581497056b32bf3c357e061dc683f6d5bbfa09e44e3e7a
build_autoconf exfatprogs-1.2.6

fetch ntfs-3g_ntfsprogs-2022.10.3 \
    https://download.tuxera.com/opensource/ntfs-3g_ntfsprogs-2022.10.3.tgz \
    f20e36ee68074b845e3629e6bced4706ad053804cbaf062fbae60738f854170c
build_autoconf ntfs-3g_ntfsprogs-2022.10.3 \
    --disable-ntfs-3g --enable-ntfsprogs --disable-mount-helper --without-uuid

dos_src="$work/build/dosfstools-4.2/src"
ex_src="$work/build/exfatprogs-1.2.6"
nt_src="$work/build/ntfs-3g_ntfsprogs-2022.10.3"
relink "$overlay_bin/fsck.fat" \
    "$dos_src/check.o" "$dos_src/file.o" "$dos_src/fsck.fat.o" "$dos_src/lfn.o" \
    "$dos_src/boot.o" "$dos_src/common.o" "$dos_src/fat.o" "$dos_src/io.o" "$dos_src/charconv.o"
relink "$overlay_bin/fsck.exfat" \
    "$ex_src/fsck/fsck.o" "$ex_src/fsck/repair.o" "$ex_src/lib/libexfat.a"
relink "$overlay_bin/ntfsfix" \
    "$nt_src/ntfsprogs/ntfsfix.o" "$nt_src/ntfsprogs/utils.o" \
    "$nt_src/libntfs-3g/.libs/libntfs-3g.a"
ln -sfn fsck.fat "$overlay_bin/fsck.vfat"
require_static_mips "$overlay_bin/fsck.fat"
require_static_mips "$overlay_bin/fsck.exfat"
require_static_mips "$overlay_bin/ntfsfix"

cp -a "$work/src/dosfstools-4.2/COPYING" "$license_dir/dosfstools-COPYING"
cp -a "$work/src/exfatprogs-1.2.6/COPYING" "$license_dir/exfatprogs-COPYING"
cp -a "$work/src/ntfs-3g_ntfsprogs-2022.10.3/COPYING" "$license_dir/ntfs-3g-COPYING"
cp -a "$work/src/ntfs-3g_ntfsprogs-2022.10.3/COPYING.LIB" "$license_dir/ntfs-3g-COPYING.LIB"

notice="$license_dir/NOTICE"
cat > "$notice" << EOF
SD filesystem repair tools

These static checkers are built by scripts/build_sd_fsck_tools.sh and copied
into both firmware images by scripts/repack_upt.sh. The player runs one of
them only after a card has mounted read-only.

  dosfstools 4.2 (fsck.fat, also installed as fsck.vfat)
    https://github.com/dosfstools/dosfstools/releases/download/v4.2/dosfstools-4.2.tar.gz
    SHA-256: 64926eebf90092dca21b14259a5301b7b98e7b1943e8a201c7d726084809b527
    License: GPL-3.0-or-later; see dosfstools-COPYING.

  exfatprogs 1.2.6 (fsck.exfat)
    https://github.com/exfatprogs/exfatprogs/releases/download/1.2.6/exfatprogs-1.2.6.tar.gz
    SHA-256: 53b74148a1f8c4b67a581497056b32bf3c357e061dc683f6d5bbfa09e44e3e7a
    License: GPL-2.0-only; see exfatprogs-COPYING.

  ntfs-3g 2022.10.3 (ntfsfix only; the FUSE mount helper is not installed)
    https://download.tuxera.com/opensource/ntfs-3g_ntfsprogs-2022.10.3.tgz
    SHA-256: f20e36ee68074b845e3629e6bced4706ad053804cbaf062fbae60738f854170c
    License: GPL-2.0-only; see ntfs-3g-COPYING and ntfs-3g-COPYING.LIB.

The committed ELF files are MIPS32, little-endian, o32, statically linked
against musl and stripped. ntfsfix repairs common inconsistencies that make
ntfs-3g mount a volume read-only. It is not a full Windows chkdsk.

Committed runtime hashes:
EOF
(
    cd "$overlay_bin"
    sha256sum fsck.fat fsck.exfat ntfsfix
) | sed 's/^/  /' >> "$notice"

file "$overlay_bin/fsck.fat" "$overlay_bin/fsck.exfat" "$overlay_bin/ntfsfix"
echo "Installed SD repair tools into $overlay_bin"
