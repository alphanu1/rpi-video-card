# CRTPi Lane 1 wire protocol, v2 ("CRT1")

Transport: USB bulk, vendor-specific interface (WinUSB on Windows, libusb
everywhere). Two endpoints: BULK OUT (host->device commands/frames), BULK IN
(device->host events/replies). All integers little-endian.

## Packet header (12 bytes, both directions)

    offset  size  field
    0       4     magic     = 0x31545243  ("CRT1")
    4       1     cmd
    5       1     flags     (bit0: reply-requested; others reserved 0)
    6       2     seq       (echoed in replies/acks)
    8       4     len       (payload bytes following the header)

Unknown cmd -> device replies EVT_STATUS with ST_EBADCMD and resyncs by
magic hunt. Torn transfer -> 250ms silence watchdog resyncs (same philosophy
as the FPGA design: host recovery = resend).

## Commands (host -> device, BULK OUT)

CMD_GET_INFO (0x01), len=0
  Reply EVT_INFO: proto version, device name, active lane, active profile
  string, current mode (achieved timings), max frame dimensions, supported
  pixel formats bitmap.

CMD_SET_MODE (0x10), len=32 — the heart of Lane 1
  struct wire_modeline {
      u32 pclock_hz;     //  0 pixel clock in Hz (exact, from host switchres)
      u16 hactive;       //  4
      u16 hbegin;        //  6 hsync start (position, switchres semantics)
      u16 hend;          //  8 hsync end   (position)
      u16 htotal;        // 10
      u16 vactive;       // 12
      u16 vbegin;        // 14
      u16 vend;          // 16
      u16 vtotal;        // 18
      u16 mode_flags;    // 20 bit0 interlace, bit1 doublescan,
                         //    bit2 hsync positive, bit3 vsync positive
      u8  pixfmt;        // 22 PIXFMT_RGB565=1, PIXFMT_XRGB8888=2
      u8  _pad;          // 23
      u64 host_tag;      // 24 opaque, echoed in reply
  };
  Device validates against the monitor profile clamp (unless enforcement is
  off), applies via KMS, replies EVT_MODE_RESULT carrying the *achieved*
  timings: readback pixel clock, computed hfreq/vfreq (measured over vblank
  after settle), and a status code (ST_OK / ST_ECLAMPED / ST_ERANGE /
  ST_EKMS).

CMD_FRAME (0x20), len = 16 + pixel data
  struct frame_hdr { u32 x, y, w, h; } then w*h pixels in the active pixfmt,
  row-major. Full frame = (0,0,hactive,vactive); partial rects allowed
  (damage updates). Device blits into the scanout dumb buffer. No reply
  unless flags bit0 set (then EVT_STATUS acks with seq — useful for pacing
  without vsync subscription).

CMD_VSYNC_SUB (0x30), len=1 (0=off, 1=on)
  Enables EVT_VSYNC on BULK IN each vertical blank: u64 monotonic timestamp
  (ns), u32 frame counter. This is how GroovyMAME-style hosts phase-lock.

CMD_SET_PROFILE (0x40), len = string
  crt_range string or preset name ("arcade_15", ...). Persisted. Reply
  EVT_STATUS. In Lane 2 this also triggers modelist regen + re-enumeration.

CMD_SET_LANE (0x41), len=1 (1=native, 2=gud)
  Switches display ownership. Persisted default is boot-config; this is the
  runtime override. Reply EVT_STATUS before the switch takes effect.

CMD_SET_ENFORCE (0x42), len=1 (0=off, 1=on)  — profile clamp toggle.

## Events (device -> host, BULK IN)

EVT_INFO (0x81), EVT_MODE_RESULT (0x90), EVT_VSYNC (0xB0),
EVT_STATUS (0xA0: u16 seq-echo, u16 status code).

Status codes: ST_OK=0, ST_EBADCMD=1, ST_ERANGE=2 (mode outside profile,
rejected), ST_ECLAMPED=3 (adjusted to nearest legal, details in payload),
ST_EKMS=4 (kernel rejected commit), ST_EBUSY=5.

## Bandwidth sanity

320x240 RGB565 @ 60fps = 9.2 MB/s; worst realistic case 768x576 XRGB @ 50
= 88 MB/s > USB2 — hence RGB565 default, damage rects, and (later protocol
rev) optional LZ4 per-packet compression flag, mirroring GUD's approach.
