.code16
.section .text
.global ap_trampoline_start
.global ap_trampoline_bin
.global ap_trampoline_bin_end

ap_trampoline_bin:
ap_trampoline_start:
    cli
    lgdt gdt_ptr
    ljmp $0x08, $ap_pm_start

.code32
ap_pm_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $ap_long_start

.code64
ap_long_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    xor %rbp, %rbp
    call ap_startup

    cli
1:  hlt
    jmp 1b

.align 8
gdt:
    .quad 0
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
gdt_ptr:
    .word gdt_end - gdt - 1
    .long gdt
    .long 0
gdt_end:

gdt64:
    .quad 0
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
    .quad 0x0000920000000000
    .quad 0x00209A0000000000
    .quad 0x00209A0000000000
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64
    .long 0
gdt64_end:

ap_trampoline_bin_end: