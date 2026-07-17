/* protocol.h -- CRTPi Lane 1 wire protocol v2 ("CRT1")
 *
 * Author: Ben Templeman (alphanu1)
 * Date:   2026-07-17
 * Shared verbatim between the device daemon and the host library.
 * All wire integers little-endian; structs are packed. See docs/PROTOCOL.md
 */
#ifndef CRTPI_PROTOCOL_H
#define CRTPI_PROTOCOL_H

#include <stdint.h>

#define CRT1_MAGIC      0x31545243u /* "CRT1" */

/* commands: host -> device */
#define CMD_GET_INFO    0x01
#define CMD_SET_MODE    0x10
#define CMD_FRAME       0x20
#define CMD_VSYNC_SUB   0x30
#define CMD_SET_PROFILE 0x40
#define CMD_SET_LANE    0x41
#define CMD_SET_ENFORCE 0x42

/* events: device -> host */
#define EVT_INFO        0x81
#define EVT_MODE_RESULT 0x90
#define EVT_STATUS      0xA0
#define EVT_VSYNC       0xB0

/* status codes */
#define ST_OK       0
#define ST_EBADCMD  1
#define ST_ERANGE   2
#define ST_ECLAMPED 3
#define ST_EKMS     4
#define ST_EBUSY    5

/* pixel formats */
#define PIXFMT_RGB565   1
#define PIXFMT_XRGB8888 2

/* mode_flags bits */
#define MF_INTERLACE  (1u << 0)
#define MF_DOUBLESCAN (1u << 1)
#define MF_HSYNC_POS  (1u << 2)   /* absent = negative sync (CRT default) */
#define MF_VSYNC_POS  (1u << 3)

#define FLG_WANT_REPLY (1u << 0)

#pragma pack(push, 1)

struct crt1_hdr {
    uint32_t magic;
    uint8_t  cmd;
    uint8_t  flags;
    uint16_t seq;
    uint32_t len;
};

/* CMD_SET_MODE payload. Field semantics are switchres modeline semantics:
 * hbegin/hend are sync START/END POSITIONS, not porch widths. The device
 * converts to whatever the sink needs (see the position-vs-width bug class
 * documented in docs/CODE_REVIEW_switchres_VC4.md, item 8). */
struct wire_modeline {
    uint32_t pclock_hz;
    uint16_t hactive, hbegin, hend, htotal;
    uint16_t vactive, vbegin, vend, vtotal;
    uint16_t mode_flags;
    uint8_t  pixfmt;
    uint8_t  _pad;
    uint64_t host_tag;
};

/* CMD_FRAME payload prefix */
struct frame_hdr {
    uint32_t x, y, w, h;
};

/* EVT_MODE_RESULT payload */
struct mode_result {
    uint16_t status;
    uint16_t _pad;
    uint32_t achieved_pclock_hz;   /* KMS/PLL readback (0 if unavailable)  */
    uint32_t achieved_hfreq_mhz_x1000; /* hfreq in Hz (name kept simple)   */
    uint32_t achieved_vfreq_uhz;   /* measured vfreq in microhertz         */
    uint64_t host_tag;
    struct wire_modeline applied;  /* the mode actually committed          */
};

/* EVT_VSYNC payload */
struct vsync_evt {
    uint64_t timestamp_ns;         /* CLOCK_MONOTONIC                      */
    uint32_t frame_count;
    uint32_t _pad;
};

/* EVT_STATUS payload */
struct status_evt {
    uint16_t seq_echo;
    uint16_t status;
};

#pragma pack(pop)

#endif /* CRTPI_PROTOCOL_H */
