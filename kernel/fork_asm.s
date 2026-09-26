.section .text
.global fork_trampoline
fork_trampoline:
    mov %rdi, %rsi
    sub $176, %rsp
    mov %rsp, %rdi
    mov $176, %rcx
    rep movsb
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    add $16, %rsp
    iretq

.section .note.GNU-stack,"",@progbits
