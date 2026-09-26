#ifndef FELINOS_VGA_H
#define FELINOS_VGA_H

#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define VGA_BLACK         0
#define VGA_BLUE          1
#define VGA_GREEN         2
#define VGA_CYAN          3
#define VGA_RED           4
#define VGA_MAGENTA       5
#define VGA_BROWN         6
#define VGA_LIGHT_GREY    7
#define VGA_DARK_GREY     8
#define VGA_LIGHT_BLUE    9
#define VGA_LIGHT_GREEN   10
#define VGA_LIGHT_CYAN    11
#define VGA_LIGHT_RED     12
#define VGA_LIGHT_MAGENTA 13
#define VGA_YELLOW        14
#define VGA_WHITE         15

void vga_initialize(void);
void vga_putchar(char c);
void vga_writestring(const char *data);
void vga_set_color(uint8_t fg, uint8_t bg);
uint8_t vga_get_color(void);
void vga_set_raw_color(uint8_t color);
void vga_clear(void);
void vga_put_at(size_t x, size_t y, char c, uint8_t color);
void vga_write_at(size_t x, size_t y, const char *s, uint8_t color);
void vga_fill_row(size_t y, char c, uint8_t color);
void vga_get_cursor(size_t *x, size_t *y);
void vga_set_cursor(size_t x, size_t y);
void vga_update_hw_cursor(void);
void vga_hide_cursor(void);
uint32_t vga_scroll_count(void);

#endif
