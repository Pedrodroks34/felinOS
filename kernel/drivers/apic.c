#include "drivers/apic.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "drivers/cpu.h"
#include "drivers/acpi.h"
#include "lib/string.h"
#include "io.h"
#include "vmm.h"
#include "console.h"
#include "sync.h"
#include "sched.h"
#include "idt.h"
#include "gdt.h"
#include "paging.h"
#include "acpi.h"

static void parse_madt(void);
static void apic_setup_timer(void);

#define APIC_BASE_MSR 0x1B

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

static volatile uint32_t *lapic_mmio;
static volatile uint32_t *ioapic_mmio;

static struct cpu_info cpus[16];
static int cpu_count;
static int apic_present;
static uint32_t lapic_phys_base;
static uint32_t ioapic_phys_base;
static uint8_t ioapic_id_val;

static volatile int ap_booted_count;
static uint32_t ap_entry_point;
static uint32_t ap_stack_top[16];
static uint32_t ap_pml4[16];

void apic_init(void) {
    cpu_count = 0;
    apic_present = 0;
    lapic_phys_base = 0;
    ioapic_phys_base = 0;
    ioapic_id_val = 0;

    if (!cpu_has_feature("apic")) {
        klog("apic: not present");
        return;
    }

    uint64_t apic_msr = rdmsr(APIC_BASE_MSR);
    lapic_phys_base = (uint32_t)(apic_msr & 0xFFFFF000);

    lapic_mmio = (volatile uint32_t *)vmm_map_physical(lapic_phys_base, 0x1000, VM_READ | VM_WRITE | VM_UNCACHED, "lapic");
    if (!lapic_mmio) {
        klog("apic: failed to map Local APIC at 0x%08x", lapic_phys_base);
        return;
    }

    lapic_mmio[APIC_SVR / 4] = APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR;

    uint32_t apic_id = lapic_mmio[APIC_ID / 4] >> 24;

    cpus[0].is_bsp = 1;
    cpus[0].apic_id = apic_id;
    cpus[0].present = 1;
    cpus[0].initialized = 1;
    cpu_count = 1;

    apic_present = 1;
    klog("apic: Local APIC at 0x%08x, BSP APIC ID %u", lapic_phys_base, apic_id);

    ioapic_init();
    apic_setup_timer();
}

void ioapic_init(void) {
    ioapic_phys_base = 0xFEC00000;

    ioapic_mmio = (volatile uint32_t *)vmm_map_physical(ioapic_phys_base, 0x1000, VM_READ | VM_WRITE | VM_UNCACHED, "ioapic");
    if (!ioapic_mmio) {
        klog("apic: failed to map I/O APIC at 0x%08x", ioapic_phys_base);
        return;
    }

    uint32_t id_reg = ioapic_mmio[IOAPIC_REG_ID / 4];
    ioapic_id_val = (id_reg >> 24) & 0x0F;

    klog("apic: I/O APIC at 0x%08x, ID %u", ioapic_phys_base, ioapic_id_val);

    for (int i = 0; i < IOAPIC_MAX_IRQS; i++) {
        ioapic_set_redirection(i, 0x20 + i, ioapic_id_val, 1);
    }

    ioapic_set_redirection(0, 0x20, ioapic_id_val, 0);
    ioapic_set_redirection(1, 0x21, ioapic_id_val, 0);
}

void apic_setup_timer(void) {
    lapic_mmio[APIC_LVT_TMR / 4] = APIC_LVT_MASKED | APIC_TIMER_VECTOR;
    lapic_mmio[APIC_TMR_DCR / 4] = APIC_TMR_DCR_DIV16;
    lapic_mmio[APIC_TMR_ICR / 4] = 1000000;

    lapic_mmio[APIC_LVT_ERR / 4] = APIC_ERROR_VECTOR | APIC_LVT_MASKED;
    lapic_mmio[APIC_LVT_LINT0 / 4] = APIC_LVT_MASKED;
    lapic_mmio[APIC_LVT_LINT1 / 4] = APIC_LVT_MASKED;

    /* IDT gates are set up in idt_install() */

    lapic_mmio[APIC_LVT_TMR / 4] = APIC_TIMER_VECTOR | APIC_LVT_PERIODIC;
}

/* APIC ISR C handlers called from assembly ISR stubs */
void apic_timer_irq(struct regs *r) {
    apic_write(APIC_EOI, 0);
    sched_tick();
}

void apic_spurious_irq(struct regs *r) {
}

void apic_error_irq(struct regs *r) {
    klog("apic: error interrupt");
    apic_write(APIC_EOI, 0);
}

void apic_ipi_irq(struct regs *r) {
    apic_write(APIC_EOI, 0);
}

void apic_resched_irq(struct regs *r) {
    apic_write(APIC_EOI, 0);
    schedule();
}

uint32_t apic_read(uint32_t reg) {
    if (!lapic_mmio) return 0;
    return lapic_mmio[reg / 4];
}

void apic_write(uint32_t reg, uint32_t value) {
    if (!lapic_mmio) return;
    lapic_mmio[reg / 4] = value;
}

uint32_t ioapic_read(uint32_t reg) {
    if (!ioapic_mmio) return 0;
    ioapic_mmio[0] = reg;
    return ioapic_mmio[4 / 4];
}

void ioapic_write(uint32_t reg, uint32_t value) {
    if (!ioapic_mmio) return;
    ioapic_mmio[0] = reg;
    ioapic_mmio[4 / 4] = value;
}

void apic_send_eoi(uint32_t vector) {
    if (!lapic_mmio) return;
    lapic_mmio[APIC_EOI / 4] = 0;
}

void apic_send_ipi(int apic_id, uint8_t vector, uint32_t dest_mode, uint32_t level) {
    if (!lapic_mmio) return;

    while (lapic_mmio[APIC_ICR_LOW / 4] & (1u << 12)) {
        __asm__ volatile ("pause");
    }

    lapic_mmio[APIC_ICR_HIGH / 4] = ((uint32_t)apic_id) << 24;

    uint32_t icr_low = vector | dest_mode | level | APIC_ICR_ASSERT;
    lapic_mmio[APIC_ICR_LOW / 4] = icr_low;

    while (lapic_mmio[APIC_ICR_LOW / 4] & (1u << 12)) {
        __asm__ volatile ("pause");
    }
}

void ioapic_set_redirection(uint8_t irq, uint8_t vector, uint32_t dest_apic_id, int masked) {
    if (!ioapic_mmio) return;

    uint32_t reg = IOAPIC_REG_REDIR_BASE + irq * 2;
    uint32_t low = vector;
    uint32_t high = dest_apic_id << 24;

    if (masked) {
        low |= IOAPIC_REDIR_DM_MASKED;
    }

    ioapic_write(reg, low);
    ioapic_write(reg + 1, high);
}

void ioapic_mask_irq(uint8_t irq) {
    if (!ioapic_mmio) return;
    uint32_t reg = IOAPIC_REG_REDIR_BASE + irq * 2;
    uint32_t low = ioapic_read(reg);
    low |= IOAPIC_REDIR_DM_MASKED;
    ioapic_write(reg, low);
}

void ioapic_unmask_irq(uint8_t irq) {
    if (!ioapic_mmio) return;
    uint32_t reg = IOAPIC_REG_REDIR_BASE + irq * 2;
    uint32_t low = ioapic_read(reg);
    low &= ~IOAPIC_REDIR_DM_MASKED;
    ioapic_write(reg, low);
}

int ioapic_get_irq_vector(uint8_t irq) {
    if (!ioapic_mmio) return -1;
    uint32_t reg = IOAPIC_REG_REDIR_BASE + irq * 2;
    return ioapic_read(reg) & 0xFF;
}

uint8_t apic_get_id(void) {
    if (!lapic_mmio) return 0;
    return (uint8_t)(lapic_mmio[APIC_ID / 4] >> 24);
}

int smp_cpu_count(void) {
    return cpu_count;
}

int smp_is_bsp(void) {
    return cpus[0].is_bsp;
}

int smp_get_apic_id(void) {
    return apic_get_id();
}

void smp_init(void) {
    if (!apic_present) return;

    const struct acpi_info *ai = acpi_get_info();
    if (ai->present) {
        parse_madt();
    }

    klog("smp: %d CPU(s) detected", cpu_count);
}

void parse_madt(void) {
    const struct acpi_table_ref *madt_ref = NULL;
    for (int i = 0; i < acpi_table_count(); i++) {
        const struct acpi_table_ref *t = acpi_get_table(i);
        if (t && memcmp(t->sig, "APIC", 4) == 0) {
            madt_ref = t;
            break;
        }
    }

    if (!madt_ref) return;

    const void *madt = vmm_map_physical(madt_ref->phys, madt_ref->length, VM_READ, "acpi-madt");
    if (!madt) return;

    const uint8_t *ptr = (const uint8_t *)madt + sizeof(struct acpi_header) + 8;
    const uint8_t *end = (const uint8_t *)madt + madt_ref->length;

    while (ptr + 2 <= end) {
        uint8_t type = ptr[0];
        uint8_t len = ptr[1];

        if (ptr + len > end) break;

        if (type == 0 && len >= 8) {
            uint8_t acpi_id = ptr[3];
            uint8_t apic_id = ptr[4];
            uint32_t flags = *(uint32_t *)(ptr + 4);

            if (flags & 1) {
                int found = 0;
                for (int i = 0; i < cpu_count; i++) {
                    if (cpus[i].apic_id == apic_id) {
                        found = 1;
                        break;
                    }
                }
                if (!found && cpu_count < 16) {
                    cpus[cpu_count].is_bsp = 0;
                    cpus[cpu_count].apic_id = apic_id;
                    cpus[cpu_count].present = 1;
                    cpus[cpu_count].initialized = 0;
                    cpu_count++;
                    klog("smp: found AP with APIC ID %u (ACPI ID %u)", apic_id, acpi_id);
                }
            }
        } else if (type == 1 && len >= 8) {
            ioapic_id_val = ptr[3];
            ioapic_phys_base = *(uint32_t *)(ptr + 4);
            klog("smp: I/O APIC at 0x%08x, ID %u", ioapic_phys_base, ioapic_id_val);
        } else if (type == 2 && len >= 10) {
            uint8_t bus = ptr[3];
            uint8_t irq = ptr[4];
            uint32_t gsi = *(uint32_t *)(ptr + 4);
            uint16_t flags = *(uint16_t *)(ptr + 8);
            klog("smp: interrupt override: bus %u, irq %u -> gsi %u, flags %x", bus, irq, gsi, flags);
        }

        ptr += len;
    }

    vmm_free((void *)madt);
}

void smp_boot_aps(void) {
    if (!apic_present) return;

    klog("smp: booting application processors not implemented (trampoline not linked)");
    return;
}

uint32_t apic_get_base(void) {
    return lapic_phys_base;
}

uint32_t apic_get_ioapic_base(void) {
    return ioapic_phys_base;
}

int apic_is_present(void) {
    return apic_present;
}

int apic_get_cpu_count(void) {
    return cpu_count;
}

uint8_t apic_get_ioapic_id(void) {
    return ioapic_id_val;
}

void smp_send_reschedule_ipi(void) {
    if (!apic_present) return;
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].present && cpus[i].initialized && !cpus[i].is_bsp) {
            apic_send_ipi(cpus[i].apic_id, APIC_RESCHED_VECTOR, APIC_ICR_DM_FIXED, APIC_ICR_ASSERT);
        }
    }
}