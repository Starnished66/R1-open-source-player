#!/usr/bin/env bash
set -euo pipefail

readonly archive="${GITHUB_WORKSPACE:-$(pwd)}/toolchains/mipsel-linux-musl-cross.tar.xz"
readonly expected_sha256="02e6ed140f2df1a937bb3e5bd94dda96de3c9576ce363da34f989abf0aedfc94"
readonly destination_root="${1:?usage: $0 DESTINATION_ROOT}"
readonly toolchain_dir="$destination_root/mipsel-linux-musl-cross"

test -f "$archive" || {
    echo "Repository toolchain archive is missing: $archive" >&2
    exit 1
}

echo "$expected_sha256  $archive" | sha256sum --check --status
mkdir -p "$destination_root"
tar -xJf "$archive" -C "$destination_root"
test -x "$toolchain_dir/bin/mipsel-linux-musl-gcc" || {
    echo "Toolchain compiler was not extracted correctly." >&2
    exit 1
}

echo "$toolchain_dir"
