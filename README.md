# FelinOS

FelinOS is a 64-bit (x86-64, long mode) operating system for the x86 PC, built around its own
kernel called **Gato**. It is not based on Linux, BSD or any existing
kernel: the bootstrap code, the drivers, the filesystem and the shell are
all written from scratch in C and AT&T assembly.

Version: **0.2** — Kernel: **Gato 0.2** — Architecture: **x86_64**

## What FelinOS can do

Gato boots from a Multiboot-compliant bootloader (GRUB), sets up segmentation
and paging, installs a full interrupt table, brings up the hardware it finds
on the machine, builds its file tree from a mount table and drops the user into an
interactive shell with 90 commands.

Captured from `qemu-system-x86_64 -kernel gato.bin -m 64M`, so it is one
processor, no disks and no DHCP lease. `make run` reports the disks and the
extra processors; the order of the lines is the same.

```
FelinOS 0.2  -  Gato kernel 0.2 (x86_64)

  [ok] global descriptors     flat 64-bit code and data segments
  [ok] interrupt table        32 exceptions, 16 hardware IRQs
  [ok] memory map             32384 KB reported by the bootloader
  [ok] physical memory        8192 frames (32 MB), 7688 free
  [ok] paging                 32 MB mapped, 16 tables, directory at 0x001f8000
  [ok] virtual memory         demand paging, copy-on-write, 5 regions
  [ok] kernel heap            22484 KB demand-paged heap at 0xc0000000
  [ok] apic                   local APIC at 0xfee00000, 1 CPU(s)
  [ok] system timer           PIT channel 0 at 100 Hz
  [ok] scheduler              preemptive round-robin, 50 ms time slices
  [ok] keyboard               PS/2 set 1, shift, ctrl and caps lock
  [ok] framebuffer            320x200 256-color at 0xA0000, inactive (VGA text still active)
  [ok] font renderer          8x16 bitmap font for framebuffer
  [ok] serial console         COM1 at 115200 baud, input and output
  [ok] real time clock        2026-09-27 05:05:54
  [ok] processor              AuthenticAMD family 15 model 107
  [ok] pci bus                6 devices on the bus
  [ok] network                e1000 driver, mac 52:54:00:12:34:56, run 'dhcp' or 'ifconfig' to configure
  [ok] acpi                   rev 0, 5 tables, S5 via PM1a 0x604, reset legacy
  [ok] buffer cache           4 MB, 1024 lines of 4 KB over the ATA layer
  [ok] ata controller         1 device(s), first is QEMU DVD-ROM
  [ok] ahci controller        no SATA devices
  [ok] fat32                  FAT32 reader ready for host file exchange
  [ok] swap                   no swap area (mkswap <disk>)
  [ok] virtual filesystem     / is ramfs (none), /dev, /proc and /tmp mounted
  [ok] gatofs                 no volume (gatofs format <disk>)
  [ok] user mode              ring 3, per-process address spaces, int 0x80 syscalls, ELF loader
  [ok] shell                  vsh with pipes, redirection and history

Welcome to FelinOS running the Gato kernel.
Type 'help' for the command list, 'man <command>' for details.

root@felinos:/root$
```

## Building

Requirements: `gcc` (x86-64; `-m32` only for the `hello32` demo), `binutils`
and `make`. None of that is needed to boot: the kernel boots straight from its
own ELF, with no bootloader in the path.

The ISO is packaging, not the product, so it is a separate target and needs
extra host tools: `grub-mkrescue`, `xorriso` and `mtools`, plus GRUB's BIOS
modules (`grub-pc-bin` on Debian/Ubuntu, `extra/grub-bios` on Arch). Without
the BIOS modules the ISO still builds but only boots under UEFI firmware,
because GRUB emits one El Torito entry per platform it has modules for; the
build says so instead of leaving you with an unbootable CD.

```sh
make              # builds the kernel: gato.bin
make felinos.iso  # builds the bootable ISO, if the host tools are present
make clean        # removes objects, the kernel and the iso
make distclean    # also removes the virtual disk images
```

The kernel and the user programs build without warnings; the CI workflow
fails the build if that ever stops being true.

## Running

```sh
make run        # boots felinos.iso through GRUB in QEMU
make run-efi    # same, but under OVMF, for a UEFI-only ISO
make run-kernel # boots gato.bin directly, skipping the bootloader
make run-serial # same, but headless: the whole session goes over COM1
make run-smp    # boots with 4 processors, so the AP bring-up path runs
make test       # runs the in-kernel test suite and exits with its result
```

`make run-kernel` and `make test` need no ISO and no GRUB, so they work on a
host that has nothing but a compiler and QEMU. Getting the kernel to load that
way is the reason the kernel carries a Xen PVH note (`XEN_ELFNOTE_PHYS32_ENTRY`
in `boot/boot.s`): QEMU 9.1 dropped Multiboot support in `-kernel` and now
refuses anything that is neither a `bzImage` nor an ELF with that note, with
"Error loading uncompressed kernel without PVH ELF Note".

One difference worth knowing: `-kernel` gives the kernel no bootloader memory
map, so it falls back to a fixed 32 MB estimate and sizes the PMM from that
(see [Virtual memory](#virtual-memory-and-swap)). Through GRUB the real map
is used. `make run` and `make run-kernel` therefore do not exercise quite the
same memory path.

`make run` attaches four IDE disks so that every storage path has something
to work with: `disk.img` (GatoFS root), `gatofs.img` (a second GatoFS
volume), `fat32.img` (blank, 64 MB) and `swap.img`. Their sizes are
`VDISK_MB`, `SWAP_MB` and `FATDISK_MB`; the images are created on first use.
`make run NETDEV_BACKEND=none` drops the network card, and `NETDEV_BACKEND` is
also how the DHCP and name-resolution commands get their default gateway.

The console is mirrored to the VGA text screen and to COM1 at 115200 baud,
and keyboard input is merged with serial input, including ANSI arrow keys.
The shell and the editor are therefore fully usable over a serial line with
no display attached.

Writing the ISO to a USB stick and booting a real PC works as well; Gato
only requires a Multiboot loader, a VGA text mode and a PS/2 keyboard.

## Architecture

```
boot/boot.s          Multiboot header, stack setup, entry into kernel_main
linker.ld            AP trampoline at 0x8000, handover block at 0x7000,
                     kernel loaded at 1 MB

kernel/
  kernel.c           boot sequence and subsystem initialisation
  console.c          VGA + serial output, kprintf, kernel log ring buffer
  system.c           multiboot parsing, memory layout, version information
  gdt.c  idt.c       segmentation, 32 exceptions, 16 IRQs, APIC vectors, panic
  gdt_flush.s        lgdt plus far return, so a GDT can be reloaded in place
  pmm.c              physical frame bitmap allocator with reference counts
  paging.c           page directories/tables, address spaces, page faults
  vmm.c              virtual memory manager: regions, demand paging, COW
  swap.c             disk-backed swap area, page-out/page-in
  bcache.c           block cache between the filesystems and the ATA driver
  sched.c            preemptive round-robin scheduler, tasks, sleep/wait/kill
  sched_asm.s        the kernel stack switch (switch_context)
  fpu.c              per-task FPU/SSE state, fxsave64/fxrstor64
  fork_asm.s         the ring 3 return path used by fork
  user.c             ring 3 processes, ELF loader, syscalls
  user_asm.s         the syscall entry and return stubs
  isr.s              one stub per exception, IRQ and APIC vector
  ap_trampoline.s    real mode -> long mode entry code for application CPUs

  lib/
    string.c         mem*/str* routines, ctype helpers, number conversion
    format.c         printf-style formatter, vsnprintf and snprintf
    heap.c           first-fit allocator with block splitting and coalescing

  drivers/
    vga.c            text mode, colours, hardware cursor, scrolling
    serial.c         COM1 at 115200 baud
    screen.c         full-screen abstraction over VGA and ANSI terminals
    input.c          merges keyboard and serial, parses escape sequences
    keyboard.c       PS/2 scancode set 1, modifiers, extended keys, F1-F10
    font.c fb.c      8x16 bitmap font and linear framebuffer output
    pic.c            8259 remapping and masking
    pit.c            100 Hz timer, uptime, sleep
    rtc.c            CMOS clock, unix time, calendar arithmetic
    ata.c            ATA PIO: IDENTIFY, LBA28/48 read/write, MBR partitions,
                     interrupt-driven once the scheduler is up (IRQ14/15)
    ahci.c           AHCI/SATA: port setup, command lists, FIS handling
    pci.c            configuration space scan, vendor and class names
    cpu.c            CPUID vendor, brand, family and feature flags
    apic.c           local APIC, I/O APIC, MADT, INIT/SIPI bring-up of the APs
    acpi.c           RSDP/RSDT/XSDT/FADT/MADT parser, _S5_, ACPI reset register
    e1000.c          Intel e1000 NIC: descriptors, TX/RX rings
    power.c          reboot, power off, halt (ACPI first, legacy ports as fallback)
    speaker.c        PC speaker tones

  net/
    eth.c netif.c    frame send/receive, the interface table, checksum offload
    arp.c ip.c       address resolution and IPv4 with reassembly
    icmp.c udp.c     ping, UDP with ports and checksums
    tcp.c            TCP: handshake, sliding window, retransmission, teardown
    dhcp.c dns.c     address configuration and name resolution
    socket.c         the BSD socket layer: sockets, bind, listen, accept, data
    netbuf.c net.c   packet buffers and the transmit/receive path

  fs/
    vfs.c            VFS core: mount table, path resolution, dispatch to backends
    ramfs.c          RAM filesystem backend (/tmp, or / when there is no disk)
    gatofs.c         GatoFS on-disk filesystem driver
    gatofs_fs.c      GatoFS backend for the VFS, serialised by a per-task lock
    fat32.c          FAT32 driver, plus in-kernel volume creation
    devfs.c          /dev backend: null, zero, full, random, console, kmsg, hdX
    procfs.c         /proc backend: meminfo, uptime, cpuinfo, mounts, <pid>/status

  sh/
    shell.c          the vsh shell: parsing, pipelines, history, completion
    stream.c         console and in-memory streams used by the commands
    cmdutil.c        shared helpers for the command implementations
    cmd_fs.c         filesystem commands
    cmd_text.c       text processing commands
    cmd_sys.c        system and shell commands
    cmd_disk.c       block device commands
    cmd_gatofs.c     the gatofs subcommand
    cmd_fat32.c      the fat32 subcommand: format a volume, inspect partitions
    cmd_net.c        ifconfig, ping, dhcp, dns
    cmd_mem.c        virtual memory, swap and self-test commands
    cmd_sched.c      ps, kill, renice, sched, fg
    cmd_users.c      login, su, passwd, useradd, id, chown
    nano.c           the full-screen text editor
    script.c         the shell script reader behind `source`
    simplecc.c       a small C compiler that runs inside the shell
    test.c           the automated test suite behind `selftest` and `make test`
```

`pmm.c` tracks every physical frame in a bitmap, with a reference count per
frame so a page can be shared by more than one mapping (needed for
copy-on-write and for forked address spaces). `paging.c` builds page
directories and tables on top of that: the low 1 MB and the kernel image are
identity-mapped once at boot, the kernel's `.text` and `.rodata` are mapped
read-only, page 0 is left unmapped so a null pointer faults instead of
reading garbage, and the directory keeps a recursive self-mapping so the
kernel can walk and edit its own page tables without a separate window.
Multiple address spaces can be created, switched between and destroyed, and
`paging_clone_range` can copy a range from one space to another either as a
plain copy or as copy-on-write (shared, read-only, ref-counted frames that
split on the next write).

On top of that, `vmm.c` is the virtual memory manager: it divides the
address space into named regions (`kheap`, `vmalloc`, `mmio`, plus whatever
a caller allocates) each with their own permissions and behaviour flags —
demand-paged, swappable, shared, guarded, pinned. The kernel heap itself
is one such region: it starts at `0xC0000000`, is backed by physical frames
only when a page is actually touched, and can grow at runtime with
`vmm growheap`. A page fault first finds the region that covers the
faulting address, then decides what to do: allocate and zero a fresh frame
for a demand region, duplicate a shared frame for a copy-on-write write
fault, pull a page back from disk for one that was swapped out, or panic
with a decoded reason (and the offending region's name and flags) for
anything else, such as a write to a read-only page or an access past a
guard page.

`swap.c` turns a spare ATA disk into a swap area: `mkswap <disk>` writes a
small header and reserves the rest of the disk in 4 KB slots, `swapon`
enables it (also tried automatically at boot), and the VMM's clock-style
reclaimer evicts unreferenced, unaccessed pages to free slots under memory
pressure, or on request through `vmm reclaim`. `swapoff` walks every
swapped-out page back into RAM before turning the device off. All of this
is exercised by `vmm test`, a self-check that allocates memory, forces a
copy-on-write fork, evicts pages to disk and reads them back, verifying a
known pattern at every step.

Multitasking is built on top of all this, see "Multitasking and the scheduler".

## The shell

`vsh` supports pipelines of up to eight stages, output redirection with `>`
and `>>`, input redirection with `<`, sequencing with `;`, conditional
execution with `&&` and `||`, single and double quotes, backslash escapes,
`$VAR` and `$?` expansion, aliases, environment variables, a 32-entry
history, arrow-key line editing with `^A` `^E` `^K` `^U` `^L` `^C`, and tab
completion for both command names and paths.

```
root@felinos:/root$ seq 1 30 | grep -c 1
12
root@felinos:/root$ cut -d= -f1 /etc/os-release
NAME
KERNEL
VERSION
ARCH
root@felinos:/root$ dd if=/etc/os-release of=/dev/hda count=1
1 sectors in, 1 sectors out, 47 bytes copied
root@felinos:/root$ hexdump -n 32 /dev/hda
00000000  4e 41 4d 45 3d 46 65 6c  69 6e 4f 53 0a 4b 45 52  |NAME=FelinOS.KER|
00000010  4e 45 4c 3d 47 61 74 6f  0a 56 45 52 53 49 4f 4e  |NEL=Gato.VERSION|
00000020
```

## Commands

Filesystem: `ls` `cd` `pwd` `mkdir` `rmdir` `touch` `rm` `cp` `mv` `cat`
`tree` `find` `stat` `du` `df` `mount` `umount` `chmod` `chown` `file`

Text: `echo` `printf` `head` `tail` `wc` `grep` `sort` `uniq` `cut` `tr`
`rev` `tee` `nl` `tac` `more` `hexdump` `strings` `diff` `nano`

System: `help` `man` `which` `uname` `uptime` `date` `cal` `free` `lsmem`
`ps` `kill` `renice` `sched` `fg` `dmesg` `whoami` `hostname` `clear` `color` `history` `alias`
`unalias` `env` `export` `set` `unset` `sleep` `beep` `time` `sync` `true`
`false` `test` `expr` `seq` `yes` `basename` `dirname` `lscpu` `smp` `exit`

Hardware and disks: `lspci` `lsblk` `blkid` `bcache` `fdisk` `dd` `fat32`
`gatofs` `acpi` `reboot` `poweroff` `shutdown` `halt`

Network: `ifconfig` `ping` `dhcp` `dns`

Users: `login` `su` `passwd` `useradd` `id`

Development: `source` (run a shell script) `simplecc` (the in-shell C
compiler) `selftest` (the automated suite)

Every command has a manual page: `man <command>`.

Power management: `poweroff` and `reboot` use ACPI. At boot the kernel finds
the RSDP (EBDA and BIOS area), walks the RSDT/XSDT, reads the FADT for the
PM1a/PM1b control blocks, the SMI command port and the reset register, and
extracts the `_S5_` sleep type from the DSDT (or an SSDT). `poweroff` enables
ACPI mode if needed and writes SLP_TYP|SLP_EN to PM1a/PM1b. `reboot` writes
the FADT reset register (I/O, PCI config or memory); when it is missing it
falls back to the keyboard controller, port 0xCF9 and a triple fault. Only
when no usable ACPI is found does `poweroff` use the old QEMU/Bochs/VirtualBox
ports. `acpi` prints the parsed tables and values.

Virtual memory: `vmm` `pmap` `vmstat` `mkswap` `swapon` `swapoff`
`swapinfo` `memtest`

Every command has a manual page: `man <command>`.

## Tests

`selftest` runs the in-kernel suite and returns 0 only if every case passed,
so it is usable from a script and from `if`. It covers the PMM, the VMM, the
heap, the string and formatting helpers, the timer, the scheduler and the RTC,
and prints a `Total: N  Passed: N  Failed: N` summary at the end.

`make test` runs that same suite unattended: `tools/runtests.py` boots the
kernel headless, waits for the shell prompt, types `selftest`, reads the
summary back over the serial console, powers the machine off and exits with
the result — 0 when everything passed, 1 on a failure, 2 if the guest panicked
or never reached a prompt, 124 on timeout. It attaches the same four disks as
`make run`, so the storage-backed cases have real devices to work with.

The two are deliberately different names. `test` is the POSIX-style condition
evaluator (`test -f /etc/passwd`, `test 1 -lt 2`) and takes arguments;
`selftest` takes none. They were the same command name at one point, and since
the command table is searched in order, the suite was unreachable behind the
evaluator.

## Buffer cache

`kernel/bcache.c` is a small read-caching layer between GatoFS and the ATA
driver: 1024 lines of 4 KB (matching GatoFS's block size), fully-associative
with a short linear probe and LRU eviction. `dread`/`dwrite` in
`kernel/fs/gatofs.c` go through it instead of calling the ATA driver
directly, so a hot inode table block, directory block or indirect block is
served from memory instead of costing a disk round trip on every access.
Writes are write-through (issued to disk exactly as before, synchronously)
and then refresh the matching line, so the cache never changes GatoFS's
existing durability guarantees -- only its own write-ahead journal decides
when a write is actually durable, same as before. `bcache` (the shell
built-in) reports lines used, hit rate, evictions and bypassed (non-4-KB)
requests. The cache is invalidated whenever a volume is formatted, mounted
or unmounted, so switching disks never serves stale data.

## Storage

Every filesystem is a backend of one small VFS (`kernel/fs/vfs.h`). A mount
table maps directories to backends, and each backend fills in a `struct
fs_ops` (`open`, `read`, `write`, `readdir`, `stat`, plus `mkdir`, `remove`,
`rename`, `chmod`, `touch`, `statfs`, `sync`, `mount`, `umount`). Paths are
normalised once and routed to the mount with the longest matching prefix, so
shell commands and user programs see the same tree. A new filesystem only has
to provide an `fs_ops` table and be listed in `vfs.c`.

Mounted at boot:

```
/       gatofs when a GatoFS disk is found (formatted on first boot if the disk
        is blank), otherwise ramfs
/dev    devfs   null zero full random urandom console tty kmsg hda..hdd
/proc   procfs  meminfo uptime version cpuinfo mounts filesystems <pid>/status
/tmp    ramfs   volatile, lost on reboot
/mnt/fat<N>  FAT32 partitions found on any ATA or AHCI disk
```

The first boot on a new root creates `/bin /dev /etc /home /mnt /proc /root
/tmp /var/log` with `/etc/motd`, `/etc/hostname`, `/etc/os-release` and
`/root/README`. Everything outside `/tmp`, `/dev` and `/proc` is written to the
GatoFS disk as it happens. `mount` prints the table, `df` shows each mount,
and you can add or remove mounts yourself:

```
mount                              # list the mount table
mount -t ramfs /mnt/scratch        # another RAM filesystem
mount -t gatofs hdb /mnt/disk      # attach a GatoFS volume (one at a time)
umount /mnt/scratch                # refused while busy, or if cwd is inside
```

The disks are files under `/dev`: `/dev/hda` through `/dev/hdd` can be read
and written by byte offset with `dd`, `hexdump`, `cat` and redirection, and by
user programs through `open`. Opening a disk for writing is refused (`device
or resource busy`) while it holds the mounted GatoFS volume or is the active
swap area. `lsblk`, `blkid` and `fdisk -l` inspect them, and a disk can also be
dedicated to swap, see "Virtual memory and swap" below.

`/proc` files are generated when opened, so `cat /proc/meminfo`, `cat
/proc/uptime` and `cat /proc/1/status` work from the shell and from user
programs, and combine with pipes (`grep MemFree /proc/meminfo`).

During boot, before the scheduler starts any other task, `ata.c` reads and
writes sectors the simple way: send the command, poll the Status register
until the drive is ready, transfer the data. Once `sched_run` is handing the
CPU between tasks, the same calls switch to interrupt-driven PIO instead: the
driver enables the drive's IRQ (14 for the primary channel, 15 for the
secondary), issues the command, and blocks the calling task until the IRQ
wakes it back up, rather than spinning on the Status register with the CPU
pinned. A per-channel lock keeps two tasks from touching the same channel's
registers at once, since only one command can be in flight on a channel at a
time; whichever task loses the race waits for the other's transfer to finish
instead of corrupting it. The first sector of a write is still polled, since
the drive never raises an IRQ for it — only from the second sector onward
does "ready for more data" arrive as an interrupt. This is deliberately IRQ-driven
PIO rather than bus-master DMA: it removes the CPU-pinning problem multitasking
cares about without the extra complexity of a PRDT and scatter-gather buffers;
DMA would still be a further step up in raw throughput.

## The editor

`nano <file>` opens a full-screen editor with a title bar, a status line and
a help footer. It supports the arrow keys, `Home`, `End`, `PgUp`, `PgDn`,
`Delete`, `Backspace`, `Enter` and `Tab`, plus `^O` to write, `^X` to exit,
`^K` to cut a line, `^U` to paste it back, `^W` to search, `^G` for help and
`^C` to show the cursor position. It scrolls both vertically and
horizontally and works over VGA and over a serial terminal.

## Virtual memory and swap

The `vmm` command inspects and drives the virtual memory manager directly:

```
vmm stat                  # fault counters, resident/swapped totals, swap device
vmm regions                # named regions in the current address space
vmm spaces                 # every address space and its page directory
vmm map [-a]                # the actual page table contents, run-length encoded
vmm alloc <KB> [-c]         # reserve virtual memory, demand-paged unless -c commits it
vmm touch <id>              # write and verify a pattern across an allocated region
vmm swapout <id>            # force one region's pages out to disk
vmm free <id>               # release a region
vmm reclaim [pages]         # evict cold pages system-wide under the clock algorithm
vmm growheap <MB>           # extend the kernel heap's virtual reservation
vmm test                    # self-check: demand paging, COW, swap round trip
```

`pmap` prints the current region list followed by the page table walk;
`vmstat <hex address>` decodes a single virtual address down to its page
table entry, physical frame, reference count and containing region — the
same information a page fault panic prints for the faulting address.

Swap turns a spare ATA disk into backing store for pages the VMM decides to
evict:

```
mkswap hdd 64      # write a swap header on hdd, sized to 64 MB (-f to overwrite)
swapon hdd         # enable it (also tried automatically at boot)
swapinfo           # device, size, slots used, read/write traffic
swapoff            # page everything back into RAM, then disable
```

`make run` now attaches a third disk, `swap.img` (`make run SWAP_MB=128`
changes its size), so a fresh checkout has a working swap device without
any manual step.

## How much memory the kernel gets

The PMM is sized from what the bootloader reports, not by probing: `mem_top`
is `1 MiB + mem_upper_kb * 1024`, where `mem_upper_kb` comes from the
Multiboot memory fields and, failing that, from a hardcoded 31744 KB (32 MB).
The E820 map, when present, is also what keeps the PMM from handing out frames
the firmware is still using — the ACPI tables, the MADT and the option ROMs all
live in RAM above 1 MiB.

Booted through GRUB (`make run`, `make run-efi`) the kernel gets both, so it
sees the machine's real size and reserves the firmware's structures. Booted
straight from the ELF (`make run-kernel`, `make test`) there is no Multiboot
structure at all: the kernel falls back to 32 MB and reserves no firmware
ranges. That is why a `-m 64M` guest reports 8192 frames under
`make run-kernel` and 16352 under `make run`.

So the fallback is a safe underestimate rather than a wrong answer, but it
does mean the direct-boot path is not exercising the same memory code. Closing
the gap means giving the boot stub a 16-bit `int 0x15` E820 trampoline to
build a `multiboot_info` when the magic is absent; it is not done yet.

## Roadmap

- Per-CPU run queues and priority classes, so the application processors
  actually run tasks instead of idling (see "Processors" below)
- mmap of files, shared mappings and a page cache shared with the block layer
- A writable CMOS clock, so `clock_settime` and `settimeofday` do something

## Processors

`apic.c` drives the local APIC and the I/O APIC, and `smp` reports what it
found. The MADT is walked for processor local APIC entries and I/O APIC
entries; the kernel then brings up every application processor it found.

An AP is started the way the architecture requires: the bootstrap processor
asserts INIT, which resets the target and drops it in real mode, then sends
two SIPIs naming the page to start fetching from. The entry code
(`kernel/ap_trampoline.s`) is linked at a fixed 0x8000, so the 16-bit jump
offsets inside it are absolute linear addresses and nothing has to be copied
into low memory at runtime. It loads its own flat GDT, switches to 32-bit
mode to enable PAE, then to long mode. Before it can call any C code it pulls
the kernel PML4 and a stack out of a fixed block at 0x7000, because until
CR3 is loaded there are no page tables at all and a kernel address would
fault. `ap_startup()` then maps the AP's own LAPIC, adopts the kernel GDT and
TSS, enables the local APIC, and records the processor as online.

```
root@felinos:/root$ smp
Local APIC at 0xfee00000
I/O APIC   at 0xfec00000, ID 0
4 processor(s) in the MADT, 4 online, this one is the bootstrap processor

 CPU  APIC  ROLE        STATE     NOTE
   0     0  bootstrap  online
   1     1  app        online
   2     2  app        online
   3     3  app        online
```

The wait for the APs to report in is bounded: a processor that never answers
is left marked offline and the boot carries on, so a bad firmware table
degrades to single-processor operation instead of hanging.

**The run queue is still global.** An application processor comes up, takes
interrupts and idles in `hlt`, but the scheduler has a single run queue owned
by the bootstrap processor, so it does not run tasks yet. Per-CPU run queues
are the next step on the roadmap; the bring-up, the handover and the
accounting are in place for them.

## Networking

Gato has a TCP/IP stack over an Intel e1000, which is what QEMU's default
`e1000` device presents. The layers are separate files under `kernel/net/`:
the driver and the interface table, Ethernet framing with ARP, IPv4 with
reassembly, ICMP, UDP, TCP, then the socket layer. DHCP configures an address
and a gateway, and DNS resolves names.

```
ifconfig                      # address, mask, gateway, MTU
dhcp                          # request a lease
ping 10.0.2.2                 # or a name, via DNS
dns example.com
```

User programs use a BSD-shaped API from `user/net.h`: `socket`, `bind`,
`connect`, `listen`, `accept`, `send`, `recv`, `sendto`, `recvfrom`,
`getsockname`, `setsockopt`, `closesocket`, plus `inet_addr`, `inet_ntoa`,
`inet_pton`, `htons`, `htonl` and `gethostbyname`. Two programs ship with it:

```
nc -l 8080                    # listen and print what arrives
nc 10.0.2.2 22                # connect and send stdin
wget http://example.com/      # HTTP/1.0 client, -O to choose the output file
```

`mmap` today only serves `MAP_ANONYMOUS | MAP_PRIVATE`; a file-backed or
shared mapping is rejected with `-1` rather than silently returning something
wrong, because there is no page cache wired into the block layer yet.

## Users, signals and identity

There are real user accounts: `useradd <name>` creates one, `passwd` sets its
password, `login` starts a shell as that user, and `su` switches. The shell
tracks the current user and the prompt changes with it.

Each process carries a real and effective user and group id, plus the saved
set, so `setuid`/`setgid` and `setreuid`/`setregid` behave the way the POSIX
calls describe: a privileged process can drop privilege with `seteuid`, and
an unprivileged one cannot regain it. `id` prints the four ids, `chown` uses
them, and `ls -l` shows the owner.

Signals are delivered through `sigaction`, `sigprocmask` and `sigreturn`.
`kill` sends by number; the exit status of a killed process is 128 + signal.

## FAT32

FAT32 is a read/write driver (`kernel/fs/fat32.c`): MBR partition tables of
type 0x0B and 0x0C, the FAT chain, the directory tree and long file names.
At boot any FAT32 partition found on an ATA or AHCI disk is mounted under
`/mnt/fat<N>`.

A blank image has no partition table, so there is nothing for the boot-time
scan to find and `mkfs.vfat` lives on the host rather than in the kernel.
`fat32 format` therefore writes the volume from inside the OS: an MBR, a boot
sector, two FSInfo sectors, a backup boot sector and two FATs, with the
geometry computed from the device size. `fat32 info` shows the partition
table of any disk.

```
fat32 info                    # every disk and its partitions
fat32 format hdc              # write an MBR and a FAT32 volume on hdc
fat32 format -f -L WORK hdc   # overwrite, with a volume label
```

`format` refuses to touch a disk that already has a partition table unless
`-f` is given. The volume is not mounted by `format`; reboot, or
`mount /mnt/fat0 fat32 hdc1`, to use it.

## GatoFS (disk filesystem)

GatoFS is the native on-disk filesystem: 4 KB blocks, block bitmap, inode
table, 12 direct + indirect + double-indirect pointers. Files up to 4 GB-1,
volumes up to 128 GB (LBA28), sparse files, nested directories, names up to 57
chars. Mounted automatically at boot as `/` if a GatoFS disk is found. If none is
found, the largest ATA disk whose first 1 MiB is entirely zero is formatted
on the spot, so a fresh disk needs no manual step; a disk with a partition
table, boot sector or any data in that area is never touched.

`make run` attaches a second (sparse, 4 GB) disk `gatofs.img` as `hdb`
(`make run VDISK_MB=8192` changes the size before the first run).

```
gatofs format hdb [label]      # once per disk (-f overrides the MBR safety check)
gatofs mount [hdb] [dir] | umount | df   # default dir /mnt/gato
gatofs ls [-l] [path] | cat | write <path> <text> | stat
gatofs mkdir [-p] | rm [-r] | mv
gatofs import <file> <path>           # any VFS path -> volume path
gatofs export <path> <file>           # volume path -> any VFS path
gatofs gen|verify <path> <MB> [startMB]   # stress test with a checkable pattern
gatofs fsck [-y]                          # check the volume, -y repairs what it finds
```

The `gatofs` subcommands take volume paths (`/etc/motd` is the volume's `/etc/motd`, wherever it
is mounted). Only one GatoFS volume can be mounted at a time. `gatofs format` mounts the
new volume on `/mnt/gato`.

Kernel API for the volume itself: `kernel/fs/gatofs.h` (`gatofs_open/read/write/seek/close`,
`gatofs_load` to read a whole program into memory, `gatofs_save`,
`gatofs_readdir`, `gatofs_stat`, `gatofs_mkdir`, `gatofs_remove`,
`gatofs_rename`).

Every metadata write (inode, bitmap, directory block, indirect block) goes
through a small write-ahead journal (`kernel/fs/gatofs.c`): the changed
blocks are written to a reserved journal area and flushed, then a checksummed
header naming them is written and flushed, then the blocks are written to
their real location and flushed, and only then is the header cleared. A crash
at any point leaves either no header (nothing to redo) or a complete,
checksummed header (`jrnl_replay` replays it at the next mount before
anything else touches the volume) -- a crash mid-write can no longer corrupt
the volume the way a bare in-place write could. `gatofs fsck` walks every
inode, cross-checks its blocks against the bitmap and against each other
(catching out-of-range and cross-linked blocks), validates every directory
entry's target inode, and recomputes the free block and free inode counts;
`-y` repairs what it finds (removing bad directory entries, fixing the
bitmap and superblock counts) instead of only reporting.

## User mode and syscalls

Gato runs programs in ring 3. The GDT has user code/data segments (0x1B/0x23)
and a TSS; `int 0x80` is a DPL-3 gate. ABI: `rax` = number, `rdi/rsi/rdx/
rcx/r8/r9` = args, result in `rax` (numbers in `kernel/syscall.h`). Pointers
are validated against the process's own regions, and a user fault (page
fault, GPF) kills only the process, not the kernel.

`open` goes through the VFS, so it works on every mount (`/`, `/tmp`, `/dev`,
`/proc`); descriptors are closed when the process exits or is killed.

The syscall set covers:

- **Files and memory**: `read` `write` `open` `close` `lseek` `stat` `fstat`
  `readdir` `mkdir` `rmdir` `unlink` `rename` `chdir` `getcwd` `dup` `dup2`
  `pipe` `fcntl` `truncate` `ftruncate` `mmap` `munmap` `mprotect` `madvise`
  `msync` `sbrk`
- **Processes**: `exit` `fork` `execve` `spawn` `waitpid` `kill` `getpid`
  `getppid` `yield` `sleep`
- **Identity**: `getuid` `geteuid` `getgid` `getegid` `setuid` `setgid`
  `setreuid` `setregid` `getresuid` `getresgid` `setresuid` `setresgid`
- **Signals**: `sigaction` `sigprocmask` `sigreturn`
- **Time**: `time` `uptime` `nanosleep` `clock_gettime` `gettimeofday` `times`
  `getrusage`
- **Sockets**: `socket` `bind` `connect` `listen` `accept` `send` `recv`
  `sendto` `recvfrom` `getsockname` `setsockopt` `gethostbyname` `getifaddr`
  `getifaddrs`
- **System**: `uname` `getrlimit` `setrlimit`

A few of these are deliberately partial, and say so rather than pretending:

- `mmap` serves `MAP_ANONYMOUS | MAP_PRIVATE` only. File-backed and shared
  mappings return `-1`, because there is no page cache integrated with the
  block layer yet and returning wrong data silently would be worse.
- `clock_settime` and `settimeofday` return `-ENOSYS`; the kernel reads the
  CMOS clock but never writes it.
- `setsockopt` accepts and ignores its options, which is enough for
  `SO_REUSEADDR` to succeed; there is nothing configurable to set yet.
- `setrlimit` reports the limits actually in force rather than claiming to
  enforce limits it does not.

Programs are static ELF64 (x86-64) files, or ELF32 (i386, run in compatibility
mode), linked at 0x40000000 (`user/`), embedded in the kernel and installed at
`/bin` on boot. Run one by name (`hello a b`) or with `exec <path> [args]`.
Add one: write `user/x.c`, add it to `USER_PROGS` in the Makefile, to
`kernel/progs.s` and to the table in `kernel/user.c`.

Demos: `hello`, `hello32`, `cat`, `crash [kmem|cli|null]`, `spin [seconds]`,
`count [n] [ms]`, `launch`, `nc`, `wget`, `mmtest`.

Extra syscalls for multitasking: `yield` (10), `getppid` (11), `spawn(path, argv)`
(12), `waitpid(pid, &status)` (13, pid -1 waits for any child) and
`kill(pid, sig)` (14). `spawn` runs `/bin/<name>` or a path and returns the child
pid; the exit status of a killed process is 128 + signal.

## Multitasking and the scheduler

Every program runs as its own process: a task with a private address space
(built with `vmm_space_create`), its own file descriptor table, program break,
stdin line buffer and a 16 KB kernel stack. The shell is a kernel thread (`vsh`,
pid 1) and the CPU falls back to the `idle` task (pid 0) when nothing is ready.

The PIT (100 Hz) drives a preemptive round-robin scheduler. A task runs for a
time slice of 5 ticks (50 ms, `sched quantum <ticks>` changes it); when the slice
ends and another task is ready, the timer interrupt switches tasks even if the
program is stuck in an infinite loop. `nice` (-6..6, `renice <nice> <pid>`)
scales the slice: nice 6 gets a fifth of the CPU that nice 0 gets while both
are runnable, and nobody starves. Sleeping (`sleep`, `sleep_ms`), reading the
keyboard and `waitpid` block the task instead of busy-waiting, so an idle system
sits in `hlt`.

```
root@felinos:/root$ spin &            # run in the background
[2] spin
root@felinos:/root$ spin 3 &
[3] spin
root@felinos:/root$ ps
  PID  PPID STATE      NI      TIME   %CPU WAIT   NAME
    0     0 ready       0     1.32   25.9 -      idle [kernel]
    1     0 running     0     0.01    0.1 -      vsh [kernel]
    2     1 ready       0     0.90   38.2 -      spin
    3     1 ready       0     0.60   35.7 -      spin
root@felinos:/root$ kill 2
[2] killed      spin
```

A command ending in ` &` starts in the background (`exec <prog> &` works too) and
its termination is reported before the next prompt. Only the foreground job
receives the keyboard: `Ctrl+C` kills it, background jobs that read stdin get
end-of-file, and `fg [pid]` brings a background job back to the foreground.
Children of a process that exits are re-parented to pid 0 and reaped there.

Limits and design notes:

- Kernel code is preemptible. A timer tick can switch away from kernel code
  mid-syscall or mid-built-in the same way it already could from user code,
  provided the code is not holding a spinlock and did not disable interrupts
  itself (`preempt_disable()`/`irq_save()` both suppress it). `sched
  kpreempt off` disables it at runtime for debugging; `sched` reports the
  current setting and the running counts. Every shared kernel data structure
  that used to rely on "kernel code runs to completion" is now protected
  explicitly: see "Locking" below.
- At most 23 processes at once (`VMM_MAX_SPACES`) and 31 tasks (`SCHED_MAX_TASKS`).
- FPU/SSE state is saved and restored on every context switch
  (`kernel/fpu.c`, `fxsave64`/`fxrstor64`), so ring 3 code may use floating
  point and SSE freely; the kernel itself still avoids it and is built with
  `-mgeneral-regs-only`.

## Locking

`kernel/sync.h` provides the primitives kernel code uses to protect state now
that kernel-mode code can be preempted or block:

```
spinlock_t   busy-wait; also disables kernel preemption while held (and,
             via spin_lock_irqsave, interrupts). For short, non-blocking
             critical sections only -- the VGA cursor, the PCI config port,
             the CMOS clock, the PC speaker, the physical frame bitmap.
mutex_t      sleeping lock, FIFO hand-off, optionally recursive. The holder
             may block and is not killable until it unlocks, so a signal
             can never leak a held lock. Guards the kernel heap, the VFS
             (mount table, path operations), the VMM (regions, address
             spaces, swap), and now the ATA channel lock (mutex_lock/unlock
             instead of the earlier hand-rolled wait/busy pair).
semaphore_t  counting semaphore, FIFO wake-up, optional timeout.
```

A deadlock, a spinlock taken twice, or a mutex unlocked by a task that
doesn't own it panics immediately with the lock's name rather than hanging
silently. `sync` (the shell built-in) lists every registered lock with its
acquisition and contention counts; a mutex wait longer than 30 s logs a
warning naming the pid and the lock. Lock order is always heap-independent:
the VMM mutex is taken before the heap mutex is ever entered from inside it
(`vmm_heap_extend` releases the VMM lock before growing the heap), and the
ATA channel mutex is always the innermost lock.

`sync selftest` runs a short in-kernel test of all three primitives
(contention, recursion, FIFO ordering, semaphore hand-off) and reports
pass/fail.

## 64-bit notes

- Boot: GRUB (Multiboot 1) loads `gato.bin` (ELF64 converted with `objcopy -O elf32-i386`);
  `boot/boot.s` enables PAE + long mode with a temporary 4 GiB identity map, then calls `kernel_main`.
- Memory: 4-level paging (PML4/PDPT/PD/PT). Kernel, stacks and physical memory stay below 4 GiB
  and are identity-mapped, so kernel APIs still use 32-bit addresses (max 4 GiB RAM).
- Syscalls (`int 0x80`, number in rax): 64-bit programs pass args in
  rdi/rsi/rdx/rcx/r8/r9; 32-bit programs (ELF32, `hello32`) keep
  ebx/ecx/edx and the extra arguments are read off the interrupted stack.
  GDT: 0x1B = 32-bit user code, 0x2B = 64-bit user code.
- The kernel itself is built with `-mgeneral-regs-only` and uses no SSE or FPU
  instructions. Ring 3 code is free to use them: `kernel/fpu.c` saves and
  restores the full state on every context switch.
- The application-processor entry code is the one place that is deliberately
  not written in C: it has to reach long mode before there are page tables to
  load C from. See "Processors" above.

