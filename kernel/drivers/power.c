#include "drivers/power.h"
#include "drivers/acpi.h"
#include "console.h"
#include "io.h"

static void delay(uint32_t n) {
    while (n--) {
        io_wait();
    }
}

void power_reboot(void) {
    cli();

    acpi_reset();

    uint8_t status = 0x02;
    uint32_t guard = 100000;
    while ((status & 0x02) && guard--) {
        status = inb(0x64);
    }
    outb(0x64, 0xFE);
    delay(50000);

    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    delay(50000);

    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = { 0, 0 };

    __asm__ volatile ("lidt %0" : : "m"(null_idt));
    __asm__ volatile ("int $0x03");

    for (;;) {
        hlt();
    }
}

void power_off(void) {
    cli();

    if (acpi_can_power_off()) {
        acpi_power_off();
        kprintf("ACPI power off had no effect.\n");
        power_halt();
    }

    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    outw(0x600, 0x34);
    power_halt();
}

void power_halt(void) {
    cli();
    for (;;) {
        hlt();
    }
}
