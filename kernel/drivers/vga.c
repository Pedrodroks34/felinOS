#include "drivers/vga.h"
#include "io.h"
#include "sync.h"

#define VGA_MEMORY ((uint16_t *)0xB8000)
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

static size_t term_row;
static size_t term_col;
static uint8_t term_color = 0x07;
static uint32_t scroll_count;
static uint16_t *const term_buffer = VGA_MEMORY;
static spinlock_t crtc_lock = SPINLOCK_INIT("vga-crtc");

static uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t)c | (uint16_t)color << 8;
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    term_color = (uint8_t)(fg | bg << 4);
}

void vga_set_raw_color(uint8_t color) {
    term_color = color;
}

uint8_t vga_get_color(void) {
    return term_color;
}

void vga_update_hw_cursor(void) {
    uint16_t pos = (uint16_t)(term_row * VGA_WIDTH + term_col);
    uint32_t f = spin_lock_irqsave(&crtc_lock);
    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
    spin_unlock_irqrestore(&crtc_lock, f);
}

void vga_hide_cursor(void) {
    uint32_t f = spin_lock_irqsave(&crtc_lock);
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 0x20);
    spin_unlock_irqrestore(&crtc_lock, f);
}

void vga_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            term_buffer[y * VGA_WIDTH + x] = vga_entry(' ', term_color);
        }
    }
    term_row = 0;
    term_col = 0;
    vga_update_hw_cursor();
}

void vga_initialize(void) {
    term_row = 0;
    term_col = 0;
    scroll_count = 0;
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 14);
    outb(VGA_CRTC_INDEX, 0x0B);
    outb(VGA_CRTC_DATA, 15);
}

static void vga_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            term_buffer[(y - 1) * VGA_WIDTH + x] = term_buffer[y * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        term_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', term_color);
    }
    term_row = VGA_HEIGHT - 1;
    scroll_count++;
}

static void vga_newline(void) {
    term_col = 0;
    if (++term_row == VGA_HEIGHT) {
        vga_scroll();
    }
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_newline();
        vga_update_hw_cursor();
        return;
    }
    if (c == '\r') {
        term_col = 0;
        vga_update_hw_cursor();
        return;
    }
    if (c == '\t') {
        size_t next = (term_col + 8) & ~(size_t)7;
        while (term_col < next && term_col < VGA_WIDTH) {
            term_buffer[term_row * VGA_WIDTH + term_col] = vga_entry(' ', term_color);
            term_col++;
        }
        if (term_col >= VGA_WIDTH) {
            vga_newline();
        }
        vga_update_hw_cursor();
        return;
    }
    if (c == '\b') {
        if (term_col > 0) {
            term_col--;
        } else if (term_row > 0) {
            term_row--;
            term_col = VGA_WIDTH - 1;
        }
        term_buffer[term_row * VGA_WIDTH + term_col] = vga_entry(' ', term_color);
        vga_update_hw_cursor();
        return;
    }

    term_buffer[term_row * VGA_WIDTH + term_col] = vga_entry((unsigned char)c, term_color);
    if (++term_col == VGA_WIDTH) {
        vga_newline();
    }
    vga_update_hw_cursor();
}

void vga_writestring(const char *data) {
    for (size_t i = 0; data[i] != '\0'; i++) {
        vga_putchar(data[i]);
    }
}

void vga_put_at(size_t x, size_t y, char c, uint8_t color) {
    if (x >= VGA_WIDTH || y >= VGA_HEIGHT) {
        return;
    }
    term_buffer[y * VGA_WIDTH + x] = vga_entry((unsigned char)c, color);
}

void vga_write_at(size_t x, size_t y, const char *s, uint8_t color) {
    for (size_t i = 0; s[i] && x + i < VGA_WIDTH; i++) {
        vga_put_at(x + i, y, s[i], color);
    }
}

void vga_fill_row(size_t y, char c, uint8_t color) {
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        vga_put_at(x, y, c, color);
    }
}

void vga_get_cursor(size_t *x, size_t *y) {
    if (x) {
        *x = term_col;
    }
    if (y) {
        *y = term_row;
    }
}

void vga_set_cursor(size_t x, size_t y) {
    if (x >= VGA_WIDTH) {
        x = VGA_WIDTH - 1;
    }
    if (y >= VGA_HEIGHT) {
        y = VGA_HEIGHT - 1;
    }
    term_col = x;
    term_row = y;
    vga_update_hw_cursor();
}

uint32_t vga_scroll_count(void) {
    return scroll_count;
}
