/* Entry code for an application processor.
 *
 * An AP reset by INIT wakes up in real mode at the architectural reset
 * vector, with no page tables, no idea where the kernel is and a flat GDT of
 * its own. This brings it up in three steps and hands over to ap_startup().
 *
 * The linker script places this in .ap_tramp at 0x8000, which is what makes
 * the 16-bit jump offsets below meaningful: they are absolute linear
 * addresses, and the local GDT has a zero base, so CS:IP is the linear
 * address directly. Nothing copies this code anywhere.
 */

/* A far jump written out by hand rather than left to the assembler.
 *
 * Only the 16-bit offset form is legal in real mode, where the operand size
 * is pinned by the real-mode CS descriptor. The assembler does pick it here,
 * but it is not obvious from the source, and a disassembler reading this as a
 * 64-bit object file cannot tell the two encodings apart at all. Spelling it
 * out keeps the one constraint that matters visible. */
.macro FAR_JMP16 sel, target
    .byte 0xEA
    .word \target
    .word \sel
.endm

.section .ap_tramp, "ax", @progbits
.code16
.global ap_trampoline_start

ap_trampoline_start:
    cli
    cld
    lgdtl gdt_ptr
    FAR_JMP16 0x08, ap_pm_start

.code32
ap_pm_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Long mode needs PAE enabled before the far jump, otherwise the
     * processor refuses to leave protected mode. This is CR4.PAE, not CR0.PG:
     * paging cannot be switched on yet, because this processor has no page
     * tables at all until it loads CR3 further down. */
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    lgdtl gdt64_ptr
    FAR_JMP16 0x08, ap_long_start

.code64
ap_long_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* The bootstrap processor left what this processor needs to join the
     * kernel in the low page at 0x7000: the physical address of the kernel
     * PML4, and a stack to run on. This has to happen before any C code
     * runs, because until CR3 is loaded there are no page tables at all and
     * a kernel address would fault. Reading the block now is safe precisely
     * because paging is off, which makes a low linear address its physical
     * one. */
    mov $0x7000, %rbx
    mov (%rbx), %rax
    mov %rax, %cr3
    mov 8(%rbx), %rsp

    xor %rbp, %rbp
    call ap_startup

    /* ap_startup() parks and never returns. Halt anyway, so that a mistake
     * there cannot run off the end of the trampoline into the GDT tables. */
    cli
1:  hlt
    jmp 1b

.align 16
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF    /* 0x08: 32-bit code, ring 0 */
    .quad 0x00CF92000000FFFF    /* 0x10: data, ring 0 */
gdt_ptr:
    .word gdt_end - gdt - 1
    .long gdt
    .long 0
gdt_end:

.align 16
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF    /* 0x08: 64-bit code, ring 0 */
    .quad 0x00CF92000000FFFF    /* 0x10: data, ring 0 */
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64
    .long 0
gdt64_end:

.section .note.GNU-stack, "", @progbits
