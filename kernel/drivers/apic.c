#include "drivers/apic.h"
#include "drivers/pic.h"
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
#include "fpu.h"
#include "lib/heap.h"

static void parse_madt(void);
static void apic_setup_timer(void);

/* Runs on an application processor, entered from the AP trampoline. */
void ap_startup(void);

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

/* The application-processor entry code, linked at a fixed low address by
 * linker.ld. It is not copied anywhere: the kernel identity maps that page
 * and the 16-bit offsets inside the blob are absolute linear addresses. */
extern void ap_trampoline_start(void);

/* Low physical pages the bootstrap processor hands to the APs. 0x7000 holds
 * the handover block and 0x8000 the entry code; both sit below 1 MiB, where
 * the firmware leaves RAM alone and a real-mode entry point can reach them. */
#define AP_HANDOVER_PHYS   0x7000u
#define AP_TRAMPOLINE_PHYS 0x8000u
#define AP_STACK_SIZE      16384u

struct ap_handover {
    uint64_t pml4;       /* physical address of the kernel PML4 */
    uint64_t stack_top;  /* the kernel is identity mapped, so physical is it */
};

/* Written by the bootstrap processor and read by the trampoline before it
 * has page tables of its own, so it cannot be an ordinary variable. */
static struct ap_handover ap_handover_block __attribute__((section(".ap_handover"), used, aligned(8)));

static volatile int ap_booted_count;

/* Set by each AP from inside ap_startup(), indexed by slot rather than by
 * APIC id so a stray processor cannot scribble outside the table. */
static volatile int ap_online[16];

/* Spins through a volatile counter so the delay survives optimisation. */
static void ap_delay(uint32_t count) {
    for (volatile uint32_t i = 0; i < count; i++) {
        __asm__ volatile ("pause");
    }
}

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

    /* Setting the Local APIC's own SVR bit is not by itself a statement to the
     * CPU that interrupts are routed through the APIC; IA32_APIC_BASE is. Under
     * -kernel no firmware did it for us. Set the two writable enable bits and
     * keep the base address we just read. Bit 10 (x2APIC enable) is
     * deliberately left alone: it is read-only, and writing it makes the CPU
     * raise #GP. */
    apic_msr |= (1ULL << 8) | (1ULL << 11);
    wrmsr(APIC_BASE_MSR, apic_msr);

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

    /* The 8259s are off the delivery path from here on; make sure they cannot
     * keep the cascade line asserted behind the I/O APIC's back. */
    pic_disable();

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

    /* Go through the index/data pair: offset 0 is the index register, so
     * reading it directly yields whatever was last selected rather than the
     * ID register's contents. */
    uint32_t id_reg = ioapic_read(IOAPIC_REG_ID);
    ioapic_id_val = (id_reg >> 24) & 0x0F;

    klog("apic: I/O APIC at 0x%08x, ID %u", ioapic_phys_base, ioapic_id_val);

    /* Redirection entries name the *destination* Local APIC, which is not the
     * I/O APIC's own ID: sending a timer tick to the wrong APIC means it is
     * never delivered. Route everything to the BSP (set up in apic_init). */
    uint8_t dest = apic_get_id();

    /* Only the 16 legacy 8259 lines get a vector, laid out 0x20..0x2F to match
     * the irq0..irq15 stubs. Extending the 0x20+i pattern over the whole table
     * reached 0x37, where 0x32..0x35 are the *local* APIC's timer, error, IPI
     * and reschedule vectors: a stray interrupt on entries 18..21 would land
     * on the APIC's own handler, and 0x30/0x31/0x36/0x37 have no gate at all,
     * so the CPU would take a #GP from an interrupt it could not have
     * predicted. Everything above IRQ 15 is left masked with vector 0. */
    for (int i = 0; i < IOAPIC_MAX_IRQS; i++) {
        if (i < 16) {
            ioapic_set_redirection(i, 0x20 + i, dest, 1);
        } else {
            ioapic_set_redirection(i, 0, dest, 1);
        }
    }

    ioapic_set_redirection(0, 0x20, dest, 0);
    ioapic_set_redirection(1, 0x21, dest, 0);
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

    /* The timer is armed and unmasked now, so it becomes the tick source. */
    clockevent_select(CLOCKEVENT_LAPIC_TIMER);
}

/* APIC ISR C handlers called from assembly ISR stubs */
void apic_timer_irq(struct regs *r) {
    apic_write(APIC_EOI, 0);
    if (clockevent_current() == CLOCKEVENT_LAPIC_TIMER) {
        clockevent_tick();
    } else {
        sched_tick();
    }
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
    /* The register file is reached indirectly: write the register number to
     * IOREGSEL at offset 0x00, then read the data window at offset 0x10. The
     * data window is *not* at offset 0x04 -- that address is reserved, so
     * reading and writing there selected a register and then threw the value
     * away, which left every redirection entry in its power-on state and
     * meant no I/O APIC interrupt was ever delivered. */
    ioapic_mmio[IOAPIC_MMIO_IOREGSEL / 4] = reg;
    return ioapic_mmio[IOAPIC_MMIO_IOWIN / 4];
}

void ioapic_write(uint32_t reg, uint32_t value) {
    if (!ioapic_mmio) return;
    ioapic_mmio[IOAPIC_MMIO_IOREGSEL / 4] = reg;
    ioapic_mmio[IOAPIC_MMIO_IOWIN / 4] = value;
}

/* The I/O APIC keeps in-service bits of its own. They are cleared by writing
 * the vector to the I/O APIC's EOI register, which shares offset 0x00 with the
 * ID register: reads see the ID, writes are the EOI. The Local APIC EOI does
 * not touch them, so skipping this leaves the redirection entry stuck in
 * service and the I/O APIC stops injecting that vector -- and, because it
 * arbitrates by vector number, any lower-numbered vector behind it. That is
 * why the PIT (vector 0x20) never arrived while the storm on 0x22 did. */
void ioapic_send_eoi(uint8_t vector) {
    if (!ioapic_mmio) return;
    ioapic_write(IOAPIC_REG_ID, vector);
}

void apic_send_eoi(uint32_t vector) {
    if (!lapic_mmio) return;
    lapic_mmio[APIC_EOI / 4] = 0;
}

/* Raw ICR write. `delivery` is one of the APIC_ICR_DM_* modes and `target` is
 * a physical destination APIC id, so callers are not tempted to pass
 * already-shifted bit soup around. */
static void apic_ipi_raw(uint8_t vector, uint32_t delivery, uint32_t target) {
    if (!lapic_mmio) return;

    while (lapic_mmio[APIC_ICR_LOW / 4] & APIC_ICR_DELIVSTAT) {
        __asm__ volatile ("pause");
    }

    lapic_mmio[APIC_ICR_HIGH / 4] = target << 24;
    /* ICR Low has to be written with a single 32-bit store. */
    lapic_mmio[APIC_ICR_LOW / 4] = (uint32_t)vector | delivery;

    while (lapic_mmio[APIC_ICR_LOW / 4] & APIC_ICR_DELIVSTAT) {
        __asm__ volatile ("pause");
    }
}

void apic_send_ipi(int apic_id, uint8_t vector, uint32_t dest_mode, uint32_t level) {
    if (!lapic_mmio) return;
    apic_ipi_raw(vector, APIC_ICR_DM_FIXED, (uint32_t)apic_id);
}

void ioapic_set_redirection(uint8_t irq, uint8_t vector, uint32_t dest_apic_id, int masked) {
    if (!ioapic_mmio) return;

    uint32_t reg = IOAPIC_REG_REDIR_BASE + irq * 2;
    uint32_t low = vector;
    uint32_t high = dest_apic_id << 24;

    if (masked) {
        low |= IOAPIC_REDIR_DM_MASKED;
    }
    /* The 8254 holds OUT0 asserted for the whole terminal count period, and
     * the I/O APIC drops a pending request as soon as the line falls, so an
     * entry left on edge trigger loses the tick whenever delivery is not
     * immediate. Everything else here is a genuine edge source. */
    if (irq == 0) {
        low |= IOAPIC_REDIR_DM_LEVEL;
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

int smp_cpu_online(int index) {
    if (index < 0 || index >= cpu_count) return 0;
    return cpus[index].initialized ? 1 : 0;
}

int smp_cpu_apic_id(int index) {
    if (index < 0 || index >= cpu_count) return -1;
    return cpus[index].apic_id;
}

int smp_cpu_is_bsp(int index) {
    if (index < 0 || index >= cpu_count) return 0;
    return cpus[index].is_bsp ? 1 : 0;
}

/* Present in the firmware tables, whether or not it answered. */
int smp_cpu_detected(int index) {
    if (index < 0 || index >= cpu_count) return 0;
    return cpus[index].present ? 1 : 0;
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

/* MADT entry layouts (all fields at the offset shown):
 *   type 0  Processor Local APIC:  type len acpi_id apic_id flags(4)
 *   type 1  I/O APIC:             type len id    reserved address(8)
 *   type 2  Interrupt Source Override: type len src_bus src_irq gsi(4) flags(2)
 *   type 5  Local APIC NMI:       type len acpi_id flags(2) apic_id(4)
 *   type 9  x2APIC entry:         type len reserved acpi_id apic_id(4) flags(4) ...
 */
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

        if (len < 2 || ptr + len > end) break;

        if (type == 0 && len >= 8) {
            uint8_t acpi_id = ptr[2];
            uint8_t apic_id = ptr[3];
            uint32_t flags = *(uint32_t *)(ptr + 4);

            /* bit 0 = enabled, bit 1 = online capable */
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
        } else if (type == 1 && len >= 12) {
            ioapic_id_val = ptr[2];
            /* The address is 8 bytes; only the low half is ever mapped. */
            ioapic_phys_base = (uint32_t)ptr[4] | ((uint32_t)ptr[5] << 8) |
                               ((uint32_t)ptr[6] << 16) | ((uint32_t)ptr[7] << 24);
            if (ioapic_phys_base)
                klog("smp: I/O APIC at 0x%08x, ID %u", ioapic_phys_base, ioapic_id_val);
        } else if (type == 2 && len >= 10) {
            uint8_t bus = ptr[2];
            uint8_t irq = ptr[3];
            uint32_t gsi = *(uint32_t *)(ptr + 4);
            uint16_t flags = *(uint16_t *)(ptr + 8);
            klog("smp: interrupt override: bus %u, irq %u -> gsi %u, flags %x", bus, irq, gsi, flags);
        } else if (type == 9 && len >= 16) {
            /* x2APIC entries carry a 32-bit APIC ID. Gato maps the LAPIC in
             * 4-byte registers, so an id above 255 cannot be addressed and
             * such a processor is left out. */
            uint8_t acpi_id = ptr[3];
            uint32_t apic_id = *(uint32_t *)(ptr + 4);
            uint32_t flags = *(uint32_t *)(ptr + 8);

            if ((flags & 1) && apic_id <= 0xFF) {
                int found = 0;
                for (int i = 0; i < cpu_count; i++) {
                    if (cpus[i].apic_id == (int)apic_id) {
                        found = 1;
                        break;
                    }
                }
                if (!found && cpu_count < 16) {
                    cpus[cpu_count].is_bsp = 0;
                    cpus[cpu_count].apic_id = (int)apic_id;
                    cpus[cpu_count].present = 1;
                    cpus[cpu_count].initialized = 0;
                    cpu_count++;
                    klog("smp: found AP with APIC ID %u (x2APIC, ACPI ID %u)", apic_id, acpi_id);
                }
            }
        }

        ptr += len;
    }

    vmm_free((void *)madt);
}

/* Called from the trampoline once the AP is in long mode with the kernel
 * PML4 loaded. This runs on an application processor, never on the BSP. */
void ap_startup(void) {
    int slot = -1;

    /* The MMIO window was mapped for the BSP, but each processor has its own
     * local APIC, so the window has to be re-established here. */
    if (!apic_present || !lapic_phys_base) {
        klog("smp: AP started without an APIC, parking");
        goto park;
    }

    lapic_mmio = (volatile uint32_t *)vmm_map_physical(lapic_phys_base, 0x1000,
                                                      VM_READ | VM_WRITE | VM_UNCACHED,
                                                      "lapic-ap");
    if (!lapic_mmio) {
        klog("smp: AP could not map its own LAPIC, parking");
        goto park;
    }

    /* Adopt the kernel GDT: the trampoline set up a private one that has
     * neither a TSS nor any ring 3 descriptors. */
    gdt_reload();
    fpu_init();

    /* The I/O APIC is a single shared chip, so its redirection table is set up
     * once by the bootstrap processor. Re-running ioapic_init() from here
     * re-masked every entry the BSP had opened (including the disk IRQs
     * irq_install_handler() unmasked) and re-mapped the window, so the APs
     * silently undid the interrupt setup. */

    /* This processor's IA32_APIC_BASE comes out of INIT-SIPI-SIPI at its reset
     * value, so the APIC has to be enabled here too or no interrupt routed to
     * this CPU can be delivered. Bit 8 is the bootstrap-processor enable and
     * means nothing on an AP; bit 11 is the global enable. Bit 10 (xAPIC) is
     * left alone: it is read-only, and writing it raises #GP. */
    uint64_t ap_msr = rdmsr(APIC_BASE_MSR);
    ap_msr |= (1ULL << 11);
    wrmsr(APIC_BASE_MSR, ap_msr);

    /* Enable this LAPIC, with the timer masked and the error vector in
     * place. An AP does not drive the timekeeping clock, so the local timer
     * stays off rather than stealing ticks from the bootstrap processor. */
    lapic_mmio[APIC_SVR / 4] = APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR;
    lapic_mmio[APIC_LVT_TMR / 4] = APIC_LVT_MASKED | APIC_TIMER_VECTOR;
    lapic_mmio[APIC_LVT_ERR / 4] = APIC_LVT_MASKED | APIC_ERROR_VECTOR;
    lapic_mmio[APIC_LVT_LINT0 / 4] = APIC_LVT_MASKED;
    lapic_mmio[APIC_LVT_LINT1 / 4] = APIC_LVT_MASKED;

    /* Publish the state before bumping the counter, so the bootstrap
     * processor never sees a count that outruns the tables it reads. */
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].is_bsp || cpus[i].initialized) continue;
        if (cpus[i].apic_id != apic_get_id()) continue;
        cpus[i].initialized = 1;
        if (i < (int)(sizeof(ap_online) / sizeof(ap_online[0]))) {
            ap_online[i] = 1;
        }
        slot = i;
        break;
    }

    __asm__ volatile ("mfence" ::: "memory");
    ap_booted_count++;

    if (slot >= 0) {
        klog("smp: AP %d online, APIC ID %u", slot, apic_get_id());
    } else {
        klog("smp: an unlisted AP came up, APIC ID %u", apic_get_id());
    }

park:
    /* The scheduler still has a single global run queue owned by the
     * bootstrap processor, so an AP has nothing to run. It stays in a
     * low-power wait, still able to take interrupts, which is what lets the
     * machine be described honestly as having the processors online. */
    for (;;) {
        __asm__ volatile ("sti; hlt");
    }
}

/* Brings up every AP the MADT advertised. One that fails to report in is
 * left marked uninitialized instead of taking the system down, so a bad
 * firmware table degrades to single-processor operation. */
void smp_boot_aps(void) {
    if (!apic_present) {
        return;
    }
    if (cpu_count <= 1) {
        klog("smp: only the bootstrap processor was found");
        return;
    }

    struct address_space *kspace = paging_kernel_space();
    if (!kspace || !kspace->pd_phys) {
        klog("smp: no kernel address space, cannot start APs");
        return;
    }

    void *stack = kmalloc(AP_STACK_SIZE);
    if (!stack) {
        klog("smp: cannot allocate an AP stack");
        return;
    }
    ap_handover_block.pml4 = kspace->pd_phys;
    ap_handover_block.stack_top = (uint64_t)(uintptr_t)stack;

    /* A SIPI names the 4 KiB page its target should begin fetching from. */
    uint8_t vector = (uint8_t)(AP_TRAMPOLINE_PHYS >> 12);
    int started = 0;

    for (int i = 1; i < cpu_count; i++) {
        /* INIT asserts the reset line: the AP lands in real mode at the
         * architectural reset vector with interrupts masked. */
        apic_ipi_raw(0, APIC_ICR_DM_INIT, (uint32_t)cpus[i].apic_id);
        ap_delay(100000);

        /* Two SIPIs, because the first can be dropped while the AP is still
         * coming out of reset. The pause between them is part of the
         * handshake rather than a guess. */
        apic_ipi_raw(vector, APIC_ICR_DM_SIPI, (uint32_t)cpus[i].apic_id);
        ap_delay(100000);
        apic_ipi_raw(vector, APIC_ICR_DM_SIPI, (uint32_t)cpus[i].apic_id);
        started++;
    }

    /* Bounded wait: an AP that never reports must not stall the boot. */
    for (uint32_t waited = 0; waited < 20000000u; waited++) {
        if (ap_booted_count >= started) {
            break;
        }
        __asm__ volatile ("pause");
    }

    int online = 0;
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].initialized) online++;
    }
    klog("smp: %d of %d processor(s) online", online, cpu_count);
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
            apic_ipi_raw(APIC_RESCHED_VECTOR, APIC_ICR_DM_FIXED, (uint32_t)cpus[i].apic_id);
        }
    }
}