#include "lib/format.h"
#include "lib/string.h"

struct buf_ctx {
    char *buf;
    size_t size;
    size_t pos;
};

static void emit_buf(void *ctx, char c) {
    struct buf_ctx *b = (struct buf_ctx *)ctx;
    if (b->pos + 1 < b->size) {
        b->buf[b->pos] = c;
    }
    b->pos++;
}

static void emit_str(void (*emit)(void *, char), void *ctx, const char *s) {
    while (*s) {
        emit(ctx, *s++);
    }
}

static void emit_padded(void (*emit)(void *, char), void *ctx, const char *s,
                        int width, int zero, int left) {
    int len = (int)strlen(s);
    int pad = width - len;
    if (pad < 0) {
        pad = 0;
    }
    if (!left) {
        while (pad--) {
            emit(ctx, zero ? '0' : ' ');
        }
        emit_str(emit, ctx, s);
    } else {
        emit_str(emit, ctx, s);
        while (pad--) {
            emit(ctx, ' ');
        }
    }
}

static void u64toa(uint64_t v, char *buf, int base) {
    char rev[24];
    int n = 0;
    do {
        int d = (int)(v % (uint64_t)base);
        rev[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (uint64_t)base;
    } while (v);
    for (int i = 0; i < n; i++) {
        buf[i] = rev[n - 1 - i];
    }
    buf[n] = '\0';
}

static void i64toa(int64_t v, char *buf) {
    if (v < 0) {
        *buf++ = '-';
        u64toa((uint64_t)0 - (uint64_t)v, buf, 10);
    } else {
        u64toa((uint64_t)v, buf, 10);
    }
}

void fmt_vprint(void (*emit)(void *, char), void *ctx, const char *fmt, va_list args) {
    char tmp[36];

    while (*fmt) {
        if (*fmt != '%') {
            emit(ctx, *fmt++);
            continue;
        }
        fmt++;

        int left = 0;
        int zero = 0;
        int width = 0;

        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') {
                left = 1;
            } else {
                zero = 1;
            }
            fmt++;
        }
        while (isdigit(*fmt)) {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int longs = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            if (*fmt == 'l') {
                longs++;
            }
            fmt++;
        }

        switch (*fmt) {
            case 'c':
                tmp[0] = (char)va_arg(args, int);
                tmp[1] = '\0';
                emit_padded(emit, ctx, tmp, width, 0, left);
                break;
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) {
                    s = "(null)";
                }
                emit_padded(emit, ctx, s, width, 0, left);
                break;
            }
            case 'd':
            case 'i':
                if (longs >= 2) {
                    i64toa(va_arg(args, int64_t), tmp);
                } else {
                    itoa(va_arg(args, int32_t), tmp, 10);
                }
                emit_padded(emit, ctx, tmp, width, zero, left);
                break;
            case 'u':
                if (longs >= 2) {
                    u64toa(va_arg(args, uint64_t), tmp, 10);
                } else {
                    utoa(va_arg(args, uint32_t), tmp, 10);
                }
                emit_padded(emit, ctx, tmp, width, zero, left);
                break;
            case 'x':
                if (longs >= 2) {
                    u64toa(va_arg(args, uint64_t), tmp, 16);
                } else {
                    utoa(va_arg(args, uint32_t), tmp, 16);
                }
                emit_padded(emit, ctx, tmp, width, zero, left);
                break;
            case 'X': {
                if (longs >= 2) {
                    u64toa(va_arg(args, uint64_t), tmp, 16);
                } else {
                    utoa(va_arg(args, uint32_t), tmp, 16);
                }
                for (int i = 0; tmp[i]; i++) {
                    tmp[i] = (char)toupper(tmp[i]);
                }
                emit_padded(emit, ctx, tmp, width, zero, left);
                break;
            }
            case 'o':
                utoa(va_arg(args, uint32_t), tmp, 8);
                emit_padded(emit, ctx, tmp, width, zero, left);
                break;
            case 'p':
                emit_str(emit, ctx, "0x");
                utoa((uint32_t)va_arg(args, void *), tmp, 16);
                emit_padded(emit, ctx, tmp, 8, 1, 0);
                break;
            case '%':
                emit(ctx, '%');
                break;
            case '\0':
                return;
            default:
                emit(ctx, '%');
                emit(ctx, *fmt);
                break;
        }
        fmt++;
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    struct buf_ctx ctx = { buf, size, 0 };
    fmt_vprint(emit_buf, &ctx, fmt, args);
    if (size) {
        buf[(ctx.pos < size) ? ctx.pos : size - 1] = '\0';
    }
    return (int)ctx.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}
