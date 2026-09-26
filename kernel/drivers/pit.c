#include "drivers/pit.h"
#include "idt.h"
#include "io.h"
#include "sched.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE     1193182

static volatile uint32_t ticks;

static void pit_callback(struct regs *r) {
    ticks++;
    sched_tick();
}

void pit_init(void) {
    uint32_t divisor = PIT_BASE / PIT_FREQUENCY;

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    ticks = 0;
    irq_install_handler(0, pit_callback);
}

uint32_t pit_ticks(void) {
    return ticks;
}

uint32_t pit_uptime_ms(void) {
    return ticks * (1000 / PIT_FREQUENCY);
}

uint32_t pit_uptime_seconds(void) {
    return ticks / PIT_FREQUENCY;
}

void sleep_ms(uint32_t ms) {
    uint32_t wait = (ms * PIT_FREQUENCY) / 1000;

    if (sched_can_sleep()) {
        sched_sleep_ticks(wait);
        return;
    }
    uint32_t target = ticks + wait;
    while (ticks < target) {
        hlt();
    }
}
