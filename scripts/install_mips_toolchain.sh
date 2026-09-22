#!/usr/bin/env bash
set -euo pipefail

readonly archive="${GITHUB_WORKSPACE:-$(pwd)}/toolchains/mipsel-linux-musl-cross.tar.xz"
readonly expected_sha256="02e6ed140f2df1a937bb3e5bd94dda96de3c9576ce363da34f989abf0aedfc94"
readonly mirror_url="https://github.com/musl-cc/musl.cc/releases/download/v0.0.1/mipsel-linux-musl-cross.tgz"
readonly fallback_url="https://musl.cc/mipsel-linux-musl-cross.tgz"
readonly upstream_sha256="82626533bf7e677c225e7cbedf1d5b0d6bc60c3daaf28249e54f0eb805d89b13"
readonly destination_root="${1:?usage: $0 DESTINATION_ROOT}"
readonly toolchain_dir="$destination_root/mipsel-linux-musl-cross"

mkdir -p "$destination_root"
if test -f "$archive"; then
    echo "$expected_sha256  $archive" | sha256sum --check --status || {
        echo "Repository toolchain archive failed its pinned SHA-256 check: $archive" >&2
        exit 1
    }
    tar -xJf "$archive" -C "$destination_root"
else
    download_dir=$(mktemp -d "${TMPDIR:-/tmp}/hiby-toolchain.XXXXXX")
    trap 'rm -rf "$download_dir"' EXIT
    upstream_archive="$download_dir/mipsel-linux-musl-cross.tgz"
    echo "Repository archive unavailable; downloading verified toolchain from the GitHub release mirror" >&2
    if ! curl --fail --location --retry 2 --retry-max-time 75 \
        --connect-timeout 15 --max-time 75 \
        --output "$upstream_archive.part" "$mirror_url"; then
        echo "GitHub release mirror unavailable; trying musl.cc with a bounded timeout" >&2
        curl --fail --location --retry 1 --retry-max-time 40 \
            --connect-timeout 15 --max-time 40 \
            --output "$upstream_archive.part" "$fallback_url"
    fi
    mv "$upstream_archive.part" "$upstream_archive"
    echo "$upstream_sha256  $upstream_archive" | sha256sum --check --status || {
        echo "Downloaded toolchain failed its pinned upstream SHA-256 check" >&2
        exit 1
    }
    tar -xzf "$upstream_archive" -C "$destination_root"
fi
test -x "$toolchain_dir/bin/mipsel-linux-musl-gcc" || {
    echo "Toolchain compiler was not extracted correctly." >&2
    exit 1
}

echo "$toolchain_dir"
