#ifndef FELINOS_PIT_H
#define FELINOS_PIT_H

#include <stdint.h>

#define PIT_FREQUENCY 100

/* A tick has to come from somewhere, but it does not have to come from the
 * 8254. On an APIC machine the Local APIC timer is the natural clockevent and
 * the 8254 is only a fallback, which is how Linux arranges it too: the PIT is
 * tied to IRQ 0, cannot be routed anywhere useful, and on a machine where the
 * I/O APIC owns delivery it is the APIC timer that is actually reachable.
 *
 * Exactly one source owns the tick counter at a time. If both were allowed to
 * bump it, every machine that wires up the APIC timer would run its clock at
 * double speed. */
enum clockevent {
    CLOCKEVENT_8254 = 0,
    CLOCKEVENT_LAPIC_TIMER = 1,
};

void clockevent_select(enum clockevent src);
enum clockevent clockevent_current(void);

/* One tick from whichever source currently owns the counter. */
void clockevent_tick(void);

/* Milliseconds each tick of the current source is worth. The two sources do
 * not agree: the 8254 is programmed for exactly 100 Hz, while the Local APIC
 * timer divides its input by the DCR divisor and runs off the bus clock, so
 * its period has to be derived from the count that was programmed. */
uint32_t clockevent_ms_per_tick(void);

void pit_init(void);
uint32_t pit_ticks(void);
uint32_t pit_uptime_ms(void);
uint32_t pit_uptime_seconds(void);
void sleep_ms(uint32_t ms);

#endif
