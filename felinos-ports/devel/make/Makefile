# GNU Make for FelinOS
# ====================

include ../../config.mk

PORT_DIR     := $(CURDIR)
MAKE_SRC     := $(BUILD_DIR)/make
MAKE_VERSION := 4.4
MAKE_URL     := https://ftp.gnu.org/gnu/make/make-$(MAKE_VERSION).tar.gz

CONFIGURE_FLAGS := --prefix=/usr \
                   --disable-nls \
                   --without-guile \
                   --host=$(TARGET) \
                   CC=$(CC) CFLAGS="$(TARGET_CFLAGS)" \
                   LDFLAGS="$(TARGET_LDFLAGS)"

.PHONY: all download extract patch configure build install clean distclean

all: install

download:
	@mkdir -p $(DOWNLOAD_DIR)
	$(call download,$(MAKE_URL))

extract: download
	@mkdir -p $(BUILD_DIR)
	$(call extract,make-$(MAKE_VERSION).tar.gz,make)

patch: extract
	@echo "  PATCH     make (none needed)"

configure: patch
	@echo "  CONFIGURE make"
	@mkdir -p $(MAKE_SRC)
	@cd $(MAKE_SRC) && \
		./configure $(CONFIGURE_FLAGS) >/dev/null 2>&1
	@touch $(MAKE_SRC)/.configured

build: configure
	@echo "  BUILD     make"
	@cd $(MAKE_SRC) && $(MAKE) -j$(JOBS) >/dev/null 2>&1
	@touch $(MAKE_SRC)/.built

install: build
	@echo "  INSTALL   make -> $(SYSROOT)"
	@cd $(MAKE_SRC) && $(MAKE) DESTDIR=$(SYSROOT) install >/dev/null 2>&1

clean:
	@rm -rf $(MAKE_SRC)

distclean: clean