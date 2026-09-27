#ifndef FELINOS_APIC_H
#define FELINOS_APIC_H

#include <stdint.h>

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

/* Interrupt Command Register bits */
#define APIC_ICR_VECTOR     0x000000FF
#define APIC_ICR_DM_FIXED   0x00000000
#define APIC_ICR_DM_LOWPRI  0x00001000
#define APIC_ICR_DM_SMI     0x00002000
#define APIC_ICR_DM_NMI     0x00004000
#define APIC_ICR_DM_INIT    0x00005000
#define APIC_ICR_DM_SIPI    0x00006000
#define APIC_ICR_DEST_MODE  0x00000800
#define APIC_ICR_LEVEL     0x00004000
#define APIC_ICR_ASSERT    0x00004000
#define APIC_ICR_DEASSERT  0x00000000
#define APIC_ICR_DEST_SHORTHAND_NONE 0x00000000
#define APIC_ICR_DEST_SHORTHAND_SELF 0x00040000
#define APIC_ICR_DEST_SHORTHAND_ALL   0x00080000
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

/* I/O APIC Redirection Table bits */
#define IOAPIC_REDIR_VECTOR     0x000000FF
#define IOAPIC_REDIR_DM_FIXED   0x00000000
#define IOAPIC_REDIR_DM_LOWPRI  0x00002000
#define IOAPIC_REDIR_DM_SMI     0x00004000
#define IOAPIC_REDIR_DM_NMI     0x00008000
#define IOAPIC_REDIR_DM_INIT    0x00005000
#define IOAPIC_REDIR_DM_LOGICAL 0x00000800
#define IOAPIC_REDIR_DM_PHYSICAL 0x00000000
#define IOAPIC_REDIR_DM_PENDING 0x00001000
#define IOAPIC_REDIR_DM_MASKED  0x00010000
#define IOAPIC_REDIR_DM_TRIGGER 0x00004000

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

/* APIC functions */
void apic_init(void);
void apic_init_as_ap(void);
void apic_send_eoi(uint32_t vector);
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

/* SMP functions */
void smp_init(void);
void smp_boot_aps(void);
int smp_get_apic_id(void);
int smp_cpu_count(void);
int smp_is_bsp(void);
void smp_send_reschedule_ipi(void);

/* Getters */
uint32_t apic_get_base(void);
uint32_t apic_get_ioapic_base(void);

#endif
