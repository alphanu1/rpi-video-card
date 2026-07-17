# CRTPi — 15kHz USB display appliance (Raspberry Pi 4)

A headless Buildroot Linux that turns a Pi 4 into a USB video card for
fixed-frequency CRTs (15kHz arcade/SCART monitors — "fixed" describes the
monitor's narrow sync band, not this device: mode timing here is fully
dynamic, synthesized at runtime to land inside that band). Two lanes over
one composite USB gadget:

- **Lane 1 (native, ships first):** vendor bulk protocol; the host's own
  switchres sends *verbatim modelines* (exact fractional refreshes,
  GroovyMAME-grade). Works on Windows (WinUSB) and Linux (libusb).
- **Lane 2 (GUD, v2):** enumerates as a Generic USB Display on Linux;
  the device runs libswitchres at boot to generate a curated, profile-driven
  mode list. Mode-on-demand planned on top.

Both lanes converge on one device-side contract: *receive a fully specified
mode, program vc4 DPI via KMS, report what the PLL actually achieved.*

## Repository map

    docs/CODE_REVIEW_switchres_VC4.md  review of the Pi3-era VC4 branch
    docs/PROTOCOL.md                   Lane 1 wire protocol v2 ("CRT1")
    daemon/protocol.h                  wire structs (shared with host lib)
    daemon/drm_apply.[ch]              KMS modeline applier + clock readback
    daemon/crt_apply.c                 bring-up harness / regression tool
    daemon/crtd.c                      device daemon (FunctionFS Lane 1)
    daemon/crtpi.conf.example          appliance configuration
    scripts/gadget-setup.sh            configfs composite gadget bring-up
    buildroot-external/                BR2_EXTERNAL tree (packages, board)

`crtd` and `crt_apply` compile clean against libdrm on any Linux — you can
dry-run the applier on a desktop with KMS before the Pi image exists
(`crt_apply -c any ...` from a VT, not under X/Wayland).

## Where switchres comes from

switchres is NOT vendored in this repository — there is no cloned copy in
the tree. `buildroot-external/package/switchres/switchres.mk` is a recipe:
at build time Buildroot clones https://github.com/alphanu1/switchres.git
into its download cache (`dl/switchres/`), extracts to
`output/build/switchres-<version>/`, cross-compiles `libswitchres.so`, and
installs it into the target rootfs. By contrast `crtd` uses
`SITE_METHOD = local` pointing at the in-tree `daemon/` sources — our code
ships in the repo; external dependencies are fetched and pinned. That
asymmetry is the conventional Buildroot line.

Three workflows to know:

1. **Reproducible builds — pin the hash.** `SWITCHRES_VERSION = master`
   builds whatever the fork's master is that day. Before M3 work starts,
   set it to the full 40-char commit hash of a reviewed revision.
2. **Hacking on switchres itself — override, don't re-fetch.** Put
   `SWITCHRES_OVERRIDE_SRCDIR = /path/to/your/switchres` in a `local.mk`
   next to your Buildroot `.config`; Buildroot then rsyncs your live
   checkout into the build. Edit, `make switchres-rebuild`, repeat. The
   pinned git fetch remains the clean/CI path.
3. **Optional: vendor as a git submodule** and point `SWITCHRES_SITE` at
   it with `SITE_METHOD = local` — one offline-buildable tree, revision
   frozen by construction, at the cost of submodule ergonomics.

## Milestones

M1 — **Timing proof (the critical path).** Build `crt_apply` on a Pi 4
     running any KMS-enabled OS. Apply a battery of switchres-computed 15kHz
     modelines to the DPI output; record requested-vs-achieved pixel clock
     (debugfs readback) and measured vfreq (vblank timing). This retires the
     one open technical risk: vc4 PLL quantization at 4-9MHz dot clocks.
     Suggested battery:
       crt_apply -b 6400000 320 328 359 407 240 244 247 262     # 60.02
       crt_apply -b 7156800 384 396 432 456 224 232 235 261     # ~55.0
       crt_apply -b 6293750 256 264 288 320 240 244 247 262     # tight 256w
       crt_apply -b 4992000 256 262 285 317 224 229 232 252 i   # low clock
M2 — **Lane 1 end-to-end.** Buildroot image boots, gadget enumerates, a
     Python host script sends SET_MODE + frames, pixels land on glass.
     Dedicated vblank thread for EVT_VSYNC. Host C library (libusb/WinUSB).
M3 — **Switchres on device.** Package libswitchres (BR2 package included),
     replace the stub profile clamp in crtd with real crt_range validation,
     persist profiles, CMD_SET_LANE actually hands off the display.
M4 — **Lane 2.** GUD gadget function (notro's out-of-tree f_gud_drm, or a
     userspace GUD-over-FunctionFS implementation — decision after M2 based
     on kernel-maintenance appetite), boot-time mode list generation,
     hotplug re-enumeration, mode-on-demand command.
M5 — **Polish.** Read-only rootfs A/B, <5s boot, interlace kernel patches
     vendored, RetroArch/GroovyMAME host integration.

## Building the image (M2)

### Host prerequisites (Linux PC — the build cross-compiles for the Pi)

Debian / Ubuntu:

    sudo apt install build-essential gcc g++ make git unzip rsync bc wget \
        cpio file libssl-dev libncurses-dev python3 perl bzip2 xz-utils

Fedora / RHEL-family equivalent:

    sudo dnf install gcc gcc-c++ make git unzip rsync bc wget cpio file \
        openssl-devel ncurses-devel python3 perl bzip2 xz which

Arch / Manjaro:

    sudo pacman -S --needed base-devel git unzip rsync bc wget cpio \
        openssl ncurses python perl

Notes:
- `rsync` is required by Buildroot for local-source packages (crtd) and
  the rootfs overlay — its absence only surfaces mid-build, so install it
  up front. `libncurses-dev` is only needed for `make menuconfig`;
  `libssl-dev` is needed by the kernel build.
- Disk: ~15-20 GB free. RAM: 4 GB+ recommended. No root needed for the
  build itself. First build compiles a full cross-toolchain (30-90 min);
  later builds are incremental.
- WSL2 works; keep the tree on the Linux filesystem (not /mnt/c).
- For `make daemon` (native build of crtd/crt_apply for M1 bench work),
  the only extra host package is `libdrm-dev` (Fedora: `libdrm-devel`,
  Arch: `libdrm`).

### Build

One command; everything else is fetched automatically:

    git clone <this repo> crtpi && cd crtpi
    make

The top-level Makefile clones Buildroot (pinned to 2024.02.9), applies
`buildroot-external/configs/crtpi4_defconfig`, and builds the SD image to
`buildroot/output/images/sdcard.img`. During that build, Buildroot itself
fetches the kernel, the Pi firmware, and switchres (see "Where switchres
comes from") — no separate clone steps. `make menuconfig` tweaks the
config; `make daemon` builds crtd/crt_apply natively for M1 bench work.
First build takes a while (it compiles a cross-toolchain); subsequent
builds are incremental.

The defconfig is written against Buildroot 2024.02.x and pins the rpi
kernel tarball; when M1 fixes the kernel we standardize on, bump both in
one commit — the DPI overlay names and vc4 behavior are kernel-sensitive,
so pin after measuring, not before.

## Hardware

DPI GPIO0-21 through a vga666-style resistor DAC (RGB666). Same sync rules
as ever: csync via the DPI sync line or an external XNOR, 470R into SCART.
The Pi 4's USB-C port is the device port; power the board through the GPIO
header or a powered splitter since USB-C is occupied by data.

## Safety

`profile_enforce = on` clamps every incoming modeline against the monitor
profile's hfreq/vfreq window before it reaches the PLL. Fixed-frequency
deflection circuits can be damaged by out-of-range sync; the clamp is the
device-side seatbelt and stays on by default.
