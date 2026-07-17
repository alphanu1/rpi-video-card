# Code review: alphanu1/switchres, branch `VC4` (custom_video_pi)

Scope: the 6 commits unique to `origin/VC4` vs upstream — chiefly
`custom_video_pi.cpp` / `custom_video_pi.h` plus makefile/API glue.
(`master` is currently identical to upstream 2.2.2; the patch-1/2/3 branches
are the 2020-2021 `sr_load_ini` wrapper work, since superseded upstream.)

## Verdict

Right architecture for the firmware era (proper `custom_video` subclass,
correct caps flags, and the `hdmi_timings` field-order documentation in the
comments is genuinely valuable reference material). But it is a WIP snapshot
with compile-stopping and runtime-fatal issues — and, more importantly, the
entire substrate it targets (VCHI gencmd + DispmanX fbset) no longer exists
on the Pi 4 under full KMS. Recommendation at the end.

## Compile blockers

1. `custom_video_pi.cpp` `resize_fb()`: `char* output = NULL:` — colon
   instead of semicolon.
2. `get_timing()`: `mode->vactive = g;` — `g` is undefined.
3. Header declares methods with qualified names inside the class body:
   `char* pi_timing::get_vc4_mode();` / `bool pi_timing::resize_fb();` —
   illegal C++; must be `get_vc4_mode();` etc.
4. Signature mismatch: `resize_fb()` declared with no parameters, defined as
   `resize_fb(unsigned width, unsigned height)`.
5. Destructor defined twice: `~pi_timing() {};` inline in the header AND
   `pi_timing::~pi_timing()` in the .cpp.

## Runtime-fatal

6. `buffer`, `set_hdmi_timing` (members) and `output` (local) are raw
   `char*` that are never allocated. Every use writes through a wild/NULL
   pointer, and `sizeof(ptr)` is 8, not a buffer size:
   `snprintf(set_hdmi_timing, sizeof(set_hdmi_timing), ...)` truncates to 7
   chars *and* writes through an uninitialized pointer. Fix: fixed arrays,
   e.g. `char m_cmd[256]; char m_gencmd_buf[512];`
7. `get_timing()` zeroes the mode then computes
   `mode->hfreq = mode->pclock / mode->htotal` — division by zero.
   (Stubbed ToDo — noted, but it must return false *before* the math.)

## Semantic bugs (the ones that would have bitten on real hardware)

8. **Position vs width confusion.** `hdmi_timings` takes porch *widths*:
   `<h_front_porch> <h_sync_pulse> <h_back_porch>`. Switchres modelines
   store sync *positions* (`hbegin` = sync start, `hend` = sync end). The
   code passes `hbegin, hend, htotal` raw. Correct conversion:

       h_fp   = hbegin - hactive
       h_sync = hend   - hbegin
       h_bp   = htotal - hend
       v_fp   = vbegin - vactive
       v_sync = vend   - vbegin
       v_bp   = vtotal - vend

9. Sync polarities hardcoded to `1`; should map from `mode->hsync` /
   `mode->vsync`. 15kHz RGB wants negative sync — this alone would upset
   some monitors.
10. Interlace hardcoded to `0` in the format args; `mode->interlace` unused.
11. `%f` receives `mode->pclock` (integral type in switchres) — UB; and
    `hdmi_timings` wants integer Hz for `<pixel_freq>` anyway.
12. `update_mode()` verifies by `strcmp(get_vc4_mode(), set_hdmi_timing)` —
    the gencmd *query response* is formatted/prefixed differently from the
    *set command string*, so verification would fail even on success.
13. `resize_fb()` shells out to `fbset` (flagged ToDo by the author —
    agreed; on the appliance this becomes a KMS framebuffer, no fbset).

## Recommendation

Do not fix this file — retire it with honors. The VCHI/gencmd path is
Pi-3/firmware-only and is precisely what full KMS removed on the Pi 4. The
knowledge it encodes (field ordering, porch conversion, the need for
polarity/interlace mapping) transfers directly into the appliance's DRM
applier (`daemon/drm_apply.c` in this repo), which is its architectural
successor. Meanwhile upstream 2.2.2's `custom_video_drmkms` (which your
synced master already carries) has grown the KMS features the appliance
wants: set_timing sanity checks, dumb-buffer creation for disabled outputs,
and exposed fd/crtc-index for `drmWaitVBlank`-based sync. Suggested branch
hygiene: tag `VC4` as `archive/pi3-firmware-path` so its history is
preserved and its intent is unambiguous.
