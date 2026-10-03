# Musl Libc Port for FelinOS
# ==========================

include ../../config.mk

PORT_DIR        := $(CURDIR)
MUSL_SRC        := $(BUILD_DIR)/musl
MUSL_BUILD      := $(BUILD_DIR)/musl-build
MUSL_URL        := https://musl.libc.org/releases/musl-$(MUSL_VERSION).tar.gz

# Musl config para FelinOS
MUSL_CONFIG     := --target=$(TARGET) \
                   --prefix=/usr \
                   --disable-shared \
                   --enable-static \
                   --enable-optimize=size \
                   --disable-debug \
                   --disable-wchar \
                   --disable-legacy-malloc \
                   --enable-syscall-stub

.PHONY: all download extract patch configure build install clean distclean

all: install

# -----------------------------------------------------------------------------
# Download
# -----------------------------------------------------------------------------
download:
	@mkdir -p $(DOWNLOAD_DIR)
	$(call download,$(MUSL_URL))

# -----------------------------------------------------------------------------
# Extract
# -----------------------------------------------------------------------------
extract: download
	@mkdir -p $(BUILD_DIR)
	$(call extract,musl-$(MUSL_VERSION).tar.gz,musl)

# -----------------------------------------------------------------------------
# Patch para FelinOS
# -----------------------------------------------------------------------------
patch: extract
	@echo "  PATCH     musl para FelinOS"
	@mkdir -p $(PORT_DIR)/patches
	@$(MAKE) -s apply-patches

apply-patches:
	@for p in $(PORT_DIR)/patches/*.patch; do \
		if [ -f "$$p" ]; then \
			echo "    Applying $$p"; \
			cd $(MUSL_SRC) && $(PATCH) < "$$p"; \
		fi \
	done

# -----------------------------------------------------------------------------
# Configure
# -----------------------------------------------------------------------------
configure: patch
	@echo "  CONFIGURE musl"
	@mkdir -p $(MUSL_BUILD)
	@cd $(MUSL_BUILD) && \
		CC=$(CC) CFLAGS="$(TARGET_CFLAGS)" \
		$(MUSL_SRC)/configure $(MUSL_CONFIG) >/dev/null 2>&1
	@touch $(MUSL_BUILD)/.configured

# -----------------------------------------------------------------------------
# Build
# -----------------------------------------------------------------------------
build: configure
	@echo "  BUILD     musl"
	@cd $(MUSL_BUILD) && $(MAKE) -j$(JOBS) >/dev/null 2>&1
	@touch $(MUSL_BUILD)/.built

# -----------------------------------------------------------------------------
# Install
# -----------------------------------------------------------------------------
install: build
	@echo "  INSTALL   musl -> $(SYSROOT)"
	@cd $(MUSL_BUILD) && \
		$(MAKE) DESTDIR=$(SYSROOT) install >/dev/null 2>&1
	# Criar symlink ld-musl para compatibilidade
	@ln -sf libc.so $(SYSROOT)/lib/ld-musl-x86_64.so.1
	@touch $(MUSL_BUILD)/.installed

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------
clean:
	@rm -rf $(MUSL_BUILD)

distclean:
	@rm -rf $(MUSL_SRC) $(MUSL_BUILD)