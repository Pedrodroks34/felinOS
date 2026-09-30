#include "drivers/rtl.h"
#include <stddef.h>
#include "drivers/pit.h"
#include "io.h"
#include "vmm.h"
#include "sched.h"

#define RTL_MMIO_SPAN 0x1000u
#define RTL_BAR_FIRST 0x10u
#define RTL_BAR_LAST  0x24u

static void (*poll_fn)(void);

static int rtl_find_io_bar(const struct pci_device *dev, uint16_t *port) {
    for (uint8_t off = RTL_BAR_FIRST; off <= RTL_BAR_LAST; off += 4) {
        uint32_t bar = pci_read_config(dev->bus, dev->slot, dev->func, off);
        if (!(bar & 1u)) {
            continue;
        }
        uint32_t addr = bar & ~3u;
        if (addr != 0 && addr <= 0xFFFFu) {
            *port = (uint16_t)addr;
            return 0;
        }
    }
    return -1;
}

static int rtl_find_mem_bar(const struct pci_device *dev, uint32_t *base) {
    for (uint8_t off = RTL_BAR_FIRST; off <= RTL_BAR_LAST; off += 4) {
        uint32_t bar = pci_read_config(dev->bus, dev->slot, dev->func, off);
        if (bar & 1u) {
            continue;
        }
        if (((bar >> 1) & 3u) == 2u) {
            if (off >= RTL_BAR_LAST) {
                break;
            }
            uint32_t high = pci_read_config(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4));
            off += 4;
            if (high != 0) {
                continue;
            }
        }
        uint32_t addr = bar & ~0xFu;
        if (addr != 0) {
            *base = addr;
            return 0;
        }
    }
    return -1;
}

static int rtl_map_mem(uint32_t base, struct rtl_regs *r) {
    volatile uint8_t *p = (volatile uint8_t *)vmm_map_physical(base, RTL_MMIO_SPAN,
                                                               VM_READ | VM_WRITE | VM_UNCACHED,
                                                               "rtl-mmio");
    if (!p) {
        return -1;
    }
    r->mmio = p;
    r->io = 0;
    return 0;
}

int rtl_map(const struct pci_device *dev, struct rtl_regs *r, int prefer_mmio) {
    uint16_t port = 0;
    uint32_t base = 0;
    int have_io = (rtl_find_io_bar(dev, &port) == 0);
    int have_mem = (rtl_find_mem_bar(dev, &base) == 0);

    r->mmio = NULL;
    r->io = 0;
    if (prefer_mmio && have_mem && rtl_map_mem(base, r) == 0) {
        return 0;
    }
    if (have_io) {
        r->mmio = NULL;
        r->io = port;
        return 0;
    }
    if (have_mem && rtl_map_mem(base, r) == 0) {
        return 0;
    }
    return -1;
}

void rtl_enable_bus_master(const struct pci_device *dev) {
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, 0x04) & 0xFFFFu;
    cmd |= 0x0007u;
    cmd &= ~0x0400u;
    pci_write_config(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

int rtl_irq_usable(const struct pci_device *dev) {
    return dev->irq >= 1 && dev->irq <= 15;
}

static void rtl_poll_thread(void *arg) {
    (void)arg;
    for (;;) {
        poll_fn();
        sleep_ms(1);
    }
}

void rtl_start_poll(void (*fn)(void)) {
    poll_fn = fn;
    kthread_create("rtl-poll", rtl_poll_thread, NULL);
}

uint8_t rtl_rd8(const struct rtl_regs *r, uint32_t off) {
    if (r->mmio) {
        return r->mmio[off];
    }
    return inb((uint16_t)(r->io + off));
}

uint16_t rtl_rd16(const struct rtl_regs *r, uint32_t off) {
    if (r->mmio) {
        return *(volatile uint16_t *)(r->mmio + off);
    }
    return inw((uint16_t)(r->io + off));
}

uint32_t rtl_rd32(const struct rtl_regs *r, uint32_t off) {
    if (r->mmio) {
        return *(volatile uint32_t *)(r->mmio + off);
    }
    return inl((uint16_t)(r->io + off));
}

void rtl_wr8(const struct rtl_regs *r, uint32_t off, uint8_t value) {
    if (r->mmio) {
        r->mmio[off] = value;
        return;
    }
    outb((uint16_t)(r->io + off), value);
}

void rtl_wr16(const struct rtl_regs *r, uint32_t off, uint16_t value) {
    if (r->mmio) {
        *(volatile uint16_t *)(r->mmio + off) = value;
        return;
    }
    outw((uint16_t)(r->io + off), value);
}

void rtl_wr32(const struct rtl_regs *r, uint32_t off, uint32_t value) {
    if (r->mmio) {
        *(volatile uint32_t *)(r->mmio + off) = value;
        return;
    }
    outl((uint16_t)(r->io + off), value);
}
