#ifndef FELINOS_FONT_H
#define FELINOS_FONT_H

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

struct font_glyph {
    uint8_t data[FONT_HEIGHT];
};

struct font {
    struct font_glyph glyphs[256];
    int loaded;
};

void font_init(void);
void font_render_char(int x, int y, char c, uint32_t fg_color, uint32_t bg_color);
void font_render_string(int x, int y, const char *str, uint32_t fg_color, uint32_t bg_color);
int font_text_width(const char *str);
int font_text_height(void);

#endif