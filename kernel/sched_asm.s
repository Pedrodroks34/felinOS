.section .text

# switch_context(rdi = uint64_t *old_sp, rsi = new_sp)
.global switch_context
switch_context:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

.section .note.GNU-stack,"",@progbits
