#ifndef FELINOS_APIC_H
#define FELINOS_APIC_H

#include <stdint.h>
#include "idt.h"

/* Local APIC registers (offset from Local APIC base) */
#define APIC_ID         0x20    /* Local APIC ID */
#define APIC_VER        0x30    /* Local APIC Version */
#define APIC_TPR        0x80    /* Task Priority */
#define APIC_APR        0x90    /* Arbitration Priority */
#define APIC_PPR        0xA0    /* Processor Priority */
#define APIC_EOI        0xB0    /* End of Interrupt */
#define APIC_RRD        0xC0    /* Remote Read */
#define APIC_LDR        0xD0    /* Logical Destination */
#define APIC_DFR        0xE0    /* Destination Format */
#define APIC_SVR        0xF0    /* Spurious Interrupt Vector */
#define APIC_ISR        0x100   /* In-Service Register */
#define APIC_TMR        0x180   /* Trigger Mode Register */
#define APIC_IRR        0x200   /* Interrupt Request Register */
#define APIC_ESR        0x280   /* Error Status Register */
#define APIC_ICR_LOW    0x300   /* Interrupt Command Register Low */
#define APIC_ICR_HIGH   0x310   /* Interrupt Command Register High */
#define APIC_LVT_TMR    0x320   /* LVT Timer */
#define APIC_LVT_THERM  0x330   /* LVT Thermal Sensor */
#define APIC_LVT_PERF   0x340   /* LVT Performance Counter */
#define APIC_LVT_LINT0  0x350   /* LVT LINT0 */
#define APIC_LVT_LINT1  0x360   /* LVT LINT1 */
#define APIC_LVT_ERR    0x370   /* LVT Error */
#define APIC_TMR_ICR    0x380   /* Timer Initial Count */
#define APIC_TMR_CCR    0x390   /* Timer Current Count */
#define APIC_TMR_DCR    0x3E0   /* Timer Divide Configuration */

/* Spurious Interrupt Vector bits */
#define APIC_SVR_ENABLE     0x100
#define APIC_SVR_FOCUS_PROC 0x200

/* Interrupt Command Register bits.
 *
 * ICR Low layout: bits 0-7 vector, 8-10 delivery mode, 11 destination mode,
 * 12 delivery status (read-only), 13 level, 14 trigger, 18-19 destination
 * shorthand. ICR High holds the physical destination in bits 24-31. */
#define APIC_ICR_VECTOR     0x000000FF
#define APIC_ICR_DM_FIXED   0x00000000
#define APIC_ICR_DM_LOWPRI  0x00000300
#define APIC_ICR_DM_SMI     0x00000500
#define APIC_ICR_DM_NMI     0x00000400
#define APIC_ICR_DM_INIT    0x00000600
#define APIC_ICR_DM_SIPI    0x00000700
#define APIC_ICR_DEST_MODE  0x00000800
#define APIC_ICR_LEVEL      0x00002000  /* INIT: 0 = assert, 1 = deassert */
#define APIC_ICR_TRIGGER    0x00004000  /* INIT: 0 = edge, 1 = level */
#define APIC_ICR_DELIVSTAT  0x00001000
#define APIC_ICR_DEST_SHORTHAND_NONE     0x00000000
#define APIC_ICR_DEST_SHORTHAND_SELF     0x00040000
#define APIC_ICR_DEST_SHORTHAND_ALL      0x00080000
#define APIC_ICR_DEST_SHORTHAND_BUT_SELF 0x000C0000

/* LVT bits */
#define APIC_LVT_MASKED     0x00010000
#define APIC_LVT_PERIODIC   0x00020000
#define APIC_LVT_TSC_DEADLINE 0x00040000

/* Timer Divide Configuration */
#define APIC_TMR_DCR_DIV1   0x0B
#define APIC_TMR_DCR_DIV2   0x00
#define APIC_TMR_DCR_DIV4   0x01
#define APIC_TMR_DCR_DIV8   0x02
#define APIC_TMR_DCR_DIV16  0x03
#define APIC_TMR_DCR_DIV32  0x08
#define APIC_TMR_DCR_DIV64  0x09
#define APIC_TMR_DCR_DIV128 0x0A

/* I/O APIC registers */
#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_ARB      0x02
#define IOAPIC_REG_REDIR_BASE 0x10
#define IOAPIC_MAX_IRQS     24

/* Where the two indirect-access registers live in the I/O APIC's MMIO window.
 * Offset 0x00 selects the register, offset 0x10 is the register itself; the
 * redirection table proper only begins at 0x40. */
#define IOAPIC_MMIO_IOREGSEL 0x00
#define IOAPIC_MMIO_IOWIN    0x10

/* I/O APIC Redirection Table entry bits (low dword): 0-7 vector, 8-10
 * delivery mode, 11 destination mode, 13 polarity, 15 trigger mode, 16 mask.
 * The destination APIC ID lives in bits 31:24 of the *high* dword, not here. */
#define IOAPIC_REDIR_VECTOR     0x000000FF
#define IOAPIC_REDIR_DM_FIXED   0x00000000
#define IOAPIC_REDIR_DM_LOWPRI  0x00000300
#define IOAPIC_REDIR_DM_NMI     0x00000400
#define IOAPIC_REDIR_DM_SMI     0x00000500
#define IOAPIC_REDIR_DM_INIT    0x00000600
#define IOAPIC_REDIR_DM_LOGICAL 0x00000800
#define IOAPIC_REDIR_DM_PHYSICAL 0x00000000
#define IOAPIC_REDIR_DM_PENDING 0x00001000
#define IOAPIC_REDIR_DM_TRIGGER 0x00008000  /* 0 = edge, 1 = level */
#define IOAPIC_REDIR_DM_LEVEL   0x00008000
#define IOAPIC_REDIR_ACTIVE_LOW 0x00002000
/* Bit 16 is the mask, not bit 17. Setting 17 leaves the entry live, which is
 * how a redirection entry the driver believes it has parked goes on injecting
 * its vector -- and, because the I/O APIC arbitrates by vector number, it then
 * starves every lower-numbered vector behind it. */
#define IOAPIC_REDIR_DM_MASKED  0x00010000

/* Interrupt vectors */
#define APIC_TIMER_VECTOR   0x32
#define APIC_SPURIOUS_VECTOR 0xFF
#define APIC_ERROR_VECTOR   0x33
#define APIC_IPI_VECTOR     0x34
#define APIC_RESCHED_VECTOR 0x35

/* CPU info */
struct cpu_info {
    int is_bsp;             /* Bootstrap processor */
    int apic_id;            /* Local APIC ID */
    int present;
    int initialized;
    uint32_t family;
    uint32_t model;
    uint32_t features[4];   /* CPUID feature bits */
};

/* SMP state - access via getter functions */
int apic_is_present(void);
int apic_get_cpu_count(void);
uint32_t apic_get_base(void);
uint32_t apic_get_ioapic_base(void);
uint8_t apic_get_ioapic_id(void);

/* APIC functions. An application processor reaches its per-CPU setup through
 * ap_startup(), entered from the AP trampoline; there is no separate
 * init-as-AP entry point. */
void apic_init(void);
void apic_send_eoi(uint32_t vector);
void ioapic_send_eoi(uint8_t vector);
void apic_send_ipi(int apic_id, uint8_t vector, uint32_t dest_mode, uint32_t level);
void apic_start_ap(int apic_id, uint32_t start_addr);
uint32_t apic_read(uint32_t reg);
void apic_write(uint32_t reg, uint32_t value);
uint8_t apic_get_id(void);

/* I/O APIC functions */
void ioapic_init(void);
void ioapic_set_redirection(uint8_t irq, uint8_t vector, uint32_t dest_apic_id, int masked);
int ioapic_get_irq_vector(uint8_t irq);
uint32_t ioapic_read(uint32_t reg);
void ioapic_write(uint32_t reg, uint32_t value);
void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq);

/* APIC ISR handlers */
extern void apic_timer_irq(struct regs *r);
extern void apic_spurious_irq(struct regs *r);
extern void apic_error_irq(struct regs *r);
extern void apic_ipi_irq(struct regs *r);
extern void apic_resched_irq(struct regs *r);

/* SMP functions */
void smp_init(void);
void smp_boot_aps(void);
int smp_get_apic_id(void);
int smp_cpu_count(void);
int smp_cpu_online(int index);
int smp_cpu_detected(int index);
int smp_cpu_apic_id(int index);
int smp_cpu_is_bsp(int index);
int smp_is_bsp(void);
void smp_send_reschedule_ipi(void);

/* Getters */
uint32_t apic_get_ioapic_base(void);

#endif
