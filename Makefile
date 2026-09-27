CC = gcc
AS = gcc
LD = ld

GCC_INCLUDE = $(shell gcc -print-file-name=include)

# x86-64 long-mode kernel. Kernel code, stacks and physical memory live below
# 4 GiB (identity mapped), so the small code model is used.
CFLAGS = -m64 -mcmodel=small -mno-red-zone -mgeneral-regs-only -std=gnu11 -O2 \
         -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none \
         -fno-asynchronous-unwind-tables -fno-builtin -fno-strict-aliasing \
         -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
         -nostdinc -isystem $(GCC_INCLUDE) -Ikernel
ASFLAGS = -m64
LDFLAGS = -m elf_x86_64 -z max-page-size=0x1000 -T linker.ld -nostdlib
UFLAGS = -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none \
         -fno-asynchronous-unwind-tables -fno-builtin -nostdinc -isystem $(GCC_INCLUDE) \
         -Ikernel -Iuser -nostdlib -static -Wl,--build-id=none -T user/user.ld

C_SOURCES = $(wildcard kernel/*.c) $(wildcard kernel/lib/*.c) \
            $(wildcard kernel/drivers/*.c) $(wildcard kernel/fs/*.c) \
            $(wildcard kernel/sh/*.c) $(wildcard kernel/net/*.c)
ASM_SOURCES = boot/boot.s kernel/isr.s kernel/gdt_flush.s kernel/user_asm.s kernel/sched_asm.s \
              kernel/fork_asm.s kernel/ap_trampoline.s kernel/progs.s

OBJS = $(C_SOURCES:.c=.o) $(ASM_SOURCES:.s=.o)

KELF = gato64.elf
BIN = gato.bin
ISO = felinos.iso
DISK = disk.img
VDISK = gatofs.img
VDISK_MB ?= 4096
SWAP = swap.img
SWAP_MB ?= 64
FATDISK = fat32.img
FATDISK_MB ?= 64

# e1000 is the NIC Gato speaks to. The 'user' backend gives the guest a
# private network reachable from the host on 10.0.2.x, with DNS forwarded to
# the host, so dhcp, dns, wget and nc work out of the box. Override with
# NETDEV_BACKEND=socket to join a real LAN instead.
NETDEV_BACKEND ?= user
QEMU_NET = -netdev $(NETDEV_BACKEND),id=net0 -device e1000,netdev=net0

# index 0 is the GatoFS root, 1 the secondary GatoFS volume, 2 the FAT32 test
# disk and 3 the swap area.
QEMU_DISKS = -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
             -drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
             -drive file=$(FATDISK),format=raw,if=ide,index=2,media=disk \
             -drive file=$(SWAP),format=raw,if=ide,index=3,media=disk

QEMU_COMMON = -m 64M $(QEMU_DISKS) $(QEMU_NET)

# The kernel is the product; the ISO is packaging. Keeping the ISO out of the
# default target means a plain `make` succeeds on hosts without GRUB's host-side
# tools, and `make run-kernel` boots the result with QEMU's -kernel. Ask for the
# ISO explicitly with `make $(ISO)`.
all: $(BIN)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) -c $< -o $@

USER_PROGS = user/hello user/cat user/crash user/spin user/count user/launch user/hello32 \
             user/nc user/wget user/mmtest
USER_LIBC = user/malloc.c user/string.c user/stdio.c user/signal.c user/net.c
USER_LIBC_HDRS = user/usys.h user/string.h user/malloc.h user/stdio.h user/stat.h \
                 user/signal.h user/net.h kernel/syscall.h

user/hello32.elf: user/hello.c user/crt0_32.s $(USER_LIBC) $(USER_LIBC_HDRS) user/user.ld
	$(CC) -m32 $(UFLAGS) -Wl,-m,elf_i386 user/crt0_32.s $(USER_LIBC) $< -o $@

user/%.elf: user/%.c user/crt0.s $(USER_LIBC) $(USER_LIBC_HDRS) user/user.ld
	$(CC) -m64 -mgeneral-regs-only $(UFLAGS) -Wl,-m,elf_x86_64 -Wl,-z,max-page-size=0x1000 user/crt0.s $(USER_LIBC) $< -o $@

kernel/progs.o: kernel/progs.s $(addsuffix .elf,$(USER_PROGS))
	$(AS) $(ASFLAGS) -I. -c $< -o $@

# GRUB's Multiboot 1 loader (and QEMU -kernel) only load ELF32, so the 64-bit
# kernel is converted; the 32-bit boot stub switches to long mode itself.
# We must explicitly set VMAs because objcopy's default conversion loses the
# 64-bit layout: the 32-bit entry point must be below 1M, and the kernel text
# must be at 1M. The --change-section-vma flags preserve the 64-bit layout.
$(KELF): $(OBJS)
	$(LD) $(LDFLAGS) -o $(KELF) $(OBJS)

$(BIN): $(KELF)
	objcopy -O elf32-i386 \
	    --change-section-vma .multiboot=0x1000 \
	    --change-section-vma .ap_handover=0x7000 \
	    --change-section-vma .ap_tramp=0x8000 \
	    --change-section-vma .boot32=0x8000 \
	    --change-section-vma .text=0x100000 \
	    --change-section-vma .rodata=0x143000 \
	    --change-section-vma .data=0x174000 \
	    --change-section-vma .bss=0x174b48 \
	    --set-start=0x80c0 \
	    $(KELF) $(BIN)

$(ISO): $(BIN) boot/grub/grub.cfg tools/mkiso.sh
	tools/mkiso.sh $(BIN) boot/grub/grub.cfg $(ISO)

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=32 status=none

$(VDISK):
	truncate -s $(VDISK_MB)M $(VDISK)

$(SWAP):
	truncate -s $(SWAP_MB)M $(SWAP)

$(FATDISK):
	truncate -s $(FATDISK_MB)M $(FATDISK)

IMAGES = $(DISK) $(VDISK) $(SWAP) $(FATDISK)

run: $(ISO) $(IMAGES)
	qemu-system-x86_64 -cdrom $(ISO) -boot d \
		-m 64M $(QEMU_NET) \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-serial stdio

# The ISO is only UEFI-bootable when the host's GRUB has no BIOS modules, which
# is the case unless grub-pc-bin (Debian) or extra/grub-bios (Arch) is
# installed. run-efi boots such an ISO with OVMF; run-kernel sidesteps the
# bootloader entirely and always works.
OVMF_CODE ?= /usr/share/edk2-ovmf/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/edk2-ovmf/OVMF_VARS_4M.fd
OVMF_VARS_COPY = iso/OVMF_VARS.fd

run-efi: $(ISO) $(IMAGES)
	@command -v qemu-system-x86_64 >/dev/null || { echo "qemu-system-x86_64 not found"; exit 1; }
	@test -f "$(OVMF_CODE)" || { echo "OVMF firmware not found at $(OVMF_CODE)"; \
		echo "override with OVMF_CODE=... OVMF_VARS=..."; exit 1; }
	@mkdir -p iso
	@cp -f "$(OVMF_VARS)" "$(OVMF_VARS_COPY)"
	qemu-system-x86_64 \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS_COPY) \
		-cdrom $(ISO) \
		-m 256M $(QEMU_NET) \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-serial stdio

run-kernel: $(BIN) $(IMAGES)
	qemu-system-x86_64 -kernel $(BIN) \
		-m 64M $(QEMU_NET) \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-serial stdio

run-serial: $(BIN) $(IMAGES)
	qemu-system-x86_64 -kernel $(BIN) \
		-m 64M $(QEMU_NET) -display none \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-serial stdio

# Boots with two cores so the SMP path, per-CPU run queues and `smp` output
# are exercised.
run-smp: $(BIN) $(IMAGES)
	qemu-system-x86_64 -kernel $(BIN) \
		-m 64M $(QEMU_NET) -smp 4 \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-serial stdio

clean:
	rm -f $(OBJS) $(KELF) $(BIN) $(ISO) user/*.elf
	rm -rf iso/boot/gato.bin iso/boot/grub/eltorito.img

distclean: clean
	rm -f $(DISK) $(VDISK) $(SWAP) $(FATDISK)

test: $(BIN) $(IMAGES)
	@echo "Running automated tests in QEMU..."
	@python3 tools/runtests.py \
		-kernel $(BIN) $(QEMU_COMMON) \
		-display none \
		-monitor none \
		-no-reboot \
		-no-shutdown

.PHONY: all run run-efi run-kernel run-serial run-smp clean distclean test
