#ifndef FELINOS_CONSOLE_H
#define FELINOS_CONSOLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

void console_init(void);
void console_putchar(char c);
void console_write(const char *s);
void console_write_n(const char *s, uint32_t len);
void console_set_panic(void);
void console_clear(void);
uint32_t console_epoch(void);
void kprintf(const char *fmt, ...);
void klog(const char *fmt, ...);
const char *klog_buffer(void);
void klog_clear(void);

#endif
