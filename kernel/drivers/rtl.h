#ifndef FELINOS_RTL_H
#define FELINOS_RTL_H

#include <stdint.h>
#include "drivers/pci.h"

#define RTL_VENDOR_ID 0x10ECu

struct rtl_regs {
    volatile uint8_t *mmio;
    uint16_t io;
};

int rtl_map(const struct pci_device *dev, struct rtl_regs *r, int prefer_mmio);
void rtl_enable_bus_master(const struct pci_device *dev);
int rtl_irq_usable(const struct pci_device *dev);
void rtl_start_poll(void (*fn)(void));

uint8_t rtl_rd8(const struct rtl_regs *r, uint32_t off);
uint16_t rtl_rd16(const struct rtl_regs *r, uint32_t off);
uint32_t rtl_rd32(const struct rtl_regs *r, uint32_t off);
void rtl_wr8(const struct rtl_regs *r, uint32_t off, uint8_t value);
void rtl_wr16(const struct rtl_regs *r, uint32_t off, uint16_t value);
void rtl_wr32(const struct rtl_regs *r, uint32_t off, uint32_t value);

#endif
