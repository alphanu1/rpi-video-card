/* drm_apply.h -- runtime-modeline KMS applier for vc4 DPI (and any KMS out)
 *
 * Author: Ben Templeman (alphanu1)
 * Date:   2026-07-17
 *
 * This is the successor to custom_video_pi.cpp's firmware path: same job
 * (apply an arbitrary switchres-semantics modeline), new substrate (DRM
 * atomic-era KMS on Pi 4). It is deliberately switchres-free so it can be
 * exercised standalone by the bring-up harness; the daemon feeds it either
 * host modelines (Lane 1) or libswitchres output (Lane 2 / on-demand).
 */
#ifndef CRTPI_DRM_APPLY_H
#define CRTPI_DRM_APPLY_H

#include <stdint.h>
#include "protocol.h"

struct drm_out {
    int      fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    int      crtc_index;        /* for drmWaitVBlank HIGH_CRTC routing */
    uint32_t encoder_id;
    /* current scanout dumb buffer */
    uint32_t fb_id;
    uint32_t bo_handle;
    uint32_t pitch;
    uint64_t size;
    uint8_t *map;               /* mmap'd scanout memory */
    uint32_t width, height, bpp;
    int      connector_type;    /* DRM_MODE_CONNECTOR_* actually chosen */
};

/* Open first card with a connected connector of preferred type.
 * type_pref: DRM_MODE_CONNECTOR_DPI for the vga666-style DAC; pass 0 to
 * take any connected connector (useful for HDMI-DAC rigs and desk testing).
 * Returns 0 on success. */
int drm_out_open(struct drm_out *o, int type_pref);

/* Apply a wire_modeline: build drmModeModeInfo (position semantics map 1:1
 * onto DRM's hsync_start/hsync_end -- no porch conversion needed here, DRM
 * uses positions like switchres does), (re)allocate the dumb buffer to the
 * active size, and commit via drmModeSetCrtc.
 * Fills res with status + achieved timing readback. Returns 0 if committed. */
int drm_out_set_mode(struct drm_out *o, const struct wire_modeline *m,
                     struct mode_result *res);

/* Measure real vfreq over n vblanks (blocking ~n frames). Returns uHz, 0 on
 * failure. Also used to service EVT_VSYNC when the daemon runs it async. */
uint32_t drm_out_measure_vfreq_uhz(struct drm_out *o, int n);

/* Try to read the clock vc4 actually programmed, from
 * /sys/kernel/debug/dri/N/state (adjusted mode line). Returns Hz or 0. */
uint32_t drm_out_readback_clock_hz(struct drm_out *o);

/* Blit a PIXFMT_* rect into the scanout buffer (row-major src). */
int drm_out_blit(struct drm_out *o, const struct frame_hdr *r,
                 const uint8_t *px, uint8_t pixfmt);

void drm_out_close(struct drm_out *o);

#endif
