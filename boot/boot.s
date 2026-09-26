.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
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

.section .text
.code32
.global _start
.type _start, @function
_start:
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
.size _start, . - _start

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

.section .note.GNU-stack,"",@progbits
