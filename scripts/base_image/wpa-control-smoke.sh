#!/bin/sh
# Run on the device after copying candidate tools/libs and wpa-test.conf into
# a fresh test directory. Uses the none driver on loopback and an empty config.
# The existing lo interface satisfies the generic packet socket's ifindex
# lookup; no radio interface is opened or configured.
set -eu
test_dir=$1
case "$test_dir" in /tmp/hiby-base-test.*) ;; *) echo 'Expected a dedicated /tmp test directory' >&2; exit 2 ;; esac
# /tmp is RAM-backed on the R1. Avoid starving adbd/player while loading a
# second supplicant; remove completed test payloads before running this.
available_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
[ "${available_kb:-0}" -ge 8192 ] || { echo 'Insufficient available RAM for isolated test' >&2; exit 1; }
export LD_LIBRARY_PATH="$test_dir" LD_BIND_NOW=1
"$test_dir/wpa_supplicant" -v
"$test_dir/wpa_cli" -v
cleanup() {
    "$test_dir/wpa_cli" -p "$test_dir/wpa-ctrl" -i lo terminate >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM
"$test_dir/wpa_supplicant" -B -D none -i lo \
    -c "$test_dir/wpa-test.conf" -O "$test_dir/wpa-ctrl" \
    -P "$test_dir/wpa-test.pid" -f "$test_dir/wpa-test.log"
ready=false
for attempt in 1 2 3 4 5; do
    if [ "$("$test_dir/wpa_cli" -p "$test_dir/wpa-ctrl" -i lo ping 2>/dev/null)" = PONG ]; then
        ready=true
        break
    fi
    sleep 1
done
[ "$ready" = true ] || { echo 'Control socket did not become ready' >&2; exit 1; }
echo 'Isolated supplicant control socket: PONG'
eap=$("$test_dir/wpa_cli" -p "$test_dir/wpa-ctrl" -i lo get_capability eap)
for method in TLS PEAP TTLS FAST; do
    echo " $eap " | grep " $method " >/dev/null || { echo "Missing EAP method: $method" >&2; exit 1; }
done
echo "EAP methods: $eap"
echo 'Wi-Fi control test PASS; wlan0 and saved networks were not touched.'
