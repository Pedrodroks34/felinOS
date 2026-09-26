#ifndef FELINOS_PIT_H
#define FELINOS_PIT_H

#include <stdint.h>

#define PIT_FREQUENCY 100

void pit_init(void);
uint32_t pit_ticks(void);
uint32_t pit_uptime_ms(void);
uint32_t pit_uptime_seconds(void);
void sleep_ms(uint32_t ms);

#endif
