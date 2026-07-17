/* crt_apply.c -- bring-up harness & permanent regression tool
 *
 * Author: Ben Templeman (alphanu1)
 * Date:   2026-07-17
 *
 * Applies a switchres-format modeline to the DPI (or any) connector and
 * reports requested-vs-achieved timing. This is the experiment that retires
 * the last open risk: vc4 PLL quantization behavior at 15kHz dot clocks.
 *
 * Usage:
 *   crt_apply [-c dpi|any] [-t seconds] [-b] \
 *             pclock_hz hact hbeg hend htot vact vbeg vend vtot [i]
 *
 * Example (320x240p60 @ 15.72kHz, our FPGA-phase timing as a modeline):
 *   crt_apply 6400000 320 328 359 407 240 244 247 262
 *
 * Example (GroovyMAME-ish exact-refresh oddity):
 *   crt_apply 7156800 384 396 432 456 224 232 235 261
 *
 *   -b  draw color bars into the scanout buffer (else black)
 *   -t  hold the mode N seconds while measuring vfreq (default 5)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xf86drmMode.h>

#include "drm_apply.h"

static void draw_bars(struct drm_out *o)
{
    if (!o->map)
        return;
    /* 8 vertical RGB565 bars */
    static const uint16_t bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
    };
    for (uint32_t y = 0; y < o->height; y++) {
        uint16_t *row = (uint16_t *)(o->map + (uint64_t)y * o->pitch);
        for (uint32_t x = 0; x < o->width; x++)
            row[x] = bars[x * 8 / o->width];
    }
}

int main(int argc, char **argv)
{
    int opt, hold = 5, want_bars = 0;
    int type_pref = DRM_MODE_CONNECTOR_DPI;

    while ((opt = getopt(argc, argv, "c:t:b")) != -1) {
        switch (opt) {
        case 'c':
            type_pref = strcmp(optarg, "any") ? DRM_MODE_CONNECTOR_DPI : 0;
            break;
        case 't':
            hold = atoi(optarg);
            break;
        case 'b':
            want_bars = 1;
            break;
        default:
            return 2;
        }
    }
    if (argc - optind < 9) {
        fprintf(stderr, "usage: %s [-c dpi|any] [-t sec] [-b] "
                "pclk hact hbeg hend htot vact vbeg vend vtot [i]\n",
                argv[0]);
        return 2;
    }

    struct wire_modeline m;
    memset(&m, 0, sizeof(m));
    m.pclock_hz = (uint32_t)strtoul(argv[optind + 0], NULL, 0);
    m.hactive = atoi(argv[optind + 1]);
    m.hbegin  = atoi(argv[optind + 2]);
    m.hend    = atoi(argv[optind + 3]);
    m.htotal  = atoi(argv[optind + 4]);
    m.vactive = atoi(argv[optind + 5]);
    m.vbegin  = atoi(argv[optind + 6]);
    m.vend    = atoi(argv[optind + 7]);
    m.vtotal  = atoi(argv[optind + 8]);
    if (argc - optind > 9 && argv[optind + 9][0] == 'i')
        m.mode_flags |= MF_INTERLACE;
    m.pixfmt = PIXFMT_RGB565;

    double req_hfreq = (double)m.pclock_hz / m.htotal;
    double req_vfreq = req_hfreq / m.vtotal *
                       ((m.mode_flags & MF_INTERLACE) ? 2.0 : 1.0);
    printf("requested: pclk=%u Hz  hfreq=%.3f kHz  vfreq=%.4f Hz\n",
           m.pclock_hz, req_hfreq / 1000.0, req_vfreq);

    struct drm_out o;
    if (drm_out_open(&o, type_pref) < 0)
        return 1;
    printf("output: connector_id=%u type=%d crtc=%u(idx %d)\n",
           o.connector_id, o.connector_type, o.crtc_id, o.crtc_index);

    struct mode_result res;
    if (drm_out_set_mode(&o, &m, &res) < 0) {
        fprintf(stderr, "set_mode failed, status=%u\n", res.status);
        drm_out_close(&o);
        return 1;
    }
    if (want_bars)
        draw_bars(&o);

    printf("committed. readback pclk=%u Hz (delta %+d Hz, %+.4f%%)\n",
           res.achieved_pclock_hz,
           (int)res.achieved_pclock_hz - (int)m.pclock_hz,
           m.pclock_hz ? 100.0 *
               ((double)res.achieved_pclock_hz - m.pclock_hz) / m.pclock_hz
                       : 0.0);

    int frames = (int)(req_vfreq * hold);
    if (frames < 30)
        frames = 30;
    printf("measuring vfreq over %d vblanks...\n", frames);
    uint32_t uhz = drm_out_measure_vfreq_uhz(&o, frames);
    if (uhz)
        printf("measured vfreq = %u.%06u Hz (requested %.4f)\n",
               uhz / 1000000, uhz % 1000000, req_vfreq);
    else
        printf("vblank measurement unavailable\n");

    drm_out_close(&o);
    return 0;
}
