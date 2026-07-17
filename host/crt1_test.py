#!/usr/bin/env python3
# crt1_test.py -- CRTPi Lane 1 smoke-test client
#
# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
#
# Minimal host-side exerciser for the CRT1 protocol: sets a modeline,
# prints the achieved-timing reply, pushes one color-bar frame. This is
# the M2 bring-up tool; the real host library (libcrt1, C, libusb/WinUSB)
# supersedes it for applications.
#
# Requires: pip install pyusb   (and the udev rule from 99-crtpi.rules,
# or run as root). Linux/libusb; on Windows install WinUSB via Zadig
# until crtd ships MS OS 2.0 descriptors (M2).
#
# Usage:
#   ./crt1_test.py 6400000 320 328 359 407 240 244 247 262 [i]
#
import struct
import sys
import time

import usb.core
import usb.util

VID, PID = 0x1209, 0x0001
EP_OUT, EP_IN = 0x01, 0x82
MAGIC = 0x31545243  # "CRT1"

CMD_SET_MODE, CMD_FRAME = 0x10, 0x20
EVT_MODE_RESULT = 0x90
MF_INTERLACE = 1 << 0
PIXFMT_RGB565 = 1

HDR = struct.Struct("<IBBHI")                # magic, cmd, flags, seq, len
MODELINE = struct.Struct("<I9HBxQ")          # pclk, 8 timings+flags, fmt, tag
MODE_RESULT = struct.Struct("<HHIIIQ32s")    # status..applied


def pkt(cmd, payload=b"", seq=0, flags=0):
    return HDR.pack(MAGIC, cmd, flags, seq, len(payload)) + payload


def read_evt(dev, want):
    deadline = time.time() + 3.0
    while time.time() < deadline:
        data = bytes(dev.read(EP_IN, 4096, timeout=1000))
        if len(data) >= HDR.size:
            magic, cmd, _f, _s, ln = HDR.unpack_from(data)
            if magic == MAGIC and cmd == want:
                return data[HDR.size:HDR.size + ln]
    raise TimeoutError("no event 0x%02x" % want)


def main():
    if len(sys.argv) < 10:
        sys.exit("usage: crt1_test.py pclk hact hbeg hend htot "
                 "vact vbeg vend vtot [i]")
    v = [int(x) for x in sys.argv[1:10]]
    flags = MF_INTERLACE if (len(sys.argv) > 10 and
                             sys.argv[10].startswith("i")) else 0

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("CRTPi not found (is it plugged in / in native lane?)")
    # detach kernel driver if anything grabbed the interface
    try:
        if dev.is_kernel_driver_active(0):
            dev.detach_kernel_driver(0)
    except (usb.core.USBError, NotImplementedError):
        pass
    dev.set_configuration()

    m = MODELINE.pack(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8],
                      flags, PIXFMT_RGB565, 0xB0B0)
    dev.write(EP_OUT, pkt(CMD_SET_MODE, m, seq=1))

    res = read_evt(dev, EVT_MODE_RESULT)
    st, _p, pclk, hfreq, vf_uhz, tag, _applied = MODE_RESULT.unpack(res)
    print("status=%d  achieved pclk=%d Hz  hfreq=%d Hz  vfreq=%.6f Hz"
          % (st, pclk, hfreq, vf_uhz / 1e6))
    if st != 0:
        sys.exit("mode rejected/failed")

    # one full frame of RGB565 color bars
    w, h = v[1], v[5]
    bars = [0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000]
    row = b"".join(struct.pack("<H", bars[x * 8 // w]) for x in range(w))
    frame = struct.pack("<IIII", 0, 0, w, h) + row * h
    dev.write(EP_OUT, pkt(CMD_FRAME, frame, seq=2))
    print("frame sent (%dx%d) -- bars should be on the CRT" % (w, h))


if __name__ == "__main__":
    main()
