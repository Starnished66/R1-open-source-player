#!/bin/sh

VID="0x32BB"
PID="0x0004"
MANUFACTURER="Hiby"
PRODUCT="R1"

UDC=`ls /sys/class/udc/`

#获取序列号
if [ -f /proc/jz/efuse/efuse_chip_id ]; then
	SERIAL=$(cat /proc/jz/efuse/efuse_chip_id | awk '{print substr($2, 1, 6)}')
fi
if [ -z "$SERIAL" ]; then
	SERIAL="123456" 
fi

# 从命令行参数读取VID、PID、MANUFACTURER、PRODUCT
while getopts ":v:p:m:n:" opt; do
  case $opt in
    v) VID=$OPTARG ;;
    p) PID=$OPTARG ;;
    m) MANUFACTURER=$OPTARG ;;
    n) PRODUCT=$OPTARG ;;
    ?) exit 1;;
  esac
done

shift $((OPTIND-1))

prg_name=$0
status=$1

# 配置uac设备
uac_start() {
	echo "Creating the USB gadget..."
	if [ ! -d /sys/kernel/config/usb_gadget ]; then
		mount -t configfs none /sys/kernel/config
	fi

	mkdir /sys/kernel/config/usb_gadget/android0
	cd /sys/kernel/config/usb_gadget/android0

	# 配置设备描述符
	echo "Setting Device Descriptor..."
	echo "239" > bDeviceClass
	echo "2" > bDeviceSubClass
	echo "1" > bDeviceProtocol
	echo "0x200" > bcdUSB
	echo "0x100" > bcdDevice
	echo $VID > idVendor
	echo $PID > idProduct

	# 配置字符串描述符
	echo "Setting English strings..."
	mkdir strings/0x409
	echo $MANUFACTURER > strings/0x409/manufacturer
	echo $PRODUCT > strings/0x409/product
	echo $SERIAL > strings/0x409/serialnumber


	# 配置配置描述符
	echo "Creating Config..."
	mkdir configs/c.1
	echo "10" > configs/c.1/MaxPower
	echo "0xC0" > configs/c.1/bmAttributes
	mkdir  configs/c.1/strings/0x409
	echo "uac_sa" > configs/c.1/strings/0x409/configuration

	# 配置功能描述符
	echo "Creating functions..."
	mkdir functions/uac_sa.a
	echo "0x03" > functions/uac_sa.a/c_chmask
	echo "2" > functions/uac_sa.a/c_ssize
	echo "48000" > functions/uac_sa.a/c_srate
	# macOS CoreAudio compatibility: this gadget's descriptor set (confirmed
	# by reading the decompressed kernel directly) is host-to-device only --
	# there is no capture/mic interface here to disable, so p_chmask/p_ssize/
	# p_srate below are harmless no-ops kept at their stock values. The real
	# fix is dynamic_feedback: the descriptor already declares an explicit
	# feedback IN endpoint alongside the async isochronous OUT endpoint, but
	# it defaults to inactive (this vendor attribute is never touched by the
	# stock script). Async host-to-device audio with an unused feedback
	# endpoint is a well-documented cause of a gadget working on Linux
	# (whose usb-audio driver is unusually lenient) while failing on macOS,
	# which requires working feedback for this case.
	echo "0x00" > functions/uac_sa.a/p_chmask
	echo "2" > functions/uac_sa.a/p_ssize
	echo "48000" > functions/uac_sa.a/p_srate
	echo "1" 2>/dev/null > functions/uac_sa.a/dynamic_feedback || true

	ln -s  functions/uac_sa.a configs/c.1

	echo $UDC > UDC
}


# 卸载uac设备
uac_stop() {

	echo "0" > /sys/class/usb_gadget/android0/soft_disconnect

	echo "stopping the USB gadget"

	cd /sys/kernel/config/usb_gadget/android0

	echo "Unbinding USB Device controller..."
	echo "" > UDC

	echo "Deleting uac gadget functionality : uac.0"
	rm configs/c.1/uac_sa.a
	rmdir functions/uac_sa.a

	echo "Cleaning up configuration..."
	rmdir configs/c.1/strings/0x409
	rmdir configs/c.1

	echo "cleaning English string..."
	rmdir strings/0x409

	echo "Removing gadget directory..."
	cd -
	rmdir /sys/kernel/config/usb_gadget/android0/

	umount /sys/kernel/config
}


case "$status" in
	start)

	# Leftover unbound android0 after Storage used to make this exit 1
	# before creating the UAC function. Only refuse if UDC is bound.
	if [ -d /sys/kernel/config/usb_gadget/android0 ]; then
		udc=$(cat /sys/kernel/config/usb_gadget/android0/UDC 2>/dev/null | tr -d '\r\n ')
		if [ -n "$udc" ] && [ "$udc" != "none" ]; then
			echo "Error: usb configfs already mounted"
			exit 1
		fi
		echo "" > /sys/kernel/config/usb_gadget/android0/UDC 2>/dev/null
		for f in /sys/kernel/config/usb_gadget/android0/functions/*; do
			[ -e "$f" ] || continue
			name=$(basename "$f")
			rm -f /sys/kernel/config/usb_gadget/android0/configs/c.1/$name
			if [ -e "$f/lun.0/file" ]; then
				echo "" > "$f/lun.0/file"
			fi
			rmdir "$f" 2>/dev/null
		done
		rmdir /sys/kernel/config/usb_gadget/android0/configs/c.1/strings/0x409 2>/dev/null
		rmdir /sys/kernel/config/usb_gadget/android0/configs/c.1 2>/dev/null
		rmdir /sys/kernel/config/usb_gadget/android0/strings/0x409 2>/dev/null
		rmdir /sys/kernel/config/usb_gadget/android0 2>/dev/null
		if [ -d /sys/kernel/config/usb_gadget/android0 ]; then
			echo "Error: leftover android0 could not be removed"
			exit 1
		fi
	fi

	# 配置uac设备函数
	uac_start

	;;
	stop)
	if [ "$#" != "1" ]; then
		echo "Usage: $prg_name stop"
		exit 1
	fi

	if [ ! -d /sys/kernel/config/usb_gadget/android0 ]; then
		echo "Error: usb configfs android0 uninitialized"
		exit 1
	fi

	# 卸载uac设备函数
	uac_stop

	;;
	*)

	echo "Usage: $prg_name {start|stop}"
	exit 1
esac