#ifndef FELINOS_STREAM_H
#define FELINOS_STREAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

struct stream {
    int console;
    char *buf;
    uint32_t len;
    uint32_t cap;
    uint32_t pos;
};

struct stream *stream_new(void);
struct stream *stream_console(void);
void stream_free(struct stream *s);
void stream_reset(struct stream *s);

void st_putc(struct stream *s, char c);
void st_write(struct stream *s, const char *data, uint32_t len);
void st_puts(struct stream *s, const char *text);
void st_printf(struct stream *s, const char *fmt, ...);

const char *st_data(struct stream *s);
uint32_t st_len(struct stream *s);
int st_getline(struct stream *s, char *buf, uint32_t size);
char *st_read_all(struct stream *s, uint32_t *len);
int stream_read(struct stream *s, void *buf, uint32_t size);
void console_force_flush(void);

#endif
