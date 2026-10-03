# s6 Init System for FelinOS
# ==========================
# Builds: skalibs -> execline -> s6

include ../../config.mk

PORT_DIR        := $(CURDIR)
BUILD_SKALIBS   := $(BUILD_DIR)/skalibs
BUILD_EXECLINE  := $(BUILD_DIR)/execline
BUILD_S6        := $(BUILD_DIR)/s6

SKALIBS_VERSION  := 2.14.1.0
EXECLINE_VERSION := 2.9.6.0
S6_VERSION       := 2.13.1.0

SKALIBS_URL  := https://skarnet.org/software/skalibs/skalibs-$(SKALIBS_VERSION).tar.gz
EXECLINE_URL := https://skarnet.org/software/execline/execline-$(EXECLINE_VERSION).tar.gz
S6_URL       := https://skarnet.org/software/s6/s6-$(S6_VERSION).tar.gz

# Configure flags for static cross-compilation
CONFIGURE_FLAGS := --prefix=/usr \
                   --enable-static \
                   --disable-shared \
                   --disable-allstatic \
                   --with-sysroot=$(SYSROOT) \
                   CC=$(CC) CFLAGS="$(TARGET_CFLAGS)" \
                   LDFLAGS="$(TARGET_LDFLAGS)"

.PHONY: all download extract patch configure build install clean distclean \
        skalibs execline s6

all: install

# -----------------------------------------------------------------------------
# Download
# -----------------------------------------------------------------------------
download:
	@mkdir -p $(DOWNLOAD_DIR)
	$(call download,$(SKALIBS_URL))
	$(call download,$(EXECLINE_URL))
	$(call download,$(S6_URL))

# -----------------------------------------------------------------------------
# Extract
# -----------------------------------------------------------------------------
extract: download
	@mkdir -p $(BUILD_DIR)
	$(call extract,skalibs-$(SKALIBS_VERSION).tar.gz,skalibs)
	$(call extract,execline-$(EXECLINE_VERSION).tar.gz,execline)
	$(call extract,s6-$(S6_VERSION).tar.gz,s6)

# -----------------------------------------------------------------------------
# Build skalibs
# -----------------------------------------------------------------------------
skalibs: extract
	@echo "  BUILD     skalibs"
	@mkdir -p $(BUILD_SKALIBS)
	@cd $(BUILD_SKALIBS) && \
		$(TOOLCHAIN_BUILD)/skalibs/configure $(CONFIGURE_FLAGS) >/dev/null 2>&1 && \
		$(MAKE) -j$(JOBS) >/dev/null 2>&1 && \
		$(MAKE) DESTDIR=$(SYSROOT) install >/dev/null 2>&1
	@touch $(BUILD_SKALIBS)/.installed

# -----------------------------------------------------------------------------
# Build execline (depends on skalibs)
# -----------------------------------------------------------------------------
execline: skalibs
	@echo "  BUILD     execline"
	@mkdir -p $(BUILD_EXECLINE)
	@cd $(BUILD_EXECLINE) && \
		$(TOOLCHAIN_BUILD)/execline/configure $(CONFIGURE_FLAGS) >/dev/null 2>&1 && \
		$(MAKE) -j$(JOBS) >/dev/null 2>&1 && \
		$(MAKE) DESTDIR=$(SYSROOT) install >/dev/null 2>&1
	@touch $(BUILD_EXECLINE)/.installed

# -----------------------------------------------------------------------------
# Build s6 (depends on execline)
# -----------------------------------------------------------------------------
s6: execline
	@echo "  BUILD     s6"
	@mkdir -p $(BUILD_S6)
	@cd $(BUILD_S6) && \
		$(TOOLCHAIN_BUILD)/s6/configure $(CONFIGURE_FLAGS) >/dev/null 2>&1 && \
		$(MAKE) -j$(JOBS) >/dev/null 2>&1 && \
		$(MAKE) DESTDIR=$(SYSROOT) install >/dev/null 2>&1
	@touch $(BUILD_S6)/.installed

# -----------------------------------------------------------------------------
# Install (build all in order)
# -----------------------------------------------------------------------------
install: s6
	@echo "  INSTALL   s6 init system -> $(SYSROOT)"
	# Create basic s6 service directory structure
	@mkdir -p $(SYSROOT)/etc/s6/sv
	@mkdir -p $(SYSROOT)/etc/s6/rc.init
	@echo "  s6 init system installed."

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------
clean:
	@rm -rf $(BUILD_SKALIBS) $(BUILD_EXECLINE) $(BUILD_S6)

distclean: clean
	@rm -rf $(BUILD_DIR)/skalibs $(BUILD_DIR)/execline $(BUILD_DIR)/s6