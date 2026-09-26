#ifndef FELINOS_FB_H
#define FELINOS_FB_H

#include <stdint.h>
#include <stddef.h>

#define FB_WIDTH  320
#define FB_HEIGHT 200

void fb_init(void);
int fb_active(void);
void fb_enable(void);
void fb_disable(void);
void fb_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void fb_put_pixel(size_t x, size_t y, uint8_t color);
uint8_t fb_get_pixel(size_t x, size_t y);
void fb_fill_rect(size_t x, size_t y, size_t w, size_t h, uint8_t color);
void fb_clear(uint8_t color);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint8_t *fb_buffer(void);

#endif
