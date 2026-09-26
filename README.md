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

```
FelinOS 0.2  -  Gato kernel 0.2 (x86_64)

  [ok] global descriptors     flat 64-bit code and data segments
  [ok] interrupt table        32 exceptions, 16 hardware IRQs
  [ok] memory map             65023 KB reported by the bootloader
  [ok] physical memory        16352 frames (63 MB), 15717 free
  [ok] paging                 63 MB mapped, 30 tables, directory at 0x0015c000
  [ok] virtual memory         demand paging, copy-on-write, 5 regions
  [ok] kernel heap            55768 KB demand-paged heap at 0xc0000000
  [ok] system timer           PIT channel 0 at 100 Hz
  [ok] keyboard               PS/2 set 1, shift, ctrl and caps lock
  [ok] serial console         COM1 at 115200 baud, input and output
  [ok] real time clock        2026-09-19 12:00:00
  [ok] processor              GenuineIntel family 6 model 6
  [ok] pci bus                6 devices on the bus
  [ok] ata controller         4 device(s), first is QEMU HARDDISK
  [ok] swap                   63 MB on hdd, 16376 slots
  [ok] virtual filesystem     / is gatofs (hdb), /dev, /proc and /tmp mounted
  [ok] gatofs                 GatoFS on hdb, 256 MB
  [ok] shell                  vsh with pipes, redirection and history

Welcome to FelinOS running the Gato kernel.
Type 'help' for the command list, 'man <command>' for details.

root@felinos:/root$
```

## Building

Requirements: `gcc` (x86-64; `-m32` only for the `hello32` demo), `binutils`,
`make`, and for the bootable image `grub-mkrescue`, `xorriso` and `mtools`.

```sh
make            # builds gato.bin and felinos.iso
make clean      # removes objects, the kernel and the iso
make distclean  # also removes the virtual disk image
```

## Running

```sh
make run         # boots felinos.iso through GRUB in QEMU, with a 32 MB disk
make run-kernel  # boots gato.bin directly, skipping the bootloader
make run-serial  # same, but headless: the whole session goes over COM1
```

The console is mirrored to the VGA text screen and to COM1 at 115200 baud,
and keyboard input is merged with serial input, including ANSI arrow keys.
The shell and the editor are therefore fully usable over a serial line with
no display attached.

Writing the ISO to a USB stick and booting a real PC works as well; Gato
only requires a Multiboot loader, a VGA text mode and a PS/2 keyboard.

## Architecture

```
boot/boot.s          Multiboot header, stack setup, entry into kernel_main
linker.ld            loads the kernel at 1 MB, exports kernel_start/kernel_end

kernel/
  kernel.c           boot sequence and subsystem initialisation
  console.c          VGA + serial output, kprintf, kernel log ring buffer
  system.c           multiboot parsing, memory layout, version information
  gdt.c  idt.c       segmentation, 32 exceptions, 16 IRQs, panic handler
  pmm.c              physical frame bitmap allocator with reference counts
  paging.c           page directories/tables, address spaces, page faults
  vmm.c              virtual memory manager: regions, demand paging, COW
  swap.c             disk-backed swap area, page-out/page-in
  sched.c            preemptive round-robin scheduler, tasks, sleep/wait/kill
  sched_asm.s        the kernel stack switch (switch_context)
  user.c             ring 3 processes, ELF loader, syscalls
  isr.s gdt_flush.s  low level stubs

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
    pic.c            8259 remapping and masking
    pit.c            100 Hz timer, uptime, sleep
    rtc.c            CMOS clock, unix time, calendar arithmetic
    ata.c            ATA PIO: IDENTIFY, LBA28/48 read/write, MBR partitions,
                     interrupt-driven once the scheduler is up (IRQ14/15)
    pci.c            configuration space scan, vendor and class names
    cpu.c            CPUID vendor, brand, family and feature flags
    acpi.c           RSDP/RSDT/XSDT/FADT parser, _S5_ lookup, ACPI reset register
    power.c          reboot, power off, halt (ACPI first, legacy ports as fallback)
    speaker.c        PC speaker tones

  fs/
    vfs.c            VFS core: mount table, path resolution, dispatch to the backends
    ramfs.c          RAM filesystem backend (/tmp, or / when there is no disk)
    gatofs.c         GatoFS on-disk filesystem driver
    gatofs_fs.c      GatoFS backend for the VFS, serialised by a per-task lock
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
    cmd_mem.c        virtual memory, swap and self-test commands
    cmd_sched.c      ps, kill, renice, sched, fg
    nano.c           the full-screen text editor
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
`tree` `find` `stat` `du` `df` `mount` `umount` `chmod` `file`

Text: `echo` `printf` `head` `tail` `wc` `grep` `sort` `uniq` `cut` `tr`
`rev` `tee` `nl` `tac` `more` `hexdump` `strings` `diff` `nano`

System: `help` `man` `which` `uname` `uptime` `date` `cal` `free` `lsmem`
`ps` `kill` `renice` `sched` `fg` `dmesg` `whoami` `hostname` `clear` `color` `history` `alias`
`unalias` `env` `export` `set` `unset` `sleep` `beep` `time` `sync` `true`
`false` `test` `expr` `seq` `yes` `basename` `dirname` `lscpu` `exit`

Hardware and disks: `lspci` `lsblk` `blkid` `fdisk` `dd` `acpi` `reboot`
`poweroff` `shutdown` `halt`

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

## Roadmap

- Priority classes and per-CPU run queues on top of the round-robin scheduler
- A real on-disk filesystem, starting with FAT32 on the ATA driver
- Loading and executing programs from disk instead of built-in commands

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
and a TSS; `int 0x80` is a DPL-3 gate. ABI: `eax` = number, `ebx/ecx/edx` =
args, result in `eax` (numbers in `kernel/syscall.h`): exit, write, read,
open, close, getpid, uptime, sleep, sbrk. `open` goes through the VFS, so it works on
every mount (`/`, `/tmp`, `/dev`, `/proc`); descriptors are closed when the process
exits or is killed. Pointers are validated against user
regions. A user fault (page fault, GPF...) kills only the process.

Programs are static ELF64 (x86-64) files, or ELF32 (i386, run in compatibility mode), linked at 0x40000000 (`user/`), embedded in
the kernel and installed at `/bin` on boot. Run one by name (`hello a b`) or
with `exec <path> [args]`. Add one: write `user/x.c`, add it to `USER_PROGS`
in the Makefile, to `kernel/progs.s` and to the table in `kernel/user.c`.
Demos: `hello`, `cat`, `crash [kmem|cli|null]`, `spin [seconds]`, `count [n] [ms]`, `launch`.

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
- Syscalls (`int 0x80`, number in rax): 64-bit programs pass args in rdi/rsi/rdx; 32-bit programs
  (ELF32, `hello32`) keep ebx/ecx/edx. GDT: 0x1B = 32-bit user code, 0x2B = 64-bit user code.
- Code is built with `-mgeneral-regs-only` (SSE/FPU state is not saved on context switch).
