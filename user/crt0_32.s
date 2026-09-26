.section .text
.global _start
_start:
    xor %ebp, %ebp
    mov (%esp), %eax
    lea 4(%esp), %edx
    and $-16, %esp
    push %edx
    push %eax
    call main
    mov %eax, %ebx
    mov $1, %eax
    int $0x80
1:  jmp 1b
.section .note.GNU-stack,"",@progbits
