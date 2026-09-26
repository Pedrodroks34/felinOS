#include <stdint.h>
#include "gdt.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3, iomap_base;
} __attribute__((packed));

/* 0 null, 1 kcode64, 2 kdata, 3 ucode32, 4 udata, 5 ucode64, 6-7 TSS (16 bytes) */
static struct gdt_entry gdt[8];
static struct tss_entry tss;
static struct gdt_ptr   gp;

extern void gdt_flush(uint64_t);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access      = access;
}

void gdt_install(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint64_t)(uintptr_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    gdt_set_gate(5, 0, 0xFFFFFFFF, 0xFA, 0xAF);

    for (uint32_t i = 0; i < sizeof(tss); i++) {
        ((uint8_t *)&tss)[i] = 0;
    }
    tss.iomap_base = sizeof(tss);
    uint64_t base = (uint64_t)(uintptr_t)&tss;
    gdt_set_gate(6, (uint32_t)base, sizeof(tss) - 1, 0x89, 0x00);
    ((uint32_t *)&gdt[7])[0] = (uint32_t)(base >> 32);
    ((uint32_t *)&gdt[7])[1] = 0;

    gdt_flush((uint64_t)(uintptr_t)&gp);
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)0x30));
}

void tss_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
