#!/usr/bin/env bash
# Exercise the player's raw-PCM pipe invocation against an ALSA null device.
set -euo pipefail
[[ $# == 1 && -d "$1/usr/lib" ]] || { echo "Usage: $0 ROOTFS" >&2; exit 2; }
root=$(cd -- "$1" && pwd)
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
runner=(qemu-mipsel -L "$root" -E LD_BIND_NOW=1 \
    -E "LD_LIBRARY_PATH=$root/usr/lib:$root/lib" \
    -E "ALSA_CONFIG_PATH=$repo/scripts/base_image/alsa-null.conf")
"${runner[@]}" "$root/usr/bin/aplay" --version
"${runner[@]}" "$root/usr/bin/arecord" --version
"${runner[@]}" "$root/usr/bin/amixer" --help >/dev/null
for format in S16_LE S24_LE; do
    for rate in 44100 48000 192000; do
        head -c 65536 /dev/zero | timeout 10 "${runner[@]}" "$root/usr/bin/aplay" \
            -q -D null -t raw -f "$format" -r "$rate" -c 2
        printf 'aplay pipe to null: %s, %s Hz PASS\n' "$format" "$rate"
    done
done
timeout 10 "${runner[@]}" "$root/usr/bin/arecord" -q -D null -t raw \
    -f S16_LE -r 48000 -c 2 -d 1 /dev/null
echo 'arecord null capture PASS; no real audio hardware exercised.'
