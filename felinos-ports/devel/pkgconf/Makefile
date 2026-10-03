# pkgconf for FelinOS
# ===================

include ../../config.mk

PORT_DIR       := $(CURDIR)
PKGCONF_SRC    := $(BUILD_DIR)/pkgconf
PKGCONF_VERSION := 2.1
PKGCONF_URL    := https://distfiles.ariadne.space/pkgconf/pkgconf-$(PKGCONF_VERSION).tar.xz

CONFIGURE_FLAGS := --prefix=/usr \
                   --disable-shared \
                   --enable-static \
                   --with-sysroot=$(SYSROOT) \
                   --with-pkg-config-dir=/usr/lib/pkgconfig:/usr/share/pkgconfig \
                   CC=$(CC) CFLAGS="$(TARGET_CFLAGS)" \
                   LDFLAGS="$(TARGET_LDFLAGS)"

.PHONY: all download extract patch configure build install clean distclean

all: install

download:
	@mkdir -p $(DOWNLOAD_DIR)
	$(call download,$(PKGCONF_URL))

extract: download
	@mkdir -p $(BUILD_DIR)
	$(call extract,pkgconf-$(PKGCONF_VERSION).tar.xz,pkgconf)

patch: extract
	@echo "  PATCH     pkgconf (none needed)"

configure: patch
	@echo "  CONFIGURE pkgconf"
	@mkdir -p $(PKGCONF_SRC)
	@cd $(PKGCONF_SRC) && \
		./configure $(CONFIGURE_FLAGS) >/dev/null 2>&1
	@touch $(PKGCONF_SRC)/.configured

build: configure
	@echo "  BUILD     pkgconf"
	@cd $(PKGCONF_SRC) && $(MAKE) -j$(JOBS) >/dev/null 2>&1
	@touch $(PKGCONF_SRC)/.built

install: build
	@echo "  INSTALL   pkgconf -> $(SYSROOT)"
	@cd $(PKGCONF_SRC) && $(MAKE) DESTDIR=$(SYSROOT) install >/dev/null 2>&1
	# Create pkg-config symlink
	@ln -sf pkgconf $(SYSROOT)/usr/bin/pkg-config

clean:
	@rm -rf $(PKGCONF_SRC)

distclean: clean