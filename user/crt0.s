.section .text
.global _start
_start:
    xor %ebp, %ebp
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    and $-16, %rsp
    call main
    mov %eax, %edi
    mov $1, %eax
    int $0x80
1:  jmp 1b
.section .note.GNU-stack,"",@progbits
