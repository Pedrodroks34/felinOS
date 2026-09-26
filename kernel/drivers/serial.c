#include "drivers/serial.h"
#include "idt.h"
#include "io.h"
#include "sched.h"

#define COM1 0x3F8
#define RX_RING 256

static int serial_ready;
static uint8_t rx_ring[RX_RING];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;
static int (*break_hook)(void);

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    serial_ready = 1;
}

int serial_available(void) {
    return serial_ready;
}

static int is_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putchar(char c) {
    if (!serial_ready) {
        return;
    }
    uint32_t guard = 100000;
    while (!is_transmit_empty() && guard--) {
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    while (*s) {
        serial_putchar(*s++);
    }
}

int serial_received(void) {
    if (!serial_ready) {
        return 0;
    }
    return inb(COM1 + 5) & 1;
}

static void ring_push(uint8_t c) {
    uint32_t next = (rx_head + 1) % RX_RING;

    if (c == 3 && break_hook && break_hook()) {
        return;
    }
    if (next != rx_tail) {
        rx_ring[rx_head] = c;
        rx_head = next;
    }
}

static void serial_irq(struct regs *r) {
    while (serial_received()) {
        ring_push(inb(COM1));
    }
    sched_wake_input();
}

void serial_irq_init(void) {
    if (!serial_ready) {
        return;
    }
    rx_head = 0;
    rx_tail = 0;
    irq_install_handler(4, serial_irq);
    outb(COM1 + 1, 0x01);
}

void serial_set_break_hook(int (*hook)(void)) {
    break_hook = hook;
}

int serial_pending(void) {
    return rx_head != rx_tail || serial_received();
}

int serial_read_nonblock(void) {
    uint32_t f = irq_save();
    int c = -1;

    if (rx_head != rx_tail) {
        c = rx_ring[rx_tail];
        rx_tail = (rx_tail + 1) % RX_RING;
    } else if (serial_received()) {
        c = inb(COM1);
    }
    irq_restore(f);
    return c;
}
