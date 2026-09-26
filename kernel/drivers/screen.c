#include "drivers/screen.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "lib/format.h"
#include "lib/string.h"

static void serial_seq(const char *fmt, ...) {
    char buf[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_write(buf);
}

void screen_enter(void) {
    serial_write("\x1b[2J\x1b[H");
    vga_clear();
}

void screen_leave(void) {
    serial_write("\x1b[0m\x1b[2J\x1b[H");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

void screen_clear(void) {
    screen_enter();
}

void screen_line(size_t row, const char *text, uint8_t color) {
    size_t i = 0;
    for (; text[i] && i < VGA_WIDTH; i++) {
        vga_put_at(i, row, text[i], color);
    }
    for (; i < VGA_WIDTH; i++) {
        vga_put_at(i, row, ' ', color);
    }

    serial_seq("\x1b[%u;1H", (uint32_t)(row + 1));
    if (color >> 4) {
        serial_write("\x1b[7m");
    }
    for (i = 0; text[i] && i < VGA_WIDTH; i++) {
        serial_putchar(text[i]);
    }
    serial_write("\x1b[K\x1b[0m");
}

void screen_cursor(size_t col, size_t row) {
    vga_set_cursor(col, row);
    serial_seq("\x1b[%u;%uH", (uint32_t)(row + 1), (uint32_t)(col + 1));
}
