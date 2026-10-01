# FelinOS Ports System

## Overview

The `felinos-ports/` directory contains a BSD-style ports tree for building userspace applications for FelinOS using musl libc.

## Structure

```
felinos-ports/
├── config.mk              # Global configuration
├── Makefile               # Main entry point
├── toolchain/             # Cross-compiler toolchain
│   ├── Makefile           # Builds binutils + GCC + Linux headers
│   └── patches/           # GCC patches if needed
├── base/                  # Base system
│   ├── musl/              # musl libc
│   │   ├── Makefile
│   │   └── patches/       # FelinOS-specific patches
│   ├── busybox/           # BusyBox (ash + coreutils)
│   │   ├── Makefile
│   │   ├── felinos_defconfig  # BusyBox config
│   │   └── patches/       # BusyBox patches
│   └── felinos-base/      # Base filesystem layout
│       └── Makefile       # Creates /etc, /dev, init scripts, etc.
├── system/                # Init systems
│   ├── s6/                # s6 supervision suite
│   ├── openrc/            # OpenRC init
│   └── systemd/           # systemd (minimal)
├── devel/                 # Development tools
│   ├── make/              # GNU Make
│   ├── pkgconf/           # pkg-config
│   ├── cmake/             # CMake
│   ├── ninja/             # Ninja
│   ├── git/               # Git
│   └── openssh/           # OpenSSH/Dropbear
└── pkg/                   # Binary package cache
```

## Quick Start

```bash
# 1. Build the cross-compiler toolchain
make -C felinos-ports toolchain

# 2. Build base system (musl + busybox + base layout)
make -C felinos-ports base

# 3. Build init system (s6)
make -C felinos-ports system

# 4. Build development tools
make -C felinos-ports devel

# 5. Generate rootfs image
make -C felinos-ports mkrootfs

# 6. Test in QEMU
make -C felinos-ports test-rootfs
```

Or from the main FelinOS directory:

```bash
# Build everything
make ports-full

# Generate and test rootfs
make ports-mkrootfs
make ports-test
```

## Cross-Compilation Environment

For building additional packages manually:

```bash
source tools/cross-env.sh

# Now you can use the cross-compiler
$CC -o hello hello.c
$CC $CFLAGS -o myprog myprog.c

# Use with meson
meson setup builddir --cross-file $CROSS_FILE
meson compile -C builddir
meson install -C builddir --destdir $SYSROOT
```

## Adding New Ports

1. Create directory under appropriate category:
   ```
   felinos-ports/devel/mytool/
   ├── Makefile
   └── patches/
   ```

2. Follow the Makefile pattern:
   ```make
   include ../../config.mk
   
   PORT_DIR := $(CURDIR)
   MYTOOL_VERSION := 1.0
   MYTOOL_URL := https://example.com/mytool-$(MYTOOL_VERSION).tar.gz
   
   .PHONY: all download extract patch configure build install clean distclean
   
   all: install
   
   download:
       @mkdir -p $(DOWNLOAD_DIR)
       $(call download,$(MYTOOL_URL))
   
   # ... etc
   ```

3. Add to parent Makefile's SUBDIRS list

## musl Patches for FelinOS

The `base/musl/patches/` directory contains three patches:

1. **0001-felinos-syscall-numbers.patch** - Adds FelinOS syscall numbers to musl's syscall.h
2. **0002-felinos-syscall-abi.patch** - Overrides syscall assembly for `int 0x80` ABI
3. **0003-felinos-kernel-structs.patch** - Adapts kernel structures (stat, dirent, fcntl, socket)

## BusyBox Configuration

The `base/busybox/felinos_defconfig` enables:
- ash shell with job control, aliases, math
- Core utilities (ls, cp, mv, rm, mkdir, etc.)
- Network utilities (wget, udhcpc, ping, nc)
- System utilities (mount, fdisk, mdev, syslogd)
- Process utilities (ps, kill, top, free)
- Editor (vi)
- Init system support

## Base Filesystem Layout

The `base/felinos-base/Makefile` creates:
- `/etc/passwd`, `/etc/group`, `/etc/shadow`
- `/etc/fstab`, `/etc/inittab`, `/etc/rcS`, `/etc/rcK`
- `/etc/profile`, `/etc/hostname`, `/etc/hosts`
- `/etc/mdev.conf`, `/etc/resolv.conf`
- `/etc/os-release`, `/etc/issue`, `/etc/motd`
- `/etc/ssh/sshd_config` (placeholder)
- `/root/.profile`, `/home/felinos/.profile`
- `/var/log/wtmp`, `/var/log/btmp`, `/var/log/lastlog`

## Toolchain Details

The toolchain builds:
1. **binutils** 2.42 - Assembler, linker, objdump, etc.
2. **GCC 14.1.0** - Bootstrap (without libc) then final (with musl)
3. **GMP/MPFR/MPC/ISL** - GCC dependencies
4. **Linux headers** 6.6 - UAPI headers (filtered for FelinOS)

Target: `x86_64-felinos`
Sysroot: `../userspace/`
Prefix: `../felinos-ports/toolchain/`

## Requirements

Host system needs:
- gcc, g++, make, binutils
- wget, tar, xz, bzip2, gzip
- python3 (for test scripts)
- sudo (for rootfs image creation)

Debian/Ubuntu:
```bash
apt install build-essential wget tar xz-utils bzip2 gzip python3 sudo
```

Arch:
```bash
pacman -S base-devel wget tar xz bzip2 gzip python sudo
```

## Troubleshooting

### Toolchain build fails
- Check host gcc version (needs 10+)
- Ensure all dependencies installed
- Try `make -C felinos-ports/toolchain clean` then rebuild

### musl build fails
- Check patches apply correctly
- Verify kernel headers in sysroot

### busybox build fails
- Check config options are compatible with musl
- Some applets may need kernel features not yet implemented

### QEMU test fails
- Ensure KVM available: `ls /dev/kvm`
- Check OVMF firmware for UEFI: `ls /usr/share/edk2-ovmf/`

## License

All ports follow their upstream licenses:
- musl: MIT
- BusyBox: GPLv2
- Toolchain components: GPLv3/GPLv2
- FelinOS-specific code: Same as FelinOS kernel