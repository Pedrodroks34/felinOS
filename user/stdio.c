#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "stdio.h"
#include "usys.h"

typedef void (*emit_fn)(void *ctx, char c);

static void emit_console(void *ctx, char c) {
    (void)ctx;
    write(1, &c, 1);
}

static void emit_buf(void *ctx, char c) {
    char **p = (char **)ctx;
    **p = c;
    (*p)++;
}

static void print_num(emit_fn emit, void *ctx, unsigned long v, int base, int neg) {
    char buf[24];
    int i = 0;

    if (v == 0) {
        buf[i++] = '0';
    }
    while (v) {
        int d = (int)(v % (unsigned long)base);
        buf[i++] = d < 10 ? (char)('0' + d) : (char)('a' + d - 10);
        v /= (unsigned long)base;
    }
    if (neg) {
        emit(ctx, '-');
    }
    while (i > 0) {
        emit(ctx, buf[--i]);
    }
}

static int vformat(emit_fn emit, void *ctx, const char *fmt, va_list ap) {
    int n = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(ctx, *fmt);
            n++;
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            unsigned long u = v < 0 ? (unsigned long)(-(long)v) : (unsigned long)v;
            print_num(emit, ctx, u, 10, v < 0);
            n++;
            break;
        }
        case 'u':
            print_num(emit, ctx, (unsigned long)va_arg(ap, unsigned int), 10, 0);
            n++;
            break;
        case 'x':
            print_num(emit, ctx, (unsigned long)va_arg(ap, unsigned int), 16, 0);
            n++;
            break;
        case 'p':
            emit(ctx, '0');
            emit(ctx, 'x');
            print_num(emit, ctx, (unsigned long)(uintptr_t)va_arg(ap, void *), 16, 0);
            n++;
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            while (*s) {
                emit(ctx, *s++);
                n++;
            }
            break;
        }
        case 'c':
            emit(ctx, (char)va_arg(ap, int));
            n++;
            break;
        case '%':
            emit(ctx, '%');
            n++;
            break;
        default:
            emit(ctx, '%');
            emit(ctx, *fmt);
            n += 2;
            break;
        }
    }
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vformat(emit_console, NULL, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *out, const char *fmt, ...) {
    va_list ap;
    char *cursor = out;

    va_start(ap, fmt);
    int n = vformat(emit_buf, &cursor, fmt, ap);
    va_end(ap);
    *cursor = 0;
    return n;
}
