#include "drivers/apic.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "drivers/cpu.h"
#include "lib/string.h"
#include "io.h"
#include "vmm.h"
#include "console.h"
#include "sync.h"
#include "sched.h"
#include "idt.h"

#define APIC_BASE_MSR 0x1B

/* MSR access helpers */
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

/* Local APIC registers are accessed via memory-mapped I/O */
static volatile uint32_t *lapic_mmio;
static volatile uint32_t *ioapic_mmio;

/* CPU state */
static struct cpu_info cpus[16];
static int cpu_count;
static int apic_present;
static uint32_t lapic_phys_base;
static uint32_t ioapic_phys_base;
static uint8_t ioapic_id_val;

void apic_init(void) {
    cpu_count = 0;
    apic_present = 0;
    lapic_phys_base = 0;
    ioapic_phys_base = 0;
    ioapic_id_val = 0;

    /* Check if APIC is present */
    if (!cpu_has_feature("apic")) {
        klog("apic: not present");
        return;
    }

    /* Get Local APIC base from MSR */
    uint64_t apic_msr = rdmsr(APIC_BASE_MSR);
    lapic_phys_base = (uint32_t)(apic_msr & 0xFFFFF000);

    /* Map Local APIC */
    lapic_mmio = (volatile uint32_t *)vmm_map_physical(lapic_phys_base, 0x1000, VM_READ | VM_WRITE | VM_UNCACHED, "lapic");
    if (!lapic_mmio) {
        klog("apic: failed to map Local APIC at 0x%08x", lapic_phys_base);
        return;
    }

    /* Enable Local APIC (set spurious interrupt vector) */
    lapic_mmio[APIC_SVR / 4] = APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR;

    /* Get BSP APIC ID */
    uint32_t apic_id = lapic_mmio[APIC_ID / 4] >> 24;

    /* Initialize BSP */
    cpus[0].is_bsp = 1;
    cpus[0].apic_id = apic_id;
    cpus[0].present = 1;
    cpus[0].initialized = 1;
    cpu_count = 1;

    apic_present = 1;
    klog("apic: Local APIC at 0x%08x, BSP APIC ID %u", lapic_phys_base, apic_id);

    /* Initialize I/O APIC */
    ioapic_init();
}

void ioapic_init(void) {
    /* Find I/O APIC via ACPI or PCI */
    /* For now, use default I/O APIC base */
    ioapic_phys_base = 0xFEC00000;

    /* Map I/O APIC */
    ioapic_mmio = (volatile uint32_t *)vmm_map_physical(ioapic_phys_base, 0x1000, VM_READ | VM_WRITE | VM_UNCACHED, "ioapic");
    if (!ioapic_mmio) {
        klog("apic: failed to map I/O APIC at 0x%08x", ioapic_phys_base);
        return;
    }

    /* Get I/O APIC ID */
    uint32_t id_reg = ioapic_mmio[IOAPIC_REG_ID / 4];
    ioapic_id_val = (id_reg >> 24) & 0x0F;

    klog("apic: I/O APIC at 0x%08x, ID %u", ioapic_phys_base, ioapic_id_val);
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

    /* Wait for ICR to be idle */
    while (lapic_mmio[APIC_ICR_LOW / 4] & (1u << 12)) {
        __asm__ volatile ("pause");
    }

    /* Set destination */
    lapic_mmio[APIC_ICR_HIGH / 4] = ((uint32_t)apic_id) << 24;

    /* Send IPI */
    uint32_t icr_low = vector | dest_mode | level | APIC_ICR_ASSERT;
    lapic_mmio[APIC_ICR_LOW / 4] = icr_low;

    /* Wait for IPI to be sent */
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

    /* Parse ACPI MADT to find all CPUs */
    /* For now, just use the BSP */
    klog("smp: %d CPU(s) detected", cpu_count);
}

void smp_boot_aps(void) {
    if (!apic_present) return;

    /* Boot application processors */
    /* This requires setting up a trampoline in low memory */
    klog("smp: booting application processors...");

    /* For now, just report BSP */
    klog("smp: only BSP running (AP boot not fully implemented)");
}

void smp_send_reschedule_ipi(void) {
    if (!apic_present) return;
    apic_send_ipi(0, APIC_RESCHED_VECTOR, APIC_ICR_DM_FIXED, APIC_ICR_ASSERT);
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
