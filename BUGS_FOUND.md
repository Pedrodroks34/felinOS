# Bugs Found in FelinOS/Gato Kernel - Code Review

## Critical Bugs (Must Fix)

### 1. PMM (Physical Memory Manager) - kernel/pmm.c
- **Line 70-83**: `find_run()` starts from 0 every time - O(N) per allocation. Should use `search_from` for optimization.
- **Lines 98-101**: `memset(frame_bitmap, 0xFF, ...)` then clear all frames - inefficient. Should just `memset` to 0.
- **Line 126**: `refs_frames = (total_frames + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE` - WRONG formula! Should be `total_frames / 255` (1 ref byte per frame, max 255 refs).
- **Line 141-160**: `alloc_frame_l()` uses `search_from` but `find_run()` doesn't, defeating the optimization.

### 2. Paging - kernel/paging.c
- **Line 347-348**: `dst->mapped++` inside loop but inside `if (src_table[i] & PAGE_PRESENT)` check - `mapped` is page count, not table count. Could double-count.
- **Lines 376-382**: COW handling - doesn't check if page is already COW before modifying source table.
- **Line 472-473**: `paging_space_destroy` calls `paging_space_switch(&kernel_space)` if `as == current_space` - could cause issues if kernel is using the space.
- **Line 489-491**: Check `i * PT_ENTRIES + j >= RESERVED_PDE` - `RESERVED_PDE` is 2046, but this checks PDE index, not full virtual address.

### 3. VMM (Virtual Memory Manager) - kernel/vmm.c
- **Lines 318-334**: `copy_on_write` - doesn't handle case where page is already COW.
- **Lines 360-369**: `vmm_handle_fault_l` COW handling - doesn't check if page already COW.
- **Lines 185-212**: `evict_page` - doesn't check if page already swapped out.
- **Line 730-732**: `vmm_describe_l` - `mutex_trylock` failure just returns string, doesn't indicate lock contention.

### 4. Scheduler - kernel/sched.c
- **Lines 337-340**: `atomic_bugs` incremented and task name copied - no synchronization (diagnostic only).
- **Line 347-347**: `runq_push` sets `state = TASK_READY` but doesn't check if task already in queue.
- **Lines 239-258**: `reap_orphans` uses `irq_save/restore` but `task_release` calls `vmm_space_destroy` which takes mutexes - potential deadlock.
- **Lines 478-495**: `sched_wake_one` doesn't check if task already in run queue before pushing.
- **Line 497-534**: `sched_tick` iterates all tasks O(N) every tick.
- **Lines 536-561**: `sched_irq_return` kernel preemption check doesn't check `c->nokill > 0`.
- **Line 563-572**: `sched_syscall_return` doesn't check `c->preempt` or `c->nokill`.

### 5. Sync Primitives - kernel/sync.c
- **Lines 68-74**: `spin_lock` spin count limit 200,000,000 - arbitrary.
- **Lines 183-208**: `mutex_lock` timeout handling - if timeout occurs, `m->owner` might still not be `me`, leading to infinite loop.
- **Lines 198-204**: `warned` flag logic - `irq_restore(f)` called but then `f = irq_save()` called again while lock still held by someone else.
- **Lines 254-255**: `mutex_unlock` hands lock to `sched_wake_one(m)` which does `runq_push(t)` - task might still be in `sched_wait_on_timeout`.

### 6. GatoFS Filesystem - kernel/fs/gatofs.c
- **Lines 339-358**: `pread_i` - `static uint8_t tmp[BS]` static buffer - not thread-safe!
- **Lines 361-384**: `pwrite_i` - same static buffer issue.
- **Lines 386-402**: `dir_find` - `static struct vdirent buf[BS / DE_SIZE]` static buffer - not thread-safe!
- **Lines 404-423**: `dir_add` - same static buffer issue.
- **Lines 425-442**: `dir_del` - same static buffer issue.
- **Lines 444-454**: `dir_empty` - same static buffer issue.
- **Lines 179-191**: `iread`/`iwrite` - no bounds check on `ino`.
- **Line 126**: `refs_frames = (total_frames + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE` - WRONG formula.
- **Line 266-285**: `slot` - no bounds check on `i`.
- **Line 287-300**: `topslot` - no bounds check on `i`.
- **Line 302-316**: `bmap` - no bounds check on `idx`.
- **Line 318-327**: `free_table` - recursive, could stack overflow for deep indirection.
- **Line 339-358**: `pread_i` - `static uint8_t tmp[BS]` static buffer - not thread-safe!
- **Lines 361-384**: `pwrite_i` - same static buffer issue.
- **Lines 386-402**: `dir_find` - static buffer on stack - not thread-safe!
- **Lines 404-423**: `dir_add` - static buffer.
- **Lines 425-442**: `dir_del` - static buffer.
- **Lines 444-454**: `dir_empty` - static buffer.

### 8. FAT32 Filesystem - kernel/fs/fat32.c
- **Lines 175-218**: `fat32_find_entry` - `static uint8_t cluster_buf[512 * 8]` (4KB) on stack - OK for kernel stack but could overflow if recursive.
- **Line 175-218**: `fat32_find_entry` - `strcasecmp` used but no locale support.
- **Line 174-218**: Case-insensitive comparison with `strcasecmp` - no locale support.

### 9. ATA Driver - kernel/drivers/ata.c
- **Lines 142-158**: `ata_wait_irq` - lost wakeup race condition (fixed in earlier changes).
- **Lines 67-71**: `ata_delay` - fixed to use `io_wait()`.

### 9. Serial Driver - kernel/drivers/serial.c
- **Lines 34-42**: `serial_putchar` - busy-wait with fixed guard count (100000) - could hang if hardware broken.
- **Lines 94-105**: `serial_read_nonblock` - uses `irq_save/irq_restore` but doesn't disable interrupts during ring buffer access.

### 9. Keyboard - Minor
- **Lines 100-175**: `keyboard_callback` - no handling of Pause/Break key (0xE1 0x1D 0x45 / 0xE1 0x9D 0xC5).

### 10. Syscalls - kernel/user.c
- **Lines 163-174**: `ustr_copy` - calls `uptr_ok` which calls `vmm_find_region` which takes `vmm_mtx` - but `ustr_copy` called from syscall handlers that might already hold locks.
- **Lines 548-550**: `arg1`/`arg2`/`arg3` - no validation that registers contain valid user pointers.
- **Lines 556-563**: `stack_arg` - for 32-bit programs, reads from user stack without validating stack pointer.
- **Lines 548-567**: Syscall argument extraction - no validation that `r->rax` is valid syscall number.
- **Lines 178-193**: `user_is_elf` - opens file but doesn't close on all error paths (actually does close).
- **Lines 195-268**: `load_elf` - no validation that segments don't overlap.
- **Lines 272-302**: `build_stack` - no validation that `argv` pointers are valid.
- **Lines 569-608**: `sys_write` - allocates `IO_CHUNK` (4096) buffer per call - could be stack-heavy.
- **Lines 610-678**: `sys_read` - same issue with `IO_CHUNK` buffer.
- **Lines 700-723**: `sys_open` - `ustr_copy` called without checking if path valid first.
- **Lines 739-782**: `sys_spawn` - `ustr_copy` for path and args without proper error handling.
- **Lines 1055-1128**: `sys_execve` - `vmm_space_destroy(old_space)` called but `old_space` might be in use by other threads.
- **Lines 1130-1171**: `sys_sigaction` - `uptr_ok` called on user pointers without checking validity.
- **Lines 1173-1198**: `sys_sigreturn` - `sigframe` structure copied from user stack without validating all fields.
- **Lines 1318-1328**: `sock_arg` - doesn't validate socket is actually a socket.
- **Lines 1507-1552**: `sys_mmap` - `vmm_alloc_at` called with `VM_USER` but doesn't check if address already mapped.
- **Line 1546**: `vmm_alloc_range` - calls `find_hole` which scans all regions O(N).
- **Lines 1554-1571**: `sys_munmap` - doesn't check if range actually mapped by this process.
- **Lines 1573-1593**: `sys_mprotect` - same issue.
- **Lines 1595-1622**: `sys_madvise` - `vmm_release` called but doesn't check if pages actually mapped.
- **Lines 1682-1700**: `sys_settimeofday` - calls `unix_to_time` which uses `civil_from_days` - potential integer overflow for years > 2099.
- **Lines 1736-1763**: `sys_getrlimit`/`sys_setrlimit` - no enforcement of limits.

### 11. Boot Issues - Critical
- **`run-kernel`/`run-smp`/`run-serial`**: 32-bit ELF conversion produces binary that enters SMM mode in QEMU 10.2. The objcopy conversion loses PVH note or multiboot header.
- **`make run`**: ISO is UEFI-only (no BIOS GRUB modules), so `make run` fails with "drive with bus=1, unit=0 (index=2) exists".
- **`make run-efi`**: Works but needs OVMF firmware paths overridden.

---

## Fix Priority Order

1. **Fix static buffers in GatoFS** - Thread safety critical
2. **Fix PMM ref counting bug** - Memory management corruption
3. **Fix static buffers in VMM fault handler** - COW race conditions
4. **Fix scheduler run queue duplicates** - Task scheduling corruption
5. **Fix mutex/spinlock timeout handling** - Deadlock prevention
6. **Fix 32-bit ELF boot issue** - `run-kernel`/`run-smp` targets
7. **Fix `make run` drive index conflict** - IDE drive index conflict
8. **Add debug build configuration** - For debugging
9. **Fix syscall argument validation** - Security hardening