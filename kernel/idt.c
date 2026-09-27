#include <stdint.h>
#include "idt.h"
#include "console.h"
#include "paging.h"
#include "vmm.h"
#include "drivers/vga.h"
#include "drivers/pic.h"
#include "lib/string.h"
#include "io.h"
#include "user.h"
#include "sched.h"
#include "drivers/apic.h"

#define HI(v) ((uint32_t)((uint64_t)(v) >> 32))
#define LO(v) ((uint32_t)(v))

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;
static irq_handler_t irq_routines[16];

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void isr128(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

extern void isr50(void);  /* APIC timer */
extern void isr255(void); /* APIC spurious */
extern void isr51(void);  /* APIC error */
extern void isr52(void);  /* APIC IPI */
extern void isr53(void);  /* APIC reschedule */

static const char *exception_names[32] = {
    "Divide by zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Overflow",
    "Bound range exceeded",
    "Invalid opcode",
    "Device not available",
    "Double fault",
    "Coprocessor segment overrun",
    "Invalid TSS",
    "Segment not present",
    "Stack-segment fault",
    "General protection fault",
    "Page fault",
    "Reserved",
    "x87 floating-point exception",
    "Alignment check",
    "Machine check",
    "SIMD floating-point exception",
    "Virtualization exception",
    "Control protection exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor injection exception",
    "VMM communication exception",
    "Security exception",
    "Reserved",
    "Reserved"
};

static void idt_set_gate(uint8_t num, void (*fn)(void), uint16_t sel, uint8_t flags) {
    uint64_t base = (uint64_t)(uintptr_t)fn;
    idt[num].base_low  = (uint16_t)(base & 0xFFFF);
    idt[num].base_mid  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].base_high = (uint32_t)(base >> 32);
    idt[num].sel       = sel;
    idt[num].ist       = 0;
    idt[num].flags     = flags;
    idt[num].reserved  = 0;
}

void idt_install(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    memset(idt, 0, sizeof(idt));
    memset(irq_routines, 0, sizeof(irq_routines));

    void (*isrs[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    void (*irqs[16])(void) = {
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    };

    for (int i = 0; i < 32; i++) {
        idt_set_gate((uint8_t)i, isrs[i], 0x08, 0x8E);
    }
    idt_set_gate(128, isr128, 0x08, 0xEE);
    for (int i = 0; i < 16; i++) {
        idt_set_gate((uint8_t)(32 + i), irqs[i], 0x08, 0x8E);
    }

    /* APIC ISR handlers */
    idt_set_gate(50, isr50, 0x08, 0x8E);   /* APIC timer */
    idt_set_gate(51, isr51, 0x08, 0x8E);   /* APIC error */
    idt_set_gate(52, isr52, 0x08, 0x8E);   /* APIC IPI */
    idt_set_gate(53, isr53, 0x08, 0x8E);   /* APIC reschedule */
    idt_set_gate(255, isr255, 0x08, 0x8E); /* APIC spurious */

    __asm__ volatile ("lidt (%0)" : : "r"(&idtp));
}

void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq < 0 || irq > 15) {
        return;
    }
    irq_routines[irq] = handler;
    if (apic_is_present()) {
        /* With the APIC enabled the legacy 8259 no longer reaches the CPU: the
         * line is delivered through the matching I/O APIC redirection entry.
         * Unmasking only the PIC (as this function used to) installed a handler
         * that could never be called, so both controllers are left masked and
         * the I/O APIC is the only delivery path. */
        ioapic_unmask_irq((uint8_t)irq);
    } else {
        pic_clear_mask(irq);
    }
}

void irq_uninstall_handler(int irq) {
    if (irq < 0 || irq > 15) {
        return;
    }
    irq_routines[irq] = NULL;
    if (apic_is_present()) {
        ioapic_mask_irq((uint8_t)irq);
    } else {
        pic_set_mask(irq);
    }
}

/* Defined further down, next to the rest of the APIC glue. */
void isr50_handler(struct regs *r);
void isr51_handler(struct regs *r);
void isr52_handler(struct regs *r);
void isr53_handler(struct regs *r);
void isr255_handler(struct regs *r);

void isr_handler(struct regs *r) {
    const char *name = (r->int_no < 32) ? exception_names[r->int_no] : "Unknown";
    uint32_t cr2 = 0;

    if (r->int_no == 128) {
        syscall_dispatch(r);
        return;
    }

    /* The APIC vectors share isr_common with the CPU exceptions, so they
     * have to be recognised here. Without this the first timer tick lands
     * in the panic path below. */
    switch (r->int_no) {
    case APIC_TIMER_VECTOR:   isr50_handler(r);  return;
    case APIC_ERROR_VECTOR:   isr51_handler(r);  return;
    case APIC_IPI_VECTOR:     isr52_handler(r);  return;
    case APIC_RESCHED_VECTOR: isr53_handler(r);  return;
    case APIC_SPURIOUS_VECTOR:isr255_handler(r); return;
    default: break;
    }

    if (r->int_no == 14) {
        cr2 = paging_fault_address();
        if (vmm_handle_fault(cr2, r->err_code) == 0) {
            return;
        }
    }

    if ((r->cs & 3) == 3) {
        user_fault(r, name, cr2);
    }

    console_set_panic();
    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n\n KERNEL PANIC -- Gato \n");
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("\nException %u: %s\n", (uint32_t)r->int_no, name);
    kprintf("error=%08x  rip=%08x%08x  cs=%04x  rflags=%08x\n",
            (uint32_t)r->err_code, HI(r->rip), LO(r->rip), (uint32_t)r->cs, (uint32_t)r->rflags);
    kprintf("rax=%08x%08x rbx=%08x%08x rcx=%08x%08x\n", HI(r->rax), LO(r->rax), HI(r->rbx), LO(r->rbx), HI(r->rcx), LO(r->rcx));
    kprintf("rdx=%08x%08x rsi=%08x%08x rdi=%08x%08x\n", HI(r->rdx), LO(r->rdx), HI(r->rsi), LO(r->rsi), HI(r->rdi), LO(r->rdi));
    kprintf("rbp=%08x%08x rsp=%08x%08x\n", HI(r->rbp), LO(r->rbp), HI(r->rsp), LO(r->rsp));

    if (r->int_no == 14) {
        char detail[80];
        uint64_t cr3;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
        kprintf("faulting address: %08x  cr3=%08x (%s, %s, %s)\n", cr2, (uint32_t)cr3,
                (r->err_code & 1) ? "protection violation" : "not present",
                (r->err_code & 2) ? "write" : "read",
                (r->err_code & 4) ? "user mode" : "kernel mode");
        vmm_describe(cr2, detail, sizeof(detail));
        kprintf("%s\n", detail);
    }

    kprintf("\nSystem halted.\n");
    cli();
    for (;;) {
        hlt();
    }
}

void irq_handler(struct regs *r) {
    int irq = (int)r->int_no - 32;

    if (irq >= 0 && irq < 16) {
        irq_handler_t handler = irq_routines[irq];

        irq_enter();
        if (handler) {
            handler(r);
        }
        /* Clear the interrupt at the controller that delivered it. In APIC
         * mode the Local APIC's in-service bit for this vector is what stops
         * further interrupts; the legacy 8259 is off the delivery path and its
         * EOI is not only useless but comes too late -- with it, the first
         * interrupt latched the vector forever. apic_send_eoi() takes the
         * vector, not the IRQ number. */
        if (apic_is_present()) {
            apic_send_eoi((uint32_t)r->int_no);
            ioapic_send_eoi((uint8_t)r->int_no);
        } else {
            pic_send_eoi(irq);
        }
        irq_exit();
    }
    sched_irq_return(r);
}

/* APIC ISR handlers */
void isr50_handler(struct regs *r) {
    apic_timer_irq(r);
    sched_irq_return(r);
}

void isr51_handler(struct regs *r) {
    apic_error_irq(r);
    sched_irq_return(r);
}

void isr52_handler(struct regs *r) {
    apic_ipi_irq(r);
    sched_irq_return(r);
}

void isr53_handler(struct regs *r) {
    apic_resched_irq(r);
    sched_irq_return(r);
}

void isr255_handler(struct regs *r) {
    apic_spurious_irq(r);
    sched_irq_return(r);
}
