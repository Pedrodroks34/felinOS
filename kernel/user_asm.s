.section .text

# user_jump(rdi = entry, rsi = user stack, edx = 1 for a 32-bit (compat) program)
.global user_jump
user_jump:
    mov $0x23, %eax
    mov %ax, %ds
    mov %ax, %es
    push $0x23
    push %rsi
    push $0x202
    mov $0x2B, %eax
    mov $0x1B, %ecx
    test %edx, %edx
    cmovne %ecx, %eax
    push %rax
    push %rdi
    iretq

.section .note.GNU-stack,"",@progbits
