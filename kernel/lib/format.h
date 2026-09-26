#ifndef FELINOS_FORMAT_H
#define FELINOS_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

void fmt_vprint(void (*emit)(void *, char), void *ctx, const char *fmt, va_list args);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);

#endif
