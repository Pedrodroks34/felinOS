#ifndef FELINOS_SERIAL_H
#define FELINOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *s);
int serial_received(void);
int serial_read_nonblock(void);
int serial_available(void);
int serial_pending(void);
void serial_irq_init(void);
void serial_set_break_hook(int (*hook)(void));

#endif
