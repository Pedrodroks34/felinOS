# FelinOS Toolchain Build
# =======================
# Constrói cross-compiler GCC + binutils + linux headers + musl

include ../config.mk

# Versões
BINUTILS_VERSION := 2.42
GCC_VERSION      := 14.1.0
GMP_VERSION      := 6.3.0
MPFR_VERSION     := 4.2.1
MPC_VERSION      := 1.3.1
ISL_VERSION      := 0.25

# URLs
BINUTILS_URL     := https://ftp.gnu.org/gnu/binutils/binutils-$(BINUTILS_VERSION).tar.xz
GCC_URL          := https://ftp.gnu.org/gnu/gcc/gcc-$(GCC_VERSION)/gcc-$(GCC_VERSION).tar.xz
GMP_URL          := https://ftp.gnu.org/gnu/gmp/gmp-$(GMP_VERSION).tar.xz
MPFR_URL         := https://ftp.gnu.org/gnu/mpfr/mpfr-$(MPFR_VERSION).tar.xz
MPC_URL          := https://ftp.gnu.org/gnu/mpc/mpc-$(MPC_VERSION).tar.gz
ISL_URL          := https://ftp.gnu.org/gnu/isl/isl-$(ISL_VERSION).tar.xz
LINUX_HEADERS_URL := https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$(LINUX_HEADERS_VERSION).tar.xz

# Diretórios de build
TOOLCHAIN_BUILD  := $(BUILD_DIR)/toolchain
TOOLCHAIN_PREFIX := $(PORTS_DIR)/toolchain
SYSROOT_HEADERS  := $(SYSROOT)/usr/include

# Flags para build do GCC
GCC_BOOTSTRAP_FLAGS := --target=$(TARGET) --prefix=$(TOOLCHAIN_PREFIX) \
                       --with-sysroot=$(SYSROOT) \
                       --disable-nls --disable-shared --disable-threads \
                       --disable-libatomic --disable-libgomp --disable-libquadmath \
                       --disable-libssp --disable-libvtv --disable-libstdcxx \
                       --enable-languages=c,c++ --without-headers \
                       --with-newlib --disable-multilib

GCC_FINAL_FLAGS := --target=$(TARGET) --prefix=$(TOOLCHAIN_PREFIX) \
                   --with-sysroot=$(SYSROOT) \
                   --disable-nls --enable-threads=posix \
                   --enable-languages=c,c++ \
                   --disable-libatomic --disable-libgomp --disable-libquadmath \
                   --disable-libssp --disable-libvtv \
                   --disable-multilib \
                   --with-system-zlib

.PHONY: build download extract patch configure build-linux-headers build-binutils build-gcc-bootstrap build-musl-headers build-gcc-final build-all clean distclean

# Default target - build everything
build: build-all

build-all: build-linux-headers build-binutils build-gcc-bootstrap build-musl-headers build-gcc-final
	@echo "  Toolchain build complete!"
	@$(TOOLCHAIN_PREFIX)/bin/$(TARGET)-gcc --version | head -1

# -----------------------------------------------------------------------------
# Download
# -----------------------------------------------------------------------------
download:
	@mkdir -p $(DOWNLOAD_DIR)
	$(call download,$(BINUTILS_URL))
	$(call download,$(GCC_URL))
	$(call download,$(GMP_URL))
	$(call download,$(MPFR_URL))
	$(call download,$(MPC_URL))
	$(call download,$(ISL_URL))
	$(call download,$(LINUX_HEADERS_URL))

# -----------------------------------------------------------------------------
# Extract
# -----------------------------------------------------------------------------
extract: download
	@mkdir -p $(TOOLCHAIN_BUILD)
	$(call extract,binutils-$(BINUTILS_VERSION).tar.xz,binutils)
	$(call extract,gcc-$(GCC_VERSION).tar.xz,gcc)
	$(call extract,gmp-$(GMP_VERSION).tar.xz,gmp)
	$(call extract,mpfr-$(MPFR_VERSION).tar.xz,mpfr)
	$(call extract,mpc-$(MPC_VERSION).tar.gz,mpc)
	$(call extract,isl-$(ISL_VERSION).tar.xz,isl)
	$(call extract,linux-$(LINUX_HEADERS_VERSION).tar.xz,linux-headers)
	# Link dependencies into gcc source tree
	@ln -sf $(TOOLCHAIN_BUILD)/gmp $(TOOLCHAIN_BUILD)/gcc/gmp
	@ln -sf $(TOOLCHAIN_BUILD)/mpfr $(TOOLCHAIN_BUILD)/gcc/mpfr
	@ln -sf $(TOOLCHAIN_BUILD)/mpc $(TOOLCHAIN_BUILD)/gcc/mpc
	@ln -sf $(TOOLCHAIN_BUILD)/isl $(TOOLCHAIN_BUILD)/gcc/isl

# -----------------------------------------------------------------------------
# Linux Headers (UAPI) para sysroot
# -----------------------------------------------------------------------------
build-linux-headers: extract
	@echo "  INSTALL   linux-headers (UAPI)"
	@mkdir -p $(SYSROOT_HEADERS)
	@cd $(TOOLCHAIN_BUILD)/linux-headers && \
		$(MAKE) ARCH=x86 INSTALL_HDR_PATH=$(SYSROOT)/usr headers_install >/dev/null 2>&1
	# Fix: FelinOS usa syscalls diferentes, remover headers conflitantes
	@rm -f $(SYSROOT)/usr/include/asm/unistd*.h $(SYSROOT)/usr/include/asm-generic/unistd*.h
	@rm -f $(SYSROOT)/usr/include/linux/syscalls.h
	# Criar headers FelinOS específicos
	@mkdir -p $(SYSROOT)/usr/include/felinos
	@cp -r $(KERNEL_DIR)/include/* $(SYSROOT)/usr/include/felinos/ 2>/dev/null || true
	@touch $(TOOLCHAIN_BUILD)/.linux-headers-done

# -----------------------------------------------------------------------------
# Binutils
# -----------------------------------------------------------------------------
build-binutils: extract build-linux-headers
	@echo "  BUILD     binutils"
	@mkdir -p $(TOOLCHAIN_BUILD)/build-binutils
	@cd $(TOOLCHAIN_BUILD)/build-binutils && \
		$(TOOLCHAIN_BUILD)/binutils/configure \
			--target=$(TARGET) \
			--prefix=$(TOOLCHAIN_PREFIX) \
			--with-sysroot=$(SYSROOT) \
			--disable-nls --disable-werror \
			--enable-gold --enable-plugins \
			--disable-multilib >/dev/null 2>&1 && \
		$(MAKE) -j$(JOBS) >/dev/null 2>&1 && \
		$(MAKE) install >/dev/null 2>&1
	@touch $(TOOLCHAIN_BUILD)/.binutils-done

# -----------------------------------------------------------------------------
# GCC Bootstrap (sem libc)
# -----------------------------------------------------------------------------
build-gcc-bootstrap: build-binutils build-linux-headers
	@echo "  BUILD     gcc (bootstrap)"
	@mkdir -p $(TOOLCHAIN_BUILD)/build-gcc-bootstrap
	@cd $(TOOLCHAIN_BUILD)/build-gcc-bootstrap && \
		$(TOOLCHAIN_BUILD)/gcc/configure $(GCC_BOOTSTRAP_FLAGS) >/dev/null 2>&1 && \
		$(MAKE) -j$(JOBS) all-gcc >/dev/null 2>&1 && \
		$(MAKE) install-gcc >/dev/null 2>&1
	@touch $(TOOLCHAIN_BUILD)/.gcc-bootstrap-done

# -----------------------------------------------------------------------------
# Musl headers (construído separadamente em base/musl, mas headers necessários aqui)
# -----------------------------------------------------------------------------
build-musl-headers: build-gcc-bootstrap
	@echo "  Musl headers instalados via base/musl"
	@touch $(TOOLCHAIN_BUILD)/.musl-headers-done

# -----------------------------------------------------------------------------
# GCC Final (com musl)
# -----------------------------------------------------------------------------
build-gcc-final: build-musl-headers
	@echo "  BUILD     gcc (final com musl)"
	@mkdir -p $(TOOLCHAIN_BUILD)/build-gcc-final
	@cd $(TOOLCHAIN_BUILD)/build-gcc-final && \
		$(TOOLCHAIN_BUILD)/gcc/configure $(GCC_FINAL_FLAGS) >/dev/null 2>&1 && \
		$(MAKE) -j$(JOBS) >/dev/null 2>&1 && \
		$(MAKE) install >/dev/null 2>&1
	@touch $(TOOLCHAIN_BUILD)/.gcc-final-done

# -----------------------------------------------------------------------------
# Clean
# -----------------------------------------------------------------------------
clean:
	@rm -rf $(TOOLCHAIN_BUILD)/build-*

distclean:
	@rm -rf $(TOOLCHAIN_BUILD)
	@rm -rf $(TOOLCHAIN_PREFIX)