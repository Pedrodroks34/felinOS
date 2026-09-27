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
ASM_SOURCES = boot/boot.s kernel/isr.s kernel/gdt_flush.s kernel/user_asm.s kernel/sched_asm.s kernel/fork_asm.s kernel/progs.s

OBJS = $(C_SOURCES:.c=.o) $(ASM_SOURCES:.s=.o)

KELF = gato64.elf
BIN = gato.bin
ISO = felinos.iso
DISK = disk.img
VDISK = gatofs.img
VDISK_MB ?= 4096
SWAP = swap.img
SWAP_MB ?= 64

all: $(ISO)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) -c $< -o $@

USER_PROGS = user/hello user/cat user/crash user/spin user/count user/launch user/hello32
USER_LIBC = user/malloc.c user/string.c user/stdio.c user/signal.c
USER_LIBC_HDRS = user/usys.h user/string.h user/malloc.h user/stdio.h user/stat.h user/signal.h kernel/syscall.h

user/hello32.elf: user/hello.c user/crt0_32.s $(USER_LIBC) $(USER_LIBC_HDRS) user/user.ld
	$(CC) -m32 $(UFLAGS) -Wl,-m,elf_i386 user/crt0_32.s $(USER_LIBC) $< -o $@

user/%.elf: user/%.c user/crt0.s $(USER_LIBC) $(USER_LIBC_HDRS) user/user.ld
	$(CC) -m64 -mgeneral-regs-only $(UFLAGS) -Wl,-m,elf_x86_64 -Wl,-z,max-page-size=0x1000 user/crt0.s $(USER_LIBC) $< -o $@

kernel/progs.o: kernel/progs.s $(addsuffix .elf,$(USER_PROGS))
	$(AS) $(ASFLAGS) -I. -c $< -o $@

# GRUB's Multiboot 1 loader (and QEMU -kernel) only load ELF32, so the 64-bit
# kernel is converted; the 32-bit boot stub switches to long mode itself.
$(KELF): $(OBJS)
	$(LD) $(LDFLAGS) -o $(KELF) $(OBJS)

$(BIN): $(KELF)
	objcopy -O elf32-i386 $(KELF) $(BIN)

$(ISO): $(BIN) boot/grub/grub.cfg
	mkdir -p iso/boot/grub
	cp $(BIN) iso/boot/gato.bin
	cp boot/grub/grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso

$(DISK):
	dd if=/dev/zero of=$(DISK) bs=1M count=32 status=none

$(VDISK):
	truncate -s $(VDISK_MB)M $(VDISK)

$(SWAP):
	truncate -s $(SWAP_MB)M $(SWAP)

run: $(ISO) $(DISK) $(VDISK) $(SWAP)
	qemu-system-x86_64 -cdrom $(ISO) -m 64M -boot d \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-serial stdio

run-kernel: $(BIN) $(DISK) $(VDISK) $(SWAP)
	qemu-system-x86_64 -kernel $(BIN) -m 64M \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-serial stdio

run-serial: $(BIN) $(DISK) $(VDISK) $(SWAP)
	qemu-system-x86_64 -kernel $(BIN) -m 64M \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-display none -serial stdio

clean:
	rm -f $(OBJS) $(KELF) $(BIN) $(ISO) user/*.elf
	rm -rf iso/boot/gato.bin

distclean: clean
	rm -f $(DISK) $(VDISK) $(SWAP)

test: $(BIN) $(DISK) $(VDISK) $(SWAP)
	@echo "Running automated tests in QEMU..."
	@qemu-system-x86_64 -kernel $(BIN) -m 64M \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(VDISK),format=raw,if=ide,index=1,media=disk \
		-drive file=$(SWAP),format=raw,if=ide,index=3,media=disk \
		-display none -serial stdio \
		-append "test" \
		-monitor none \
		-no-reboot \
		-watchdog-action reset

.PHONY: all run run-kernel run-serial clean distclean test
