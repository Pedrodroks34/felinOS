.macro ISR_NOERR num
.global isr\num
isr\num:
    cli
    push $0
    push $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    cli
    push $\num
    jmp isr_common
.endm

.macro IRQ num, vector
.global irq\num
irq\num:
    cli
    push $0
    push $\vector
    jmp irq_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 128

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* APIC ISR handlers */
ISR_NOERR 50  /* APIC_TIMER_VECTOR = 0x32 */
ISR_NOERR 255 /* APIC_SPURIOUS_VECTOR = 0xFF */
ISR_NOERR 51  /* APIC_ERROR_VECTOR = 0x33 */
ISR_NOERR 52  /* APIC_IPI_VECTOR = 0x34 */
ISR_NOERR 53  /* APIC_RESCHED_VECTOR = 0x35 */

.macro COMMON_ENTRY handler
    cld
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rdi
    call \handler
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
.endm

.extern isr_handler
isr_common:
    COMMON_ENTRY isr_handler

.extern irq_handler
irq_common:
    COMMON_ENTRY irq_handler

.section .note.GNU-stack,"",@progbits
