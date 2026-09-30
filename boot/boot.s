.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot, "a"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bss
.align 4096
boot_pml4:  .skip 4096
boot_pdpt:  .skip 4096
boot_pd:    .skip 4096 * 4
.align 16
stack_bottom:
.skip 32768
stack_top:

.section .rodata
.align 8
boot_gdt:
    .quad 0
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
boot_gdt_ptr:
    .word . - boot_gdt - 1
    .long boot_gdt
    .long 0

/* 32-bit protected-mode entry point. Placed in a separate section so the
 * linker can put it at a low address (< 1M) for multiboot/PVH boot. */
.section .boot32, "ax"
.code32
.global boot32_entry
.type boot32_entry, @function
boot32_entry:
    cli
    mov $stack_top, %esp
    mov %eax, %edi
    mov %ebx, %esi

    mov $boot_pdpt, %eax
    or $3, %eax
    mov %eax, boot_pml4

    xor %ecx, %ecx
1:  mov %ecx, %edx
    shl $12, %edx
    lea boot_pd(%edx), %eax
    or $3, %eax
    mov %eax, boot_pdpt(,%ecx,8)
    inc %ecx
    cmp $4, %ecx
    jne 1b

    xor %ecx, %ecx
2:  mov %ecx, %eax
    shl $21, %eax
    or $0x83, %eax
    mov %eax, boot_pd(,%ecx,8)
    inc %ecx
    cmp $2048, %ecx
    jne 2b

    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4
    mov $boot_pml4, %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0
    lgdt boot_gdt_ptr
    ljmp $0x08, $long_mode
.size boot32_entry, . - boot32_entry

.code64
long_mode:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    mov $stack_top, %rsp
    xor %ebp, %ebp
    mov %edi, %edi
    mov %esi, %esi
    call kernel_main
    cli
halt_loop:
    hlt
    jmp halt_loop

/* Xen PVH boot note. Multiboot is the protocol GRUB speaks, but QEMU's -kernel
 * no longer loads Multiboot images: it accepts a bzImage, or an ELF carrying
 * this note, and refuses anything else with "Error loading uncompressed kernel
 * without PVH ELF Note". Emitting it costs nothing and lets the kernel boot
 * straight from the ELF, with no ISO and no GRUB in the way, which is what
 * 'make test', 'make run-kernel' and 'make run-smp' rely on.
 *
 * XEN_ELFNOTE_PHYS32_ENTRY names the 32-bit protected-mode entry point, which
 * is boot32_entry above, and QEMU takes it as the absolute address to jump to. */
.set XEN_ELFNOTE_PHYS32_ENTRY, 18

.section .note.Xen, "a", @note
.align 4
    .long 4, 4, XEN_ELFNOTE_PHYS32_ENTRY   /* namesz, descsz, type */
    .asciz "Xen"
    .align 4
    .long boot32_entry

.section .note.GNU-stack,"",@progbits
