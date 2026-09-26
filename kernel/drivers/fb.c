#include "drivers/fb.h"
#include "io.h"

#define FB_MEMORY ((uint8_t *)0xA0000)

#define FB_PORT_MISC_WRITE 0x3C2
#define FB_PORT_SEQ_INDEX  0x3C4
#define FB_PORT_SEQ_DATA   0x3C5
#define FB_PORT_CRTC_INDEX 0x3D4
#define FB_PORT_CRTC_DATA  0x3D5
#define FB_PORT_GC_INDEX   0x3CE
#define FB_PORT_GC_DATA    0x3CF
#define FB_PORT_AC_INDEX   0x3C0
#define FB_PORT_AC_WRITE   0x3C0
#define FB_PORT_INSTAT     0x3DA
#define FB_PORT_PEL_INDEX  0x3C8
#define FB_PORT_PEL_DATA   0x3C9

#define FB_REG_COUNT   61
#define FB_SEQ_COUNT   5
#define FB_CRTC_COUNT  25
#define FB_GC_COUNT    9
#define FB_AC_COUNT    21
#define FB_CRTC_OFFSET (1 + FB_SEQ_COUNT)
#define FB_GC_OFFSET   (FB_CRTC_OFFSET + FB_CRTC_COUNT)
#define FB_AC_OFFSET   (FB_GC_OFFSET + FB_GC_COUNT)

static uint8_t *const fb_memory = FB_MEMORY;
static int fb_state_active;

static const uint8_t fb_regs_text_80x25[FB_REG_COUNT] = {
    0x67,
    0x03, 0x00, 0x03, 0x00, 0x02,
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0D, 0x0E,
    0x00, 0x00, 0x00, 0x50, 0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B,
    0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00
};

static const uint8_t fb_regs_gfx_320x200x256[FB_REG_COUNT] = {
    0x63,
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x41, 0x00, 0x0F, 0x00, 0x00
};

static void fb_load_registers(const uint8_t *regs) {
    uint8_t work[FB_REG_COUNT];
    for (int i = 0; i < FB_REG_COUNT; i++) {
        work[i] = regs[i];
    }

    outb(FB_PORT_MISC_WRITE, work[0]);

    for (int i = 0; i < FB_SEQ_COUNT; i++) {
        outb(FB_PORT_SEQ_INDEX, (uint8_t)i);
        outb(FB_PORT_SEQ_DATA, work[1 + i]);
    }

    outb(FB_PORT_CRTC_INDEX, 0x03);
    outb(FB_PORT_CRTC_DATA, (uint8_t)(inb(FB_PORT_CRTC_DATA) | 0x80));
    outb(FB_PORT_CRTC_INDEX, 0x11);
    outb(FB_PORT_CRTC_DATA, (uint8_t)(inb(FB_PORT_CRTC_DATA) & ~0x80));

    work[FB_CRTC_OFFSET + 0x03] |= 0x80;
    work[FB_CRTC_OFFSET + 0x11] &= (uint8_t)~0x80;

    for (int i = 0; i < FB_CRTC_COUNT; i++) {
        outb(FB_PORT_CRTC_INDEX, (uint8_t)i);
        outb(FB_PORT_CRTC_DATA, work[FB_CRTC_OFFSET + i]);
    }

    for (int i = 0; i < FB_GC_COUNT; i++) {
        outb(FB_PORT_GC_INDEX, (uint8_t)i);
        outb(FB_PORT_GC_DATA, work[FB_GC_OFFSET + i]);
    }

    for (int i = 0; i < FB_AC_COUNT; i++) {
        inb(FB_PORT_INSTAT);
        outb(FB_PORT_AC_INDEX, (uint8_t)i);
        outb(FB_PORT_AC_WRITE, work[FB_AC_OFFSET + i]);
    }

    inb(FB_PORT_INSTAT);
    outb(FB_PORT_AC_INDEX, 0x20);
}

void fb_init(void) {
    fb_state_active = 0;
}

int fb_active(void) {
    return fb_state_active;
}

void fb_enable(void) {
    fb_load_registers(fb_regs_gfx_320x200x256);
    fb_state_active = 1;
}

void fb_disable(void) {
    fb_load_registers(fb_regs_text_80x25);
    fb_state_active = 0;
}

void fb_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    outb(FB_PORT_PEL_INDEX, index);
    outb(FB_PORT_PEL_DATA, r & 0x3F);
    outb(FB_PORT_PEL_DATA, g & 0x3F);
    outb(FB_PORT_PEL_DATA, b & 0x3F);
}

void fb_put_pixel(size_t x, size_t y, uint8_t color) {
    if (x >= FB_WIDTH || y >= FB_HEIGHT) {
        return;
    }
    fb_memory[y * FB_WIDTH + x] = color;
}

uint8_t fb_get_pixel(size_t x, size_t y) {
    if (x >= FB_WIDTH || y >= FB_HEIGHT) {
        return 0;
    }
    return fb_memory[y * FB_WIDTH + x];
}

void fb_fill_rect(size_t x, size_t y, size_t w, size_t h, uint8_t color) {
    size_t y_end = y + h;
    size_t x_end = x + w;
    if (y_end > FB_HEIGHT) {
        y_end = FB_HEIGHT;
    }
    if (x_end > FB_WIDTH) {
        x_end = FB_WIDTH;
    }
    for (size_t row = y; row < y_end; row++) {
        for (size_t col = x; col < x_end; col++) {
            fb_memory[row * FB_WIDTH + col] = color;
        }
    }
}

void fb_clear(uint8_t color) {
    fb_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, color);
}

uint32_t fb_width(void) {
    return FB_WIDTH;
}

uint32_t fb_height(void) {
    return FB_HEIGHT;
}

uint8_t *fb_buffer(void) {
    return fb_memory;
}
