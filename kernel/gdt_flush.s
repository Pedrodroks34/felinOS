.section .text
.global gdt_flush
gdt_flush:
    lgdt (%rdi)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    pop %rax
    push $0x08
    push %rax
    lretq

.section .note.GNU-stack,"",@progbits
