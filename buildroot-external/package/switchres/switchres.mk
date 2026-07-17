# Author: Ben Templeman (alphanu1)
# Date:   2026-07-17
################################################################################
#
# switchres -- pinned to a commit hash (newer Buildroot REJECTS branch names
# for git downloads; a full 40-char hash is mandatory). This hash is
# alphanu1/switchres master as of 2026-07-17 (== upstream 2.2.2 sync).
# Bump deliberately, after reviewing what changed.
#
################################################################################

SWITCHRES_VERSION = da27cc69b59c9c274bffae51ab148dedcf2b0a75
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

# Upstream links the versioned name only (soname .so.2, no unversioned
# symlink in the build dir); install it and create the links ourselves.
# The 2.2.2 suffix is tied to the pinned hash -- update together.
define SWITCHRES_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/switchres_wrapper.h \
		$(STAGING_DIR)/usr/include/switchres_wrapper.h
	$(INSTALL) -D -m 0755 $(@D)/libswitchres.so.2.2.2 \
		$(STAGING_DIR)/usr/lib/libswitchres.so.2.2.2
	ln -sf libswitchres.so.2.2.2 $(STAGING_DIR)/usr/lib/libswitchres.so.2
	ln -sf libswitchres.so.2 $(STAGING_DIR)/usr/lib/libswitchres.so
endef

define SWITCHRES_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libswitchres.so.2.2.2 \
		$(TARGET_DIR)/usr/lib/libswitchres.so.2.2.2
	ln -sf libswitchres.so.2.2.2 $(TARGET_DIR)/usr/lib/libswitchres.so.2
	ln -sf libswitchres.so.2 $(TARGET_DIR)/usr/lib/libswitchres.so
	$(INSTALL) -D -m 0644 $(@D)/switchres.ini \
		$(TARGET_DIR)/etc/switchres.ini
endef

$(eval $(generic-package))
