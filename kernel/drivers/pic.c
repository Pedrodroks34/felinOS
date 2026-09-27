#include "drivers/pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define OCW1_ALL 0xFF
#define OCW2_NONSPI 0x20   /* non-specific end of interrupt */

void pic_remap(void) {
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_disable(void) {
    /* Taking the 8259s out of the delivery path is not just a matter of
     * masking them. A cascade input whose in-service bit was latched at some
     * point keeps the master's IRQ2 line asserted for as long as nothing
     * acknowledges it, and the I/O APIC dutifully injects that vector over and
     * over -- which starves every lower-numbered vector behind it, including
     * the 8254. Masking alone leaves those latched bits in place, so the
     * end-of-interrupt has to be issued after the masks are set. */
    outb(PIC1_CMD, OCW1_ALL);
    outb(PIC2_CMD, OCW1_ALL);
    io_wait();

    /* Reading the data port settles both controllers' state. */
    (void)inb(PIC1_DATA);
    (void)inb(PIC2_DATA);

    outb(PIC2_CMD, OCW2_NONSPI);
    outb(PIC1_CMD, OCW2_NONSPI);
}

void pic_send_eoi(int irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}

void pic_set_mask(int irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) {
        irq -= 8;
    }
    outb(port, (uint8_t)(inb(port) | (1 << irq)));
}

void pic_clear_mask(int irq) {
    /* IRQ8-15 reach the CPU through the master's cascade input (IRQ2), which
       pic_remap() leaves masked. Without unmasking it, slave IRQs such as the
       secondary/primary ATA channel (15/14) and the RTC are never delivered. */
    if (irq >= 8) {
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1 << 2)));
    }
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) {
        irq -= 8;
    }
    outb(port, (uint8_t)(inb(port) & ~(1 << irq)));
}
