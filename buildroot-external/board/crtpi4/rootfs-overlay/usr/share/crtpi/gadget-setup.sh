#!/bin/sh
# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
# gadget-setup.sh -- CRTPi composite USB gadget via configfs
#
# Creates: vendor FunctionFS interface (Lane 1 + control), mounted at
# /dev/crtpi_ffs. Lane 2's GUD function is added at milestone M3 (requires
# the out-of-tree f_gud_drm gadget function or a userspace GUD-over-ffs
# implementation -- see README "Lane 2 plan").
#
# Requires kernel: dwc2 in peripheral mode, libcomposite, usb_f_fs.
# Run from init before crtd.

set -e

G=/sys/kernel/config/usb_gadget/crtpi
VID=0x1209        # pid.codes community VID -- register a real PID before release
PID=0x0001

modprobe libcomposite 2>/dev/null || true

mount -t configfs none /sys/kernel/config 2>/dev/null || true
mount -t debugfs  none /sys/kernel/debug  2>/dev/null || true  # clock readback

if [ -d "$G" ]; then
    echo "gadget already exists" >&2
    exit 0
fi

mkdir -p "$G"
echo $VID > "$G/idVendor"
echo $PID > "$G/idProduct"
echo 0x0200 > "$G/bcdUSB"

mkdir -p "$G/strings/0x409"
echo "CRTPi"                 > "$G/strings/0x409/manufacturer"
echo "CRTPi 15kHz Display"   > "$G/strings/0x409/product"
cat /proc/device-tree/serial-number 2>/dev/null | tr -d '\0' \
    > "$G/strings/0x409/serialnumber" || echo 0000 > "$G/strings/0x409/serialnumber"

mkdir -p "$G/configs/c.1/strings/0x409"
echo "Lane1+Control" > "$G/configs/c.1/strings/0x409/configuration"
echo 250 > "$G/configs/c.1/MaxPower"

# --- Lane 1: vendor FunctionFS -------------------------------------------
mkdir -p "$G/functions/ffs.crtpi"
ln -s "$G/functions/ffs.crtpi" "$G/configs/c.1/"

mkdir -p /dev/crtpi_ffs
mount -t functionfs crtpi /dev/crtpi_ffs

# NOTE: binding to the UDC must happen AFTER crtd has written descriptors to
# ep0, or enumeration fails. crtd is started by init next; then:
#   echo "$(ls /sys/class/udc | head -n1)" > $G/UDC
# The init script does exactly that (see board/crtpi4/S90crtpi).

echo "gadget staged; start crtd, then bind UDC"
