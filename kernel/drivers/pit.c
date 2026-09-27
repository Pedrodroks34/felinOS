#include "drivers/pit.h"
#include "idt.h"
#include "io.h"
#include "sched.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE     1193182

/* The Local APIC timer's input is the bus clock divided by the DCR divisor, and
 * its period is the count programmed into the ICR -- so one tick is
 * (count * divisor) bus cycles, i.e. 1000000 * 16 / 1e9 s = 16 ms exactly. The
 * bus clock is stated rather than measured because nothing on the PC reads it
 * back; 1 GHz is what QEMU's TCG and the common parts report. Keep these in
 * step with the values apic_setup_timer() programs. */
#define LAPIC_BUS_HZ       1000000000ull
#define LAPIC_TIMER_DIV    16
#define LAPIC_TIMER_COUNT  1000000u
#define LAPIC_TIMER_MS     (((uint64_t)LAPIC_TIMER_COUNT * LAPIC_TIMER_DIV * 1000ull) / LAPIC_BUS_HZ)

static volatile uint32_t ticks;
static volatile enum clockevent clockevent = CLOCKEVENT_8254;

void clockevent_select(enum clockevent src) {
    clockevent = src;
}

enum clockevent clockevent_current(void) {
    return clockevent;
}

void clockevent_tick(void) {
    ticks++;
    sched_tick();
}

uint32_t clockevent_ms_per_tick(void) {
    return clockevent == CLOCKEVENT_LAPIC_TIMER ? (uint32_t)LAPIC_TIMER_MS
                                                : (1000 / PIT_FREQUENCY);
}

static void pit_callback(struct regs *r) {
    /* Still preempt on the 8254 so a wakeup is not held off, but only the
     * selected source is allowed to move the clock. */
    if (clockevent == CLOCKEVENT_8254) {
        clockevent_tick();
    } else {
        sched_tick();
    }
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
    return ticks * clockevent_ms_per_tick();
}

uint32_t pit_uptime_seconds(void) {
    return pit_uptime_ms() / 1000;
}

void sleep_ms(uint32_t ms) {
    uint32_t per = clockevent_ms_per_tick();
    uint32_t wait = (ms + per - 1) / per;

    if (sched_can_sleep()) {
        sched_sleep_ticks(wait);
        return;
    }
    uint32_t target = ticks + wait;
    while (ticks < target) {
        hlt();
    }
}
