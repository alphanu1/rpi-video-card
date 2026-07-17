# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
#
# CRTPi top-level build driver.
#
#   make            fetch Buildroot (pinned), configure, build the SD image
#   make menuconfig tweak the Buildroot config
#   make daemon     build crtd/crt_apply natively on this machine (for M1)
#   make clean-all  remove the Buildroot tree and all build output
#
# Everything (Buildroot itself, kernel, switchres from GitHub, crtd from
# ./daemon) is fetched/built from this one entry point. Requires the usual
# Buildroot host deps: gcc, g++, make, git, unzip, rsync, bc, wget, cpio.

BR_VERSION   := 2024.02.9
BR_URL       := https://gitlab.com/buildroot.org/buildroot.git
BR_DIR       := buildroot
EXTERNAL     := $(CURDIR)/buildroot-external
DEFCONFIG    := crtpi4_defconfig
IMAGE        := $(BR_DIR)/output/images/sdcard.img

.PHONY: all image menuconfig daemon clean-all

all: image

$(BR_DIR):
	git clone --depth 1 -b $(BR_VERSION) $(BR_URL) $(BR_DIR)

$(BR_DIR)/.config: | $(BR_DIR)
	$(MAKE) -C $(BR_DIR) BR2_EXTERNAL=$(EXTERNAL) $(DEFCONFIG)

image: $(BR_DIR)/.config
	$(MAKE) -C $(BR_DIR) BR2_EXTERNAL=$(EXTERNAL)
	@echo ""
	@echo "Image ready: $(IMAGE)"
	@echo "Write it:    dd if=$(IMAGE) of=/dev/sdX bs=4M conv=fsync"

menuconfig: $(BR_DIR)/.config
	$(MAKE) -C $(BR_DIR) BR2_EXTERNAL=$(EXTERNAL) menuconfig

# Native build of the daemon + M1 harness on the host (needs libdrm-dev)
daemon:
	$(MAKE) -C daemon

clean-all:
	rm -rf $(BR_DIR)
	$(MAKE) -C daemon clean
