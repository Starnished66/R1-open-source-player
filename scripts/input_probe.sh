#!/bin/sh
# Prints every input event from every /dev/input/event* device while you press
# buttons or turn a wheel, so unknown controls (e.g. a HiBy scroll wheel) can
# be identified. Busybox-only: needs sh, dd, od; no evtest/getevent.
#
# Usage (on the device, e.g. over adb shell):
#   sh input_probe.sh            # all devices
#   sh input_probe.sh event2     # one device
#   SHOW_SYN=1 sh input_probe.sh # also print EV_SYN report separators
# Ctrl+C to stop.
#
# If a running player grabs the device exclusively (EVIOCGRAB), nothing will
# show for that device: stop the player first.

# struct input_event is 16 bytes for 32-bit userspace, 24 for 64-bit. The last
# 8 bytes are always type(u16) code(u16) value(s32).
elf_class=$(dd if=/bin/sh bs=1 skip=4 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
if [ "$elf_class" = "2" ]; then EV_SIZE=24; else EV_SIZE=16; fi

type_name() {
    case $1 in
        0) echo EV_SYN ;; 1) echo EV_KEY ;; 2) echo EV_REL ;; 3) echo EV_ABS ;;
        4) echo EV_MSC ;; 5) echo EV_SW ;; 17) echo EV_LED ;; 20) echo EV_REP ;;
        *) echo "EV_$1" ;;
    esac
}

code_name() { # type code
    case $1 in
        1) case $2 in
            1) echo KEY_ESC ;; 28) echo KEY_ENTER ;; 102) echo KEY_HOME ;;
            103) echo KEY_UP ;; 105) echo KEY_LEFT ;; 106) echo KEY_RIGHT ;; 108) echo KEY_DOWN ;;
            113) echo KEY_MUTE ;; 114) echo KEY_VOLUMEDOWN ;; 115) echo KEY_VOLUMEUP ;;
            116) echo KEY_POWER ;; 139) echo KEY_MENU ;; 158) echo KEY_BACK ;;
            163) echo KEY_NEXTSONG ;; 164) echo KEY_PLAYPAUSE ;; 165) echo KEY_PREVIOUSSONG ;;
            166) echo KEY_STOPCD ;; 168) echo KEY_REWIND ;; 207) echo KEY_PLAY ;;
            208) echo KEY_FASTFORWARD ;; 177) echo KEY_SCROLLUP ;; 178) echo KEY_SCROLLDOWN ;;
            352) echo KEY_OK ;; 353) echo KEY_SELECT ;;
            272) echo BTN_LEFT ;; 273) echo BTN_RIGHT ;; 274) echo BTN_MIDDLE ;;
            330) echo BTN_TOUCH ;;
            *) echo "KEY_$2" ;;
           esac ;;
        2) case $2 in
            0) echo REL_X ;; 1) echo REL_Y ;; 6) echo REL_HWHEEL ;; 7) echo REL_DIAL ;;
            8) echo REL_WHEEL ;; 11) echo REL_WHEEL_HI_RES ;; 12) echo REL_HWHEEL_HI_RES ;;
            *) echo "REL_$2" ;;
           esac ;;
        3) case $2 in
            0) echo ABS_X ;; 1) echo ABS_Y ;; 7) echo ABS_WHEEL ;; 8) echo ABS_WHEEL ;;
            47) echo ABS_MT_SLOT ;; 53) echo ABS_MT_POSITION_X ;; 54) echo ABS_MT_POSITION_Y ;;
            57) echo ABS_MT_TRACKING_ID ;;
            *) echo "ABS_$2" ;;
           esac ;;
        4) case $2 in 4) echo MSC_SCAN ;; *) echo "MSC_$2" ;; esac ;;
        *) echo "$2" ;;
    esac
}

value_note() { # type value
    if [ "$1" = "1" ]; then
        case $2 in 0) echo "(release)" ;; 1) echo "(press)" ;; 2) echo "(repeat)" ;; esac
    elif [ "$1" = "2" ]; then
        if [ "$2" -gt 0 ]; then echo "(+ clockwise/down?)"; elif [ "$2" -lt 0 ]; then echo "(- counter-clockwise/up?)"; fi
    fi
}

watch_device() { # /dev/input/eventN
    dev=$1
    base=${dev##*/}
    name=$(cat /sys/class/input/$base/device/name 2>/dev/null)
    exec 3<"$dev" || { echo "[$base] cannot open $dev"; return; }
    while :; do
        rec=$(dd bs=$EV_SIZE count=1 <&3 2>/dev/null | od -An -v -tu2)
        [ -z "$rec" ] && { echo "[$base] read ended"; return; }
        set -- $rec
        n=$#
        shift $((n - 4))
        t=$1; c=$2
        # Signed 32-bit value from two little-endian u16 halves, without
        # exceeding 32-bit shell arithmetic.
        hi=$4; [ "$hi" -ge 32768 ] && hi=$((hi - 65536))
        v=$(($3 + hi * 65536))
        [ "$t" = "0" ] && [ -z "$SHOW_SYN" ] && continue
        printf '[%s %s] %-7s %-20s %6d %s\n' "$base" "$name" "$(type_name "$t")" \
            "$(code_name "$t" "$c")" "$v" "$(value_note "$t" "$v")"
    done
}

echo "input_event size: $EV_SIZE bytes"
echo "=== Input devices (B: KEY/REL/ABS bitmaps show what each can report) ==="
cat /proc/bus/input/devices 2>/dev/null
echo "=== Listening; press buttons / turn the wheel. Ctrl+C to stop ==="

pids=""
trap 'kill $pids 2>/dev/null; exit 0' INT TERM
if [ -n "$1" ]; then
    set -- "/dev/input/${1#/dev/input/}"
else
    set -- /dev/input/event*
fi
for dev in "$@"; do
    [ -c "$dev" ] || continue
    watch_device "$dev" &
    pids="$pids $!"
done
wait
