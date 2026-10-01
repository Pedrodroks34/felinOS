# FelinOS Ports Configuration
# ===========================
# Configuração global para cross-compilação e build de ports

# -----------------------------------------------------------------------------
# Arquitetura e Target
# -----------------------------------------------------------------------------
ARCH            := x86_64
TARGET          := x86_64-felinos
KERNEL_VERSION  := 0.2

# -----------------------------------------------------------------------------
# Toolchain
# -----------------------------------------------------------------------------
# Prefixo do cross-compiler (será construído em toolchain/)
CROSS_PREFIX    := $(PORTS_DIR)/toolchain/bin/$(TARGET)-

CC              := $(CROSS_PREFIX)gcc
CXX             := $(CROSS_PREFIX)g++
AS              := $(CROSS_PREFIX)as
LD              := $(CROSS_PREFIX)ld
AR              := $(CROSS_PREFIX)ar
RANLIB          := $(CROSS_PREFIX)ranlib
STRIP           := $(CROSS_PREFIX)strip
OBJCOPY         := $(CROSS_PREFIX)objcopy
OBJDUMP         := $(CROSS_PREFIX)objdump
NM              := $(CROSS_PREFIX)nm
READELF         := $(CROSS_PREFIX)readelf

# Flags base para target FelinOS (sem libc, freestanding)
TARGET_CFLAGS   := -m64 -mcmodel=small -mno-red-zone -mgeneral-regs-only \
                   -std=gnu11 -O2 -pipe \
                   -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                   -fcf-protection=none -fno-asynchronous-unwind-tables \
                   -fno-builtin -fno-strict-aliasing \
                   -Wall -Wextra -Wno-unused-parameter \
                   -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
                   -nostdinc -isystem $(SYSROOT)/usr/include \
                   -I$(KERNEL_DIR)/include

TARGET_CXXFLAGS := $(TARGET_CFLAGS) -std=gnu++20 -fno-exceptions -fno-rtti

TARGET_LDFLAGS  := -nostdlib -static -Wl,--build-id=none -Wl,-z,max-page-size=0x1000

# -----------------------------------------------------------------------------
# Diretórios
# -----------------------------------------------------------------------------
PORTS_DIR       := $(CURDIR)
KERNEL_DIR      := $(PORTS_DIR)/../kernel
SYSROOT         := $(PORTS_DIR)/../userspace
BUILD_DIR       := $(PORTS_DIR)/build
DIST_DIR        := $(PORTS_DIR)/dist
PKG_DIR         := $(PORTS_DIR)/pkg
DOWNLOAD_DIR    := $(PORTS_DIR)/downloads

# -----------------------------------------------------------------------------
# Versões dos pacotes base
# -----------------------------------------------------------------------------
MUSL_VERSION    := 1.2.5
BUSYBOX_VERSION := 1.36.1
LINUX_HEADERS_VERSION := 6.6
BINUTILS_VERSION := 2.42
GCC_VERSION     := 14.1.0
GMP_VERSION     := 6.3.0
MPFR_VERSION    := 4.2.1
MPC_VERSION     := 1.3.1
ISL_VERSION     := 0.24

# -----------------------------------------------------------------------------
# Paralelismo
# -----------------------------------------------------------------------------
JOBS            ?= $(shell nproc 2>/dev/null || echo 4)

# -----------------------------------------------------------------------------
# Utilitários
# -----------------------------------------------------------------------------
WGET            := wget -c --no-check-certificate
GIT             := git
TAR             := tar
PATCH           := patch -p1
MAKE            := make -j$(JOBS)

# -----------------------------------------------------------------------------
# Funções helper
# -----------------------------------------------------------------------------
define download
	@mkdir -p $(DOWNLOAD_DIR)
	@if [ ! -f $(DOWNLOAD_DIR)/$(notdir $(1)) ]; then \
		echo "  DOWNLOAD  $(notdir $(1))"; \
		$(WGET) -O $(DOWNLOAD_DIR)/$(notdir $(1)) $(1); \
	fi
endef

define extract
	@echo "  EXTRACT   $(notdir $(1))"
	@mkdir -p $(BUILD_DIR)/$(2)
	@$(TAR) -xf $(DOWNLOAD_DIR)/$(1) -C $(BUILD_DIR)/$(2) --strip-components=1
endef

define patch_port
	@for p in $(PORT_DIR)/patches/*.patch; do \
		if [ -f "$$p" ]; then \
			echo "  PATCH     $$p"; \
			cd $(BUILD_DIR)/$(PORT_NAME) && $(PATCH) < "$$p"; \
		fi \
	done
endef

# -----------------------------------------------------------------------------
# Targets padrão
# -----------------------------------------------------------------------------
.PHONY: all toolchain base system devel clean distclean download extract patch configure build install

all: toolchain base

toolchain:
	@$(MAKE) -C toolchain

base: toolchain
	@$(MAKE) -C base/musl install
	@$(MAKE) -C base/busybox install
	@$(MAKE) -C base/felinos-base install

system: base
	@$(MAKE) -C system/s6 install

devel: base
	@$(MAKE) -C devel/make install
	@$(MAKE) -C devel/pkgconf install
	@$(MAKE) -C devel/cmake install
	@$(MAKE) -C devel/ninja install
	@$(MAKE) -C devel/git install
	@$(MAKE) -C devel/openssh install

# -----------------------------------------------------------------------------
# Limpeza
# -----------------------------------------------------------------------------
clean:
	@for d in $(TOOLCHAIN_SUBDIRS) $(BASE_SUBDIRS) $(SYSTEM_SUBDIRS) $(DEVEL_SUBDIRS); do \
		if [ -f $$d/Makefile ]; then \
			$(MAKE) -C $$d clean || true; \
		fi; \
	done
	@rm -rf $(BUILD_DIR)

distclean: clean
	@for d in $(TOOLCHAIN_SUBDIRS) $(BASE_SUBDIRS) $(SYSTEM_SUBDIRS) $(DEVEL_SUBDIRS); do \
		if [ -f $$d/Makefile ]; then \
			$(MAKE) -C $$d distclean || true; \
		fi; \
	done
	@rm -rf $(SYSROOT)/* $(DIST_DIR) $(PKG_DIR) $(DOWNLOAD_DIR)
	@rm -rf $(PORTS_DIR)/toolchain/{bin,lib,include,share}

# -----------------------------------------------------------------------------
# Sysroot
# -----------------------------------------------------------------------------
sysroot-prepare:
	@mkdir -p $(SYSROOT)/{bin,sbin,lib,lib64,usr/{bin,sbin,lib,include,share},etc/init.d,dev,proc,sys,run,tmp,var/log,root,home}
	@ln -sf lib $(SYSROOT)/lib64 2>/dev/null || true
	@ln -sf usr/bin $(SYSROOT)/bin 2>/dev/null || true
	@ln -sf usr/sbin $(SYSROOT)/sbin 2>/dev/null || true
	@ln -sf usr/lib $(SYSROOT)/lib 2>/dev/null || true

sysroot-clean:
	@rm -rf $(SYSROOT)/*

# -----------------------------------------------------------------------------
# Rootfs image generation
# -----------------------------------------------------------------------------
ROOTFS_IMG      := $(SYSROOT).img
ROOTFS_SIZE     := 256M

mkrootfs: base sysroot-prepare
	@echo "  MKROOTFS  $(ROOTFS_IMG)"
	@dd if=/dev/zero of=$(ROOTFS_IMG) bs=1 count=0 seek=$(ROOTFS_SIZE) 2>/dev/null
	@mkfs.ext4 -F -L FELINOS_ROOT $(ROOTFS_IMG) >/dev/null 2>&1
	@mkdir -p /tmp/felinos-rootfs-mnt
	@sudo mount -o loop $(ROOTFS_IMG) /tmp/felinos-rootfs-mnt
	@sudo cp -a $(SYSROOT)/* /tmp/felinos-rootfs-mnt/
	@sudo umount /tmp/felinos-rootfs-mnt
	@rmdir /tmp/felinos-rootfs-mnt
	@echo "  Rootfs pronto: $(ROOTFS_IMG)"

# -----------------------------------------------------------------------------
# QEMU test
# -----------------------------------------------------------------------------
QEMU            := qemu-system-x86_64
QEMU_FLAGS      := -m 256M -smp 4 -serial stdio -display none \
                   -kernel $(KERNEL_DIR)/../gato.bin \
                   -drive file=$(ROOTFS_IMG),format=raw,if=virtio \
                   -netdev user,id=net0 -device virtio-net-pci,netdev=net0

test-rootfs: mkrootfs
	@$(QEMU) $(QEMU_FLAGS)

# -----------------------------------------------------------------------------
# Informação
# -----------------------------------------------------------------------------
info:
	@echo "FelinOS Ports Build System"
	@echo "=========================="
	@echo "TARGET:        $(TARGET)"
	@echo "ARCH:          $(ARCH)"
	@echo "KERNEL_DIR:    $(KERNEL_DIR)"
	@echo "SYSROOT:       $(SYSROOT)"
	@echo "BUILD_DIR:     $(BUILD_DIR)"
	@echo "PORTS_DIR:     $(PORTS_DIR)"
	@echo "JOBS:          $(JOBS)"
	@echo ""
	@echo "Targets disponíveis:"
	@echo "  make toolchain    - Cross-compiler + headers"
	@echo "  make base         - musl + busybox + base layout"
	@echo "  make system       - s6/openrc init system"
	@echo "  make devel        - make, pkgconf, cmake, ninja, git, ssh"
	@echo "  make full         - Tudo acima"
	@echo "  make mkrootfs     - Gera imagem rootfs.ext4"
	@echo "  make test-rootfs  - Boota QEMU com rootfs"
	@echo "  make clean        - Limpa build dirs"
	@echo "  make distclean    - Limpa tudo (inclui sysroot)"
	@echo "  make info         - Esta informação"