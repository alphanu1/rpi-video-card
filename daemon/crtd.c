/* crtd.c -- CRTPi device daemon
 *
 * Author: Ben Templeman (alphanu1)
 * Date:   2026-07-17
 *
 * Lane 1 (native): vendor bulk interface via FunctionFS (this file),
 *                  verbatim host modelines -> drm_apply.
 * Lane 2 (gud):    GUD gadget owns the display; crtd keeps only the control
 *                  interface alive (profile mgmt, lane switch, on-demand
 *                  modes). Wiring for lane 2 is milestone M3 -- see README.
 *
 * FunctionFS: scripts/gadget-setup.sh creates the composite gadget and
 * mounts the function at /dev/crtpi_ffs; we write descriptors+strings to
 * ep0, then service ep1 (BULK OUT, host->device) and ep2 (BULK IN).
 *
 * Deliberately single-threaded around a poll() loop except the frame path,
 * which is just reads+memcpy -- at USB2 rates this is nowhere near a
 * bottleneck on the Pi 4.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <poll.h>
#include <linux/usb/functionfs.h>

/* glibc's htole*() aren't constant expressions; the gadget and all supported
 * hosts are little-endian, so use identity with a hard guard. */
#if __BYTE_ORDER != __LITTLE_ENDIAN
#error "crtd assumes a little-endian build target"
#endif
#define LE16(x) (x)
#define LE32(x) (x)

#include <xf86drmMode.h>

#include "protocol.h"
#include "drm_apply.h"

#define FFS_DIR_DEFAULT "/dev/crtpi_ffs"
#define CONF_DEFAULT    "/etc/crtpi.conf"

#ifndef CRTPI_VERSION
#define CRTPI_VERSION "dev"
#endif
#define MAX_FRAME_BYTES (2048u * 1024u)   /* 1024x512 @16bpp ceiling */

/* ------------------------------------------------------------------ */
/* FunctionFS descriptors: one interface, 2 bulk endpoints             */
/* ------------------------------------------------------------------ */

struct ffs_descs {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count;
    __le32 hs_count;
    struct {
        struct usb_interface_descriptor intf;
        struct usb_endpoint_descriptor_no_audio ep_out;
        struct usb_endpoint_descriptor_no_audio ep_in;
    } __attribute__((packed)) fs, hs;
} __attribute__((packed));

static const struct ffs_descs descriptors = {
    .header = {
        .magic  = LE32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
        .flags  = LE32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
        .length = LE32(sizeof(descriptors)),
    },
    .fs_count = LE32(3),
    .hs_count = LE32(3),
    .fs = {
        .intf = {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DT_INTERFACE,
            .bNumEndpoints = 2,
            .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
            .iInterface = 1,
        },
        .ep_out = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 1 | USB_DIR_OUT,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = LE16(64),
        },
        .ep_in = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 2 | USB_DIR_IN,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = LE16(64),
        },
    },
    .hs = {
        .intf = {
            .bLength = sizeof(struct usb_interface_descriptor),
            .bDescriptorType = USB_DT_INTERFACE,
            .bNumEndpoints = 2,
            .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
            .iInterface = 1,
        },
        .ep_out = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 1 | USB_DIR_OUT,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = LE16(512),
        },
        .ep_in = {
            .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = 2 | USB_DIR_IN,
            .bmAttributes = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize = LE16(512),
        },
    },
};

static const struct {
    struct usb_functionfs_strings_head header;
    struct {
        __le16 code;
        char str[12];
    } __attribute__((packed)) lang;
} __attribute__((packed)) strings = {
    .header = {
        .magic  = LE32(FUNCTIONFS_STRINGS_MAGIC),
        .length = LE32(sizeof(strings)),
        .str_count  = LE32(1),
        .lang_count = LE32(1),
    },
    .lang = { .code = LE16(0x0409), .str = "CRTPi Lane1" },
};

/* ------------------------------------------------------------------ */
/* Device state                                                        */
/* ------------------------------------------------------------------ */

struct crtd {
    int ep0, ep_out, ep_in;
    int bound;                      /* function enabled by host */
    struct drm_out out;
    int have_mode;
    uint8_t pixfmt;
    int vsync_sub;
    uint32_t frame_count;
    /* config */
    char profile[256];
    int enforce;
    int splash;
    int lane;                       /* 1=native, 2=gud */
    double clamp_hfreq_min, clamp_hfreq_max;
    double clamp_vfreq_min, clamp_vfreq_max;
    uint8_t *pkt;                   /* payload scratch */
};

/* crt_range: "hfmin-hfmax, vfmin-vfmax, ..." -- we clamp on the first two
 * fields; full switchres validation arrives with libswitchres in M3. */
static void parse_profile_clamp(struct crtd *d)
{
    d->clamp_hfreq_min = 15000;  d->clamp_hfreq_max = 16100;
    d->clamp_vfreq_min = 47;     d->clamp_vfreq_max = 65;
    if (!strncmp(d->profile, "arcade_15", 9))
        return;                       /* defaults above */
    if (!strcmp(d->profile, "pal")) {
        d->clamp_vfreq_min = 45;  d->clamp_vfreq_max = 55;
        return;
    }
    if (!strncmp(d->profile, "crt_range:", 10)) {
        double a, b, c, e;
        if (sscanf(d->profile + 10, "%lf-%lf , %lf-%lf", &a, &b, &c, &e) == 4
         || sscanf(d->profile + 10, "%lf-%lf,%lf-%lf", &a, &b, &c, &e) == 4) {
            d->clamp_hfreq_min = a; d->clamp_hfreq_max = b;
            d->clamp_vfreq_min = c; d->clamp_vfreq_max = e;
        }
    }
}

static void load_config(struct crtd *d, const char *path)
{
    char line[512], key[64], val[256];

    strcpy(d->profile, "arcade_15");
    d->enforce = 1;
    d->lane = 1;
    d->splash = 1;

    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == '\n')
                continue;
            if (sscanf(line, " %63[^= ] = %255[^\n]", key, val) != 2)
                continue;
            if (!strcmp(key, "monitor_profile"))
                snprintf(d->profile, sizeof(d->profile), "%s", val);
            else if (!strcmp(key, "profile_enforce"))
                d->enforce = strcmp(val, "off") != 0;
            else if (!strcmp(key, "splash"))
                d->splash = strcmp(val, "off") != 0;
            else if (!strcmp(key, "lane"))
                d->lane = strcmp(val, "gud") ? 1 : 2;
        }
        fclose(f);
    }
    parse_profile_clamp(d);
    printf("crtd: lane=%s profile=%s enforce=%s "
           "clamp h=[%.0f..%.0f] v=[%.1f..%.1f]\n",
           d->lane == 2 ? "gud" : "native", d->profile,
           d->enforce ? "on" : "off",
           d->clamp_hfreq_min, d->clamp_hfreq_max,
           d->clamp_vfreq_min, d->clamp_vfreq_max);
}

/* ------------------------------------------------------------------ */
/* IN-endpoint helpers                                                 */
/* ------------------------------------------------------------------ */

static int send_evt(struct crtd *d, uint8_t evt, uint16_t seq,
                    const void *payload, uint32_t len)
{
    struct crt1_hdr h = {
        .magic = CRT1_MAGIC, .cmd = evt, .flags = 0, .seq = seq, .len = len
    };
    if (write(d->ep_in, &h, sizeof(h)) != (ssize_t)sizeof(h))
        return -1;
    if (len && write(d->ep_in, payload, len) != (ssize_t)len)
        return -1;
    return 0;
}

static void send_status(struct crtd *d, uint16_t seq, uint16_t st)
{
    struct status_evt e = { .seq_echo = seq, .status = st };
    send_evt(d, EVT_STATUS, seq, &e, sizeof(e));
}

/* ------------------------------------------------------------------ */
/* Idle splash: 320x240 SMPTE bars when powered but no host driving us. */
/* Candidates are tried against the profile clamp so a PAL-only or      */
/* custom-range profile never gets out-of-band sync from our own hands. */
/* ------------------------------------------------------------------ */

static int mode_fits_clamp(const struct crtd *d, uint32_t pclk,
                           uint16_t htot, uint16_t vtot, int interlace)
{
    double hf, vf;
    if (!d->enforce)
        return 1;
    if (!htot || !vtot)
        return 0;
    hf = (double)pclk / htot;
    vf = hf / vtot * (interlace ? 2.0 : 1.0);
    return hf >= d->clamp_hfreq_min && hf <= d->clamp_hfreq_max &&
           vf >= d->clamp_vfreq_min && vf <= d->clamp_vfreq_max;
}

static void splash_show(struct crtd *d)
{
    /* 320x240 progressive: ~60Hz NTSC-timed, then ~50Hz PAL-timed. */
    static const struct wire_modeline cand[2] = {
        { .pclock_hz = 6400000, .hactive = 320, .hbegin = 328,
          .hend = 359, .htotal = 407, .vactive = 240, .vbegin = 244,
          .vend = 247, .vtotal = 262, .mode_flags = 0,
          .pixfmt = PIXFMT_XRGB8888 },
        { .pclock_hz = 6359375, .hactive = 320, .hbegin = 328,
          .hend = 359, .htotal = 407, .vactive = 240, .vbegin = 269,
          .vend = 272, .vtotal = 312, .mode_flags = 0,
          .pixfmt = PIXFMT_XRGB8888 },
    };
    struct mode_result res;
    int i;

    if (!d->splash)
        return;
    for (i = 0; i < 2; i++) {
        if (!mode_fits_clamp(d, cand[i].pclock_hz, cand[i].htotal,
                             cand[i].vtotal, 0))
            continue;
        if (drm_out_set_mode(&d->out, &cand[i], &res) == 0) {
            char l1[64], l2[64], l3[64];
            uint32_t ty = d->out.height - 3 * 12 - 6;
            drm_out_draw_testpattern(&d->out);
            snprintf(l1, sizeof(l1), " CRTPI %.10s  320X240 %s ",
                     CRTPI_VERSION, i ? "50HZ" : "60HZ");
            snprintf(l2, sizeof(l2), " LANE %s  PROFILE %.16s ",
                     d->lane == 2 ? "GUD" : "NATIVE", d->profile);
            snprintf(l3, sizeof(l3), " H %.0f-%.0f V %.0f-%.0f  NO HOST ",
                     d->clamp_hfreq_min, d->clamp_hfreq_max,
                     d->clamp_vfreq_min, d->clamp_vfreq_max);
            drm_out_draw_text(&d->out, 8, ty, l1);
            drm_out_draw_text(&d->out, 8, ty + 12, l2);
            drm_out_draw_text(&d->out, 8, ty + 24, l3);
            printf("crtd: splash up (320x240 %s)\n", i ? "50Hz" : "60Hz");
            return;
        }
    }
    fprintf(stderr, "crtd: splash skipped (no candidate fits profile)\n");
}

/* ------------------------------------------------------------------ */
/* Command handling                                                    */
/* ------------------------------------------------------------------ */

static void handle_set_mode(struct crtd *d, const struct crt1_hdr *h,
                            const uint8_t *pl)
{
    struct wire_modeline m;
    struct mode_result res;

    if (h->len < sizeof(m)) {
        send_status(d, h->seq, ST_EBADCMD);
        return;
    }
    memcpy(&m, pl, sizeof(m));

    if (d->enforce && m.htotal && m.vtotal) {
        double hf = (double)m.pclock_hz / m.htotal;
        double vf = hf / m.vtotal *
                    ((m.mode_flags & MF_INTERLACE) ? 2.0 : 1.0);
        if (hf < d->clamp_hfreq_min || hf > d->clamp_hfreq_max ||
            vf < d->clamp_vfreq_min || vf > d->clamp_vfreq_max) {
            fprintf(stderr, "crtd: mode rejected by profile clamp "
                    "(hf=%.1f vf=%.2f)\n", hf, vf);
            memset(&res, 0, sizeof(res));
            res.status = ST_ERANGE;
            res.host_tag = m.host_tag;
            send_evt(d, EVT_MODE_RESULT, h->seq, &res, sizeof(res));
            return;
        }
    }

    if (drm_out_set_mode(&d->out, &m, &res) == 0) {
        d->have_mode = 1;
        d->pixfmt = m.pixfmt;
        res.achieved_vfreq_uhz = drm_out_measure_vfreq_uhz(&d->out, 30);
    }
    send_evt(d, EVT_MODE_RESULT, h->seq, &res, sizeof(res));
}

static void handle_frame(struct crtd *d, const struct crt1_hdr *h,
                         const uint8_t *pl)
{
    struct frame_hdr fh;

    if (!d->have_mode || h->len < sizeof(fh)) {
        if (h->flags & FLG_WANT_REPLY)
            send_status(d, h->seq, ST_EBUSY);
        return;
    }
    memcpy(&fh, pl, sizeof(fh));
    if (drm_out_blit(&d->out, &fh, pl + sizeof(fh), d->pixfmt) == 0) {
        d->frame_count++;
        if (h->flags & FLG_WANT_REPLY)
            send_status(d, h->seq, ST_OK);
    } else if (h->flags & FLG_WANT_REPLY) {
        send_status(d, h->seq, ST_EBADCMD);
    }
}

static void dispatch(struct crtd *d, const struct crt1_hdr *h,
                     const uint8_t *pl)
{
    switch (h->cmd) {
    case CMD_SET_MODE:
        handle_set_mode(d, h, pl);
        break;
    case CMD_FRAME:
        handle_frame(d, h, pl);
        break;
    case CMD_VSYNC_SUB:
        d->vsync_sub = h->len >= 1 && pl[0];
        send_status(d, h->seq, ST_OK);
        break;
    case CMD_SET_PROFILE:
        if (h->len && h->len < sizeof(d->profile)) {
            memcpy(d->profile, pl, h->len);
            d->profile[h->len] = 0;
            parse_profile_clamp(d);
            /* TODO M3: persist + (lane 2) regen modelist & re-enumerate */
            send_status(d, h->seq, ST_OK);
        } else {
            send_status(d, h->seq, ST_EBADCMD);
        }
        break;
    case CMD_SET_ENFORCE:
        if (h->len >= 1) {
            d->enforce = !!pl[0];
            send_status(d, h->seq, ST_OK);
        }
        break;
    case CMD_SET_LANE:
        /* TODO M3: hand display to/from GUD gadget. Ack, don't act, yet. */
        send_status(d, h->seq, h->len >= 1 ? ST_OK : ST_EBADCMD);
        break;
    case CMD_GET_INFO: {
        char info[128];
        int n = snprintf(info, sizeof(info),
                         "crtpi " CRTPI_VERSION " proto=2 lane=%d profile=%s",
                         d->lane, d->profile);
        send_evt(d, EVT_INFO, h->seq, info, (uint32_t)n);
        break;
    }
    default:
        send_status(d, h->seq, ST_EBADCMD);
    }
}

/* Read exactly n bytes from the OUT endpoint (bulk reads can split). */
static int read_full(int fd, uint8_t *buf, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0)
            return -1;
        got += (uint32_t)r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ep0: bind/enable events                                             */
/* ------------------------------------------------------------------ */

static int service_ep0(struct crtd *d)
{
    struct usb_functionfs_event ev;
    ssize_t r = read(d->ep0, &ev, sizeof(ev));
    if (r < (ssize_t)sizeof(ev))
        return -1;
    switch (ev.type) {
    case FUNCTIONFS_ENABLE:
        printf("crtd: host enabled function\n");
        d->bound = 1;
        break;
    case FUNCTIONFS_DISABLE:
    case FUNCTIONFS_UNBIND:
        printf("crtd: function disabled\n");
        d->bound = 0;
        splash_show(d);         /* host gone -> put the idle pattern back */
        break;
    case FUNCTIONFS_SETUP:
        /* No class-specific ep0 traffic in v2; stall by reading/writing 0 */
        if (ev.u.setup.bRequestType & USB_DIR_IN)
            (void)!write(d->ep0, NULL, 0);
        else
            (void)!read(d->ep0, NULL, 0);
        break;
    default:
        break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *ffs = argc > 1 ? argv[1] : FFS_DIR_DEFAULT;
    const char *conf = argc > 2 ? argv[2] : CONF_DEFAULT;
    char path[300];
    struct crtd d;

    memset(&d, 0, sizeof(d));
    load_config(&d, conf);

    d.pkt = malloc(MAX_FRAME_BYTES);
    if (!d.pkt)
        return 1;

    if (drm_out_open(&d.out, DRM_MODE_CONNECTOR_DPI) < 0) {
        fprintf(stderr, "crtd: DPI connector not found, trying any\n");
        if (drm_out_open(&d.out, 0) < 0)
            return 1;
    }

    splash_show(&d);            /* powered-on pattern before any host */

    snprintf(path, sizeof(path), "%s/ep0", ffs);
    d.ep0 = open(path, O_RDWR);
    if (d.ep0 < 0) {
        perror(path);
        return 1;
    }
    if (write(d.ep0, &descriptors, sizeof(descriptors)) < 0 ||
        write(d.ep0, &strings, sizeof(strings)) < 0) {
        perror("crtd: ep0 descriptor write");
        return 1;
    }
    snprintf(path, sizeof(path), "%s/ep1", ffs);
    d.ep_out = open(path, O_RDWR);
    snprintf(path, sizeof(path), "%s/ep2", ffs);
    d.ep_in = open(path, O_RDWR);
    if (d.ep_out < 0 || d.ep_in < 0) {
        perror("crtd: ep open");
        return 1;
    }
    printf("crtd " CRTPI_VERSION ": FunctionFS up at %s, waiting for host\n", ffs);

    struct pollfd pfd[2] = {
        { .fd = d.ep0,    .events = POLLIN },
        { .fd = d.ep_out, .events = POLLIN },
    };

    for (;;) {
        /* When a host is bound, block on the OUT ep via read; ep0 events are
         * rare, so poll both with a timeout that doubles as the resync
         * watchdog boundary. */
        int rc = poll(pfd, 2, 250);
        if (rc < 0)
            break;

        if (pfd[0].revents & POLLIN)
            if (service_ep0(&d) < 0)
                break;

        if (!(pfd[1].revents & POLLIN))
            continue;

        struct crt1_hdr h;
        if (read_full(d.ep_out, (uint8_t *)&h, sizeof(h)) < 0)
            continue;
        if (h.magic != CRT1_MAGIC) {
            /* resync: drain until the next plausible header boundary; with
             * bulk framing the simplest robust recovery is to flush the
             * pipe and let the host's watchdog resend. */
            uint8_t junk[512];
            while (read(d.ep_out, junk, sizeof(junk)) == sizeof(junk))
                ;
            continue;
        }
        if (h.len > MAX_FRAME_BYTES) {
            send_status(&d, h.seq, ST_EBADCMD);
            continue;
        }
        if (h.len && read_full(d.ep_out, d.pkt, h.len) < 0)
            continue;

        dispatch(&d, &h, d.pkt);

        if (d.vsync_sub && d.have_mode) {
            /* piggyback one vsync event per serviced packet batch;
             * M2 moves this to a dedicated vblank-event thread */
            struct vsync_evt v = {
                .timestamp_ns = 0, .frame_count = d.frame_count
            };
            send_evt(&d, EVT_VSYNC, 0, &v, sizeof(v));
        }
    }

    drm_out_close(&d.out);
    return 0;
}
