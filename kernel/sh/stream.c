#include "sh/stream.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/format.h"
#include "console.h"
#include "drivers/input.h"

/* Console output buffer: batch writes to reduce VGA framebuffer updates.
 * Flushes on newline or when buffer is full. */
#define CONSOLE_BUF_SIZE 256
static char console_write_buf[CONSOLE_BUF_SIZE];
static uint32_t console_write_pos = 0;

static void console_flush_buf(void) {
    if (console_write_pos == 0) return;
    for (uint32_t i = 0; i < console_write_pos; i++) {
        console_putchar(console_write_buf[i]);
    }
    console_write_pos = 0;
}

static void console_buffered_putc(char c) {
    if (console_write_pos >= CONSOLE_BUF_SIZE - 1) {
        console_flush_buf();
    }
    console_write_buf[console_write_pos++] = c;
    if (c == '\n') console_flush_buf();
}

/* Force flush - call from shell prompt output */
void console_force_flush(void) {
    console_flush_buf();
}

static struct stream console_stream = { 1, NULL, 0, 0, 0 };

struct stream *stream_console(void) {
    return &console_stream;
}

struct stream *stream_new(void) {
    struct stream *s = (struct stream *)kzalloc(sizeof(struct stream));
    if (!s) {
        return NULL;
    }
    s->console = 0;
    s->cap = 256;
    s->buf = (char *)kmalloc(s->cap);
    if (!s->buf) {
        kfree(s);
        return NULL;
    }
    s->buf[0] = '\0';
    return s;
}

void stream_free(struct stream *s) {
    if (!s || s->console) {
        return;
    }
    if (s->buf) {
        kfree(s->buf);
    }
    kfree(s);
}

void stream_reset(struct stream *s) {
    if (!s || s->console) {
        return;
    }
    s->len = 0;
    s->pos = 0;
    if (s->buf) {
        s->buf[0] = '\0';
    }
}

static int stream_grow(struct stream *s, uint32_t needed) {
    if (s->cap > needed) {
        return 0;
    }
    uint32_t cap = s->cap ? s->cap : 256;
    while (cap <= needed) {
        cap *= 2;
    }
    char *buf = (char *)krealloc(s->buf, cap);
    if (!buf) {
        return -1;
    }
    s->buf = buf;
    s->cap = cap;
    return 0;
}

void st_putc(struct stream *s, char c) {
    if (!s) {
        return;
    }
    if (s->console) {
        console_buffered_putc(c);
        return;
    }
    if (stream_grow(s, s->len + 2) < 0) {
        return;
    }
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}

void st_write(struct stream *s, const char *data, uint32_t len) {
    if (!s) {
        return;
    }
    if (s->console) {
        for (uint32_t i = 0; i < len; i++) {
            console_buffered_putc(data[i]);
        }
        return;
    }
    if (stream_grow(s, s->len + len + 1) < 0) {
        return;
    }
    memcpy(s->buf + s->len, data, len);
    s->len += len;
    s->buf[s->len] = '\0';
}

void st_puts(struct stream *s, const char *text) {
    st_write(s, text, (uint32_t)strlen(text));
}

static void emit_stream(void *ctx, char c) {
    st_putc((struct stream *)ctx, c);
}

void st_printf(struct stream *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fmt_vprint(emit_stream, s, fmt, args);
    va_end(args);
}

const char *st_data(struct stream *s) {
    if (!s || s->console || !s->buf) {
        return "";
    }
    return s->buf;
}

uint32_t st_len(struct stream *s) {
    if (!s || s->console) {
        return 0;
    }
    return s->len;
}

static int console_read_line(char *buf, uint32_t size) {
    uint32_t pos = 0;

    for (;;) {
        int key = input_getkey();

        if (key == 4) {
            if (pos == 0) {
                console_putchar('\n');
                return -1;
            }
            buf[pos] = '\0';
            console_putchar('\n');
            return (int)pos;
        }
        if (key == 3) {
            console_write("^C\n");
            return -1;
        }
        if (key == '\n') {
            buf[pos] = '\0';
            console_putchar('\n');
            return (int)pos;
        }
        if (key == '\b') {
            if (pos > 0) {
                pos--;
                console_putchar('\b');
            }
            continue;
        }
        if (key > 0xFF || key < 32) {
            continue;
        }
        if (pos + 1 < size) {
            buf[pos++] = (char)key;
            console_putchar((char)key);
        }
    }
}

int st_getline(struct stream *s, char *buf, uint32_t size) {
    if (!s) {
        return -1;
    }
    if (s->console) {
        return console_read_line(buf, size);
    }
    if (s->pos >= s->len) {
        return -1;
    }

    uint32_t i = 0;
    while (s->pos < s->len && s->buf[s->pos] != '\n') {
        if (i + 1 < size) {
            buf[i++] = s->buf[s->pos];
        }
        s->pos++;
    }
    if (s->pos < s->len && s->buf[s->pos] == '\n') {
        s->pos++;
    }
    buf[i] = '\0';
    return (int)i;
}

char *st_read_all(struct stream *s, uint32_t *len) {
    if (!s) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }

    if (!s->console) {
        uint32_t remaining = (s->pos < s->len) ? s->len - s->pos : 0;
        char *out = (char *)kmalloc(remaining + 1);
        if (!out) {
            return NULL;
        }
        memcpy(out, s->buf + s->pos, remaining);
        out[remaining] = '\0';
        s->pos = s->len;
        if (len) {
            *len = remaining;
        }
        return out;
    }

    uint32_t cap = 512;
    uint32_t used = 0;
    char *out = (char *)kmalloc(cap);
    if (!out) {
        return NULL;
    }
    out[0] = '\0';

    char line[512];
    while (st_getline(s, line, sizeof(line)) >= 0) {
        uint32_t ll = (uint32_t)strlen(line);
        if (used + ll + 2 >= cap) {
            cap = (used + ll + 2) * 2;
            char *grown = (char *)krealloc(out, cap);
            if (!grown) {
                break;
            }
            out = grown;
        }
        memcpy(out + used, line, ll);
        used += ll;
        out[used++] = '\n';
        out[used] = '\0';
    }
    if (len) {
        *len = used;
    }
    return out;
}

int stream_read(struct stream *s, void *buf, uint32_t size) {
    if (!s) return -1;
    if (s->console) {
        /* For console, read one line at a time */
        char line[512];
        int n = st_getline(s, line, sizeof(line));
        if (n < 0) return n;
        if ((uint32_t)n > size) n = size;
        memcpy(buf, line, n);
        if ((uint32_t)n < size) {
            ((char *)buf)[n] = '\n';
            n++;
        }
        return n;
    }
    if (s->pos >= s->len) return 0;
    uint32_t remaining = s->len - s->pos;
    if (remaining > size) remaining = size;
    memcpy(buf, s->buf + s->pos, remaining);
    s->pos += remaining;
    return remaining;
}
