# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
################################################################################
#
# switchres -- pin SWITCHRES_VERSION to a reviewed commit hash before release
#
################################################################################

SWITCHRES_VERSION = master
SWITCHRES_SITE = https://github.com/alphanu1/switchres.git
SWITCHRES_SITE_METHOD = git
SWITCHRES_LICENSE = GPL-2.0+
SWITCHRES_LICENSE_FILES = LICENSE
SWITCHRES_INSTALL_STAGING = YES
SWITCHRES_DEPENDENCIES = libdrm

# Upstream makefile targets: 'libswitchres' builds the shared lib.
# PLATFORM/CROSS handling follows the upstream makefile conventions; verify
# against the pinned revision when bumping.
define SWITCHRES_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CXX="$(TARGET_CXX)" \
		PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		libswitchres
endef

define SWITCHRES_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/switchres_wrapper.h \
		$(STAGING_DIR)/usr/include/switchres_wrapper.h
	$(INSTALL) -D -m 0755 $(@D)/libswitchres.so \
		$(STAGING_DIR)/usr/lib/libswitchres.so
endef

define SWITCHRES_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libswitchres.so \
		$(TARGET_DIR)/usr/lib/libswitchres.so
	$(INSTALL) -D -m 0644 $(@D)/switchres.ini \
		$(TARGET_DIR)/etc/switchres.ini
endef

$(eval $(generic-package))
