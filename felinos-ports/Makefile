# FelinOS Ports - Main Makefile
# ==============================

include config.mk

# Subdiretórios com ports (ordem importa para dependências)
TOOLCHAIN_SUBDIRS := toolchain
BASE_SUBDIRS      := base/musl base/busybox base/felinos-base
SYSTEM_SUBDIRS    := system/s6 system/openrc
DEVEL_SUBDIRS     := devel/make devel/pkgconf devel/cmake devel/ninja devel/git devel/openssh

.PHONY: all toolchain base system devel clean distclean \
        $(TOOLCHAIN_SUBDIRS) $(BASE_SUBDIRS) $(SYSTEM_SUBDIRS) $(DEVEL_SUBDIRS) \
        download extract patch configure build install sysroot-prepare sysroot-clean \
        mkrootfs test-rootfs info

# Default target
all: toolchain base

# -----------------------------------------------------------------------------
# Toolchain: cross-compiler + linux headers + musl
# -----------------------------------------------------------------------------
toolchain:
	@$(MAKE) -C toolchain build

# -----------------------------------------------------------------------------
# Base system: musl libc + busybox + felinos base layout
# -----------------------------------------------------------------------------
base: toolchain $(BASE_SUBDIRS)

$(BASE_SUBDIRS):
	@$(MAKE) -C $@

# -----------------------------------------------------------------------------
# System services: init system, supervision
# -----------------------------------------------------------------------------
system: base $(SYSTEM_SUBDIRS)

$(SYSTEM_SUBDIRS):
	@$(MAKE) -C $@

# -----------------------------------------------------------------------------
# Development tools
# -----------------------------------------------------------------------------
devel: base $(DEVEL_SUBDIRS)

$(DEVEL_SUBDIRS):
	@$(MAKE) -C $@

# -----------------------------------------------------------------------------
# Full build: tudo
# -----------------------------------------------------------------------------
full: toolchain base system devel

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
# Download de fontes (para bootstrap offline)
# -----------------------------------------------------------------------------
download-sources:
	@$(MAKE) -C toolchain download
	@$(MAKE) -C base/musl download
	@$(MAKE) -C base/busybox download
	@$(MAKE) -C system/s6 download
	@$(MAKE) -C devel/make download
	@$(MAKE) -C devel/pkgconf download
	@$(MAKE) -C devel/cmake download
	@$(MAKE) -C devel/ninja download
	@$(MAKE) -C devel/git download
	@$(MAKE) -C devel/openssh download

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

.PHONY: all toolchain base system devel clean distclean \
        $(TOOLCHAIN_SUBDIRS) $(BASE_SUBDIRS) $(SYSTEM_SUBDIRS) $(DEVEL_SUBDIRS) \
        download extract patch configure build install sysroot-prepare sysroot-clean \
        mkrootfs test-rootfs info