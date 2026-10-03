# Repository Instructions

FelinOS: from-scratch x86-64 OS (kernel "Gato"), freestanding C + AT&T asm. No libc, no Linux/BSD code. Run all commands from the repo root.

## Commands

- `make` builds `gato.bin` (default target; needs only gcc, binutils, make). The ISO is not part of it.
- `make test` runs `tools/runtests.py`: boots `gato.bin` headless in QEMU, types `selftest` on COM1, powers off. Exit codes: 0 pass, 1 failure, 2 panic or no prompt, 124 timeout. Needs `qemu-system-x86_64` and `python3`; first boot formats a 4 GB sparse disk, so allow up to 300 s.
- `make run-kernel` / `make run-serial` / `make run-smp` boot `gato.bin` directly via QEMU `-kernel` (no GRUB, needs the PVH note in `boot/boot.s`).
- `make felinos.iso` / `make run` need `grub-mkrescue`, `xorriso`, `mtools` and BIOS GRUB modules (`grub-pc-bin`); see `tools/mkiso.sh`. Without the BIOS modules the ISO is UEFI-only and `make run` will not boot it (`make run-efi` will).
- `make clean` removes objects/ELFs/ISO; `make distclean` also deletes `disk.img`, `gatofs.img`, `swap.img`, `fat32.img`.
- Disk image sizes and NIC are Makefile variables: `VDISK_MB`, `SWAP_MB`, `FATDISK_MB`, `NETDEV_BACKEND`, `NIC_MODEL`.

## Build gotchas

- Objects have no header dependency tracking (`%.o` depends only on its `.c`/`.s`). After editing any `.h` (especially `kernel/syscall.h`, shared with `user/`), run `make clean && make`.
- Kernel sources are picked up by wildcard from `kernel/{,lib,drivers,fs,sh,net,mm}/*.c`. Files in `kernel/mm/*.disabled` are intentionally not built.
- Kernel is built `-mgeneral-regs-only -ffreestanding -nostdinc`: no FPU/SSE, no host headers or libc. Use `kernel/lib/` (string, format, heap).
- The build is currently warning-free under `-Wall -Wextra`; keep it that way.
- `gato64.elf` is converted with `objcopy -O elf32-i386` into `gato.bin` (Multiboot 1 / PVH both need ELF32). Do not hand-set section VMAs in that rule.
- `linker.ld` has fixed addresses: handover block at `0x7000`, AP trampoline (`kernel/ap_trampoline.s`) at `0x8000`, kernel at 1 MB. Moving these breaks SMP bring-up.

## Adding a user program

Write `user/x.c`, then add it in three places: `USER_PROGS` in `Makefile`, a `PROG x` line in `kernel/progs.s`, and `PROG_DECL(x)` plus a table entry in `kernel/user.c`. Programs are static ELFs embedded into the kernel and installed at `/bin` on boot.

## Testing

- The suite lives in `kernel/sh/test.c` and runs inside the kernel as `selftest`. `tools/runtests.py` parses the line `Checks: N  Passed: N  Failed: N`; keep that format if you touch the summary output.
- `test` (shell condition evaluator) and `selftest` (suite) are different commands; do not merge them.

## Docs that are stale

- `README.md` mentions a CI workflow and a `Total:` summary line; there is no `.github/` directory and the real line is `Checks:`.
- `felinos-ports/README.md` references `system/`, `devel/`, `pkg/` dirs and `make ports-*` targets that do not exist in the repo or root `Makefile`. `felinos-ports/` is a separate tree and is not part of the kernel build.
- `BUGS_FOUND.md` is a historical log, not current state.
