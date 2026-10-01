# Busybox Port for FelinOS
# ========================

include ../../config.mk

PORT_DIR        := $(CURDIR)
BUSYBOX_SRC     := $(BUILD_DIR)/busybox
BUSYBOX_BUILD   := $(BUILD_DIR)/busybox-build
BUSYBOX_URL     := https://busybox.net/downloads/busybox-$(BUSYBOX_VERSION).tar.bz2

# Busybox config: minimal para FelinOS
# ash shell, coreutils básicos, init, networking
BUSYBOX_DEFCONFIG := felinos_defconfig

.PHONY: all download extract patch configure build install clean distclean

all: install

# -----------------------------------------------------------------------------
# Download
# -----------------------------------------------------------------------------
download:
	@mkdir -p $(DOWNLOAD_DIR)
	$(call download,$(BUSYBOX_URL))

# -----------------------------------------------------------------------------
# Extract
# -----------------------------------------------------------------------------
extract: download
	@mkdir -p $(BUILD_DIR)
	$(call extract,busybox-$(BUSYBOX_VERSION).tar.bz2,busybox)

# -----------------------------------------------------------------------------
# Patch para FelinOS
# -----------------------------------------------------------------------------
patch: extract
	@echo "  PATCH     busybox para FelinOS"
	@mkdir -p $(PORT_DIR)/patches
	@$(MAKE) -s apply-patches

apply-patches:
	@for p in $(PORT_DIR)/patches/*.patch; do \
		if [ -f "$$p" ]; then \
			echo "    Applying $$p"; \
			cd $(BUSYBOX_SRC) && $(PATCH) < "$$p"; \
		fi \
	done
	# Aplicar config customizada
	@cp $(PORT_DIR)/felinos_defconfig $(BUSYBOX_SRC)/.config

# -----------------------------------------------------------------------------
# Configure
# -----------------------------------------------------------------------------
configure: patch
	@echo "  CONFIGURE busybox"
	@mkdir -p $(BUSYBOX_BUILD)
	@cd $(BUSYBOX_BUILD) && \
		$(MAKE) -C $(BUSYBOX_SRC) O=$(BUSYBOX_BUILD) \
			ARCH=x86_64 CROSS_COMPILE=$(CROSS_PREFIX) \
			olddefconfig >/dev/null 2>&1
	@touch $(BUSYBOX_BUILD)/.configured

# -----------------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------------
build: configure
	@echo "  BUILD     busybox"
	@cd $(BUSYBOX_BUILD) && \
		$(MAKE) -j$(JOBS) \
			ARCH=x86_64 CROSS_COMPILE=$(CROSS_PREFIX) \
			CONFIG_PREFIX=$(SYSROOT) \
			> /dev/null 2>&1
	@touch $(BUSYBOX_BUILD)/.built

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------
install: build
	@echo "  INSTALL   busybox -> $(SYSROOT)"
	@cd $(BUSYBOX_BUILD) && \
		$(MAKE) CONFIG_PREFIX=$(SYSROOT) install >/dev/null 2>&1
	# Criar symlinks úteis
	@ln -sf busybox $(SYSROOT)/bin/sh
	@ln -sf busybox $(SYSROOT)/bin/ash
	@ln -sf busybox $(SYSROOT)/sbin/init
	@touch $(BUSYBOX_BUILD)/.installed

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------
clean:
	@rm -rf $(BUSYBOX_BUILD)

distclean:
	@rm -rf $(BUSYBOX_SRC) $(BUSYBOX_BUILD)