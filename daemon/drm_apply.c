/* drm_apply.c -- see drm_apply.h
 *
 * Author: Ben Templeman (alphanu1)
 * Date:   2026-07-17
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "drm_apply.h"

static int find_crtc_for_connector(int fd, drmModeRes *res,
                                   drmModeConnector *conn,
                                   uint32_t *crtc_id, int *crtc_index)
{
    /* Prefer the currently attached crtc, else first compatible. */
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
        if (enc) {
            if (enc->crtc_id) {
                for (int i = 0; i < res->count_crtcs; i++) {
                    if (res->crtcs[i] == enc->crtc_id) {
                        *crtc_id = enc->crtc_id;
                        *crtc_index = i;
                        drmModeFreeEncoder(enc);
                        return 0;
                    }
                }
            }
            drmModeFreeEncoder(enc);
        }
    }
    for (int e = 0; e < conn->count_encoders; e++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc)
            continue;
        for (int i = 0; i < res->count_crtcs; i++) {
            if (enc->possible_crtcs & (1u << i)) {
                *crtc_id = res->crtcs[i];
                *crtc_index = i;
                drmModeFreeEncoder(enc);
                return 0;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return -1;
}

int drm_out_open(struct drm_out *o, int type_pref)
{
    char path[32];

    memset(o, 0, sizeof(*o));
    o->fd = -1;

    for (int card = 0; card < 8; card++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", card);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            close(fd);
            continue;
        }

        drmModeConnector *pick = NULL;
        for (int pass = 0; pass < 2 && !pick; pass++) {
            for (int c = 0; c < res->count_connectors; c++) {
                drmModeConnector *conn =
                    drmModeGetConnector(fd, res->connectors[c]);
                if (!conn)
                    continue;
                int type_ok = (pass == 1) || !type_pref ||
                              (int)conn->connector_type == type_pref;
                /* DPI panels often report UNKNOWN status with no attached
                 * "monitor"; accept != disconnected rather than requiring
                 * connected. */
                if (type_ok &&
                    conn->connection != DRM_MODE_DISCONNECTED) {
                    pick = conn;
                    break;
                }
                drmModeFreeConnector(conn);
            }
            if (type_pref == 0)
                break; /* one pass is enough when no preference */
        }

        if (pick) {
            uint32_t crtc_id;
            int crtc_index;
            if (find_crtc_for_connector(fd, res, pick,
                                        &crtc_id, &crtc_index) == 0) {
                o->fd = fd;
                o->connector_id = pick->connector_id;
                o->connector_type = (int)pick->connector_type;
                o->crtc_id = crtc_id;
                o->crtc_index = crtc_index;
                drmModeFreeConnector(pick);
                drmModeFreeResources(res);
                /* Sole client on the appliance => master is implicit, but
                 * ask anyway; failure only matters if someone else owns it */
                drmSetMaster(o->fd);
                return 0;
            }
            drmModeFreeConnector(pick);
        }
        drmModeFreeResources(res);
        close(fd);
    }
    fprintf(stderr, "drm_apply: no usable connector found\n");
    return -1;
}

static void destroy_fb(struct drm_out *o)
{
    if (o->fb_id) {
        drmModeRmFB(o->fd, o->fb_id);
        o->fb_id = 0;
    }
    if (o->map) {
        munmap(o->map, o->size);
        o->map = NULL;
    }
    if (o->bo_handle) {
        struct drm_mode_destroy_dumb d = { .handle = o->bo_handle };
        drmIoctl(o->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        o->bo_handle = 0;
    }
}

static int create_fb(struct drm_out *o, uint32_t w, uint32_t h, uint32_t bpp)
{
    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = bpp };
    if (drmIoctl(o->fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) {
        perror("create_dumb");
        return -1;
    }
    o->bo_handle = c.handle;
    o->pitch = c.pitch;
    o->size = c.size;
    o->width = w;
    o->height = h;
    o->bpp = bpp;

    uint32_t fourcc = (bpp == 16) ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888;
    uint32_t handles[4] = { c.handle }, pitches[4] = { c.pitch },
             offsets[4] = { 0 };
    if (drmModeAddFB2(o->fd, w, h, fourcc, handles, pitches, offsets,
                      &o->fb_id, 0) < 0) {
        /* fall back to legacy AddFB for XRGB8888 */
        if (bpp == 32 &&
            drmModeAddFB(o->fd, w, h, 24, 32, c.pitch, c.handle,
                         &o->fb_id) == 0)
            goto map;
        perror("addfb2");
        destroy_fb(o);
        return -1;
    }
map:;
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(o->fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) {
        perror("map_dumb");
        destroy_fb(o);
        return -1;
    }
    o->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  o->fd, m.offset);
    if (o->map == MAP_FAILED) {
        o->map = NULL;
        perror("mmap");
        destroy_fb(o);
        return -1;
    }
    memset(o->map, 0, c.size);
    return 0;
}

/* switchres positions map 1:1 onto DRM positions -- both use sync start/end,
 * unlike hdmi_timings' porch widths. Only the flags need translation. */
static void wire_to_drm(const struct wire_modeline *m, drmModeModeInfo *dm)
{
    memset(dm, 0, sizeof(*dm));
    dm->clock       = (m->pclock_hz + 500) / 1000;   /* DRM wants kHz */
    dm->hdisplay    = m->hactive;
    dm->hsync_start = m->hbegin;
    dm->hsync_end   = m->hend;
    dm->htotal      = m->htotal;
    dm->vdisplay    = m->vactive;
    dm->vsync_start = m->vbegin;
    dm->vsync_end   = m->vend;
    dm->vtotal      = m->vtotal;
    dm->flags |= (m->mode_flags & MF_HSYNC_POS) ? DRM_MODE_FLAG_PHSYNC
                                                : DRM_MODE_FLAG_NHSYNC;
    dm->flags |= (m->mode_flags & MF_VSYNC_POS) ? DRM_MODE_FLAG_PVSYNC
                                                : DRM_MODE_FLAG_NVSYNC;
    if (m->mode_flags & MF_INTERLACE)
        dm->flags |= DRM_MODE_FLAG_INTERLACE;
    if (m->mode_flags & MF_DOUBLESCAN)
        dm->flags |= DRM_MODE_FLAG_DBLSCAN;

    uint32_t denom = (uint32_t)m->htotal * m->vtotal;
    dm->vrefresh = denom ? (m->pclock_hz + denom / 2) / denom : 0;
    if (m->mode_flags & MF_INTERLACE)
        dm->vrefresh *= 2;
    snprintf(dm->name, sizeof(dm->name), "%ux%u_crtpi",
             m->hactive, m->vactive);
    dm->type = DRM_MODE_TYPE_USERDEF | DRM_MODE_TYPE_DRIVER;
}

int drm_out_set_mode(struct drm_out *o, const struct wire_modeline *m,
                     struct mode_result *res)
{
    drmModeModeInfo dm;

    memset(res, 0, sizeof(*res));
    res->host_tag = m->host_tag;
    memcpy(&res->applied, m, sizeof(*m));

    if (!m->htotal || !m->vtotal || !m->hactive || !m->vactive ||
        m->hbegin < m->hactive || m->hend <= m->hbegin ||
        m->htotal < m->hend || m->vbegin < m->vactive ||
        m->vend <= m->vbegin || m->vtotal < m->vend) {
        res->status = ST_ERANGE;
        return -1;
    }

    wire_to_drm(m, &dm);

    uint32_t bpp = (m->pixfmt == PIXFMT_XRGB8888) ? 32 : 16;
    if (!o->fb_id || o->width != m->hactive || o->height != m->vactive ||
        o->bpp != bpp) {
        destroy_fb(o);
        if (create_fb(o, m->hactive, m->vactive, bpp) < 0) {
            res->status = ST_EKMS;
            return -1;
        }
    }

    if (drmModeSetCrtc(o->fd, o->crtc_id, o->fb_id, 0, 0,
                       &o->connector_id, 1, &dm) < 0) {
        fprintf(stderr, "drm_apply: SetCrtc failed: %s\n", strerror(errno));
        res->status = ST_EKMS;
        return -1;
    }

    res->status = ST_OK;
    res->achieved_pclock_hz = drm_out_readback_clock_hz(o);
    if (!res->achieved_pclock_hz)
        res->achieved_pclock_hz = m->pclock_hz; /* honest fallback: requested */
    res->achieved_hfreq_mhz_x1000 =
        res->achieved_pclock_hz / (m->htotal ? m->htotal : 1);
    return 0;
}

uint32_t drm_out_measure_vfreq_uhz(struct drm_out *o, int n)
{
    drmVBlank vbl;
    struct timespec t0, t1;

    memset(&vbl, 0, sizeof(vbl));
    vbl.request.type = DRM_VBLANK_RELATIVE;
    if (o->crtc_index > 0)
        vbl.request.type |= (o->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT) &
                            DRM_VBLANK_HIGH_CRTC_MASK;
    vbl.request.sequence = 1;

    if (drmWaitVBlank(o->fd, &vbl) < 0)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) {
        vbl.request.type = DRM_VBLANK_RELATIVE;
        if (o->crtc_index > 0)
            vbl.request.type |= (o->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT)
                                & DRM_VBLANK_HIGH_CRTC_MASK;
        vbl.request.sequence = 1;
        if (drmWaitVBlank(o->fd, &vbl) < 0)
            return 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                  (t1.tv_nsec - t0.tv_nsec);
    if (!ns)
        return 0;
    /* n frames in ns nanoseconds -> uHz */
    return (uint32_t)((uint64_t)n * 1000000ull * 1000000000ull / ns);
}

uint32_t drm_out_readback_clock_hz(struct drm_out *o)
{
    /* vc4 may quantize the requested PLL clock; the adjusted mode is visible
     * in debugfs. Requires debugfs mounted (the appliance does this). The
     * format is stable enough for an appliance where we pin the kernel:
     * lines like: "  crtc=..." then "    mode: ..." containing the clock.
     * We grep for the first "clock=" or the mode line on our crtc. Best
     * effort: return 0 if anything is off, callers fall back to requested.
     */
    char path[64], line[512];
    uint32_t khz = 0;
    (void)o; /* card index scan is global; kept for future per-fd mapping */

    for (int n = 0; n < 4 && !khz; n++) {
        snprintf(path, sizeof(path), "/sys/kernel/debug/dri/%d/state", n);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof(line), f)) {
            char *p = strstr(line, "clock=");
            if (p && sscanf(p, "clock=%u", &khz) == 1 && khz)
                break;
            /* "mode:" dump form: name hz clock hdisp hss hse htot ... */
            p = strstr(line, "mode:");
            if (p) {
                unsigned hz, clk;
                char name[64];
                if (sscanf(p, "mode: \"%63[^\"]\": %u %u", name, &hz, &clk)
                        == 3 && clk) {
                    khz = clk;
                    break;
                }
            }
        }
        fclose(f);
    }
    return khz * 1000u;
}

int drm_out_blit(struct drm_out *o, const struct frame_hdr *r,
                 const uint8_t *px, uint8_t pixfmt)
{
    uint32_t bypp = (pixfmt == PIXFMT_XRGB8888) ? 4 : 2;

    if (!o->map || pixfmt != ((o->bpp == 32) ? PIXFMT_XRGB8888
                                             : PIXFMT_RGB565))
        return -1;
    if (r->x + r->w > o->width || r->y + r->h > o->height)
        return -1;

    const uint8_t *src = px;
    uint8_t *dst = o->map + (uint64_t)r->y * o->pitch + (uint64_t)r->x * bypp;
    for (uint32_t row = 0; row < r->h; row++) {
        memcpy(dst, src, (size_t)r->w * bypp);
        src += (size_t)r->w * bypp;
        dst += o->pitch;
    }
    return 0;
}

void drm_out_close(struct drm_out *o)
{
    destroy_fb(o);
    if (o->fd >= 0) {
        drmDropMaster(o->fd);
        close(o->fd);
        o->fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Test pattern: 8 SMPTE-style bars + 1px white geometry border.       */
/* Drawn into the current scanout buffer; used as the powered-on/idle  */
/* splash so a CRT shows life with no USB host connected.              */
/* ------------------------------------------------------------------ */

int drm_out_draw_testpattern(struct drm_out *o)
{
    static const uint32_t bars32[8] = {
        0x00FFFFFF, 0x00FFFF00, 0x0000FFFF, 0x0000FF00,
        0x00FF00FF, 0x00FF0000, 0x000000FF, 0x00000000
    };
    static const uint16_t bars16[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
    };
    uint32_t x, y;

    if (!o->map || !o->width || !o->height)
        return -1;

    for (y = 0; y < o->height; y++) {
        uint8_t *row = o->map + (uint64_t)y * o->pitch;
        int edge_y = (y == 0 || y == o->height - 1);
        for (x = 0; x < o->width; x++) {
            int idx = (int)(x * 8 / o->width);
            int edge = edge_y || x == 0 || x == o->width - 1;
            if (o->bpp == 16)
                ((uint16_t *)row)[x] = edge ? 0xFFFF : bars16[idx];
            else
                ((uint32_t *)row)[x] = edge ? 0x00FFFFFF : bars32[idx];
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal text renderer for the splash status readout. Opaque 8x12    */
/* cells: white glyph on black, so text is legible over the bars.      */
/* ------------------------------------------------------------------ */

#include "font8x12.h"

static void put_cell(struct drm_out *o, uint32_t cx, uint32_t cy,
                     const uint8_t *glyph)
{
    uint32_t gx, gy;
    for (gy = 0; gy < FONT_H; gy++) {
        uint32_t y = cy + gy;
        uint8_t bits = glyph[gy];
        uint8_t *row;
        if (y >= o->height)
            return;
        row = o->map + (uint64_t)y * o->pitch;
        for (gx = 0; gx < FONT_W; gx++) {
            uint32_t x = cx + gx;
            int on = bits & (0x80 >> gx);
            if (x >= o->width)
                break;
            if (o->bpp == 16)
                ((uint16_t *)row)[x] = on ? 0xFFFF : 0x0000;
            else
                ((uint32_t *)row)[x] = on ? 0x00FFFFFF : 0x00000000;
        }
    }
}

int drm_out_draw_text(struct drm_out *o, uint32_t x, uint32_t y,
                      const char *s)
{
    if (!o->map)
        return -1;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c > 126)
            c = '?';
        put_cell(o, x, y, font8x12[c - 32]);
        x += FONT_W;
        if (x + FONT_W > o->width)
            break;
    }
    return 0;
}
