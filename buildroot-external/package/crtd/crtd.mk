# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
################################################################################
#
# crtd -- built from the in-tree daemon/ sources
#
################################################################################

CRTD_VERSION = 1.0
CRTD_SITE = $(BR2_EXTERNAL_CRTPI_PATH)/../daemon
CRTD_SITE_METHOD = local
CRTD_LICENSE = GPL-2.0+
CRTD_DEPENDENCIES = libdrm

define CRTD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		CRTPI_VERSION="`cat $(BR2_EXTERNAL_CRTPI_PATH)/../VERSION 2>/dev/null || echo dev`"
endef

define CRTD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/crtd $(TARGET_DIR)/usr/bin/crtd
	$(INSTALL) -D -m 0755 $(@D)/crt_apply $(TARGET_DIR)/usr/bin/crt_apply
	$(INSTALL) -D -m 0644 $(@D)/crtpi.conf.example $(TARGET_DIR)/etc/crtpi.conf
endef

$(eval $(generic-package))
