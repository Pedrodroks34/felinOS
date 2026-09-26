#ifndef FELINOS_SCREEN_H
#define FELINOS_SCREEN_H

#include <stdint.h>
#include <stddef.h>

void screen_enter(void);
void screen_leave(void);
void screen_clear(void);
void screen_line(size_t row, const char *text, uint8_t color);
void screen_cursor(size_t col, size_t row);

#endif
