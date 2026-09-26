#ifndef FELINOS_KEYBOARD_H
#define FELINOS_KEYBOARD_H

#include <stdint.h>

#define KEY_UP     0x101
#define KEY_DOWN   0x102
#define KEY_LEFT   0x103
#define KEY_RIGHT  0x104
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_PGUP   0x107
#define KEY_PGDN   0x108
#define KEY_DELETE 0x109
#define KEY_INSERT 0x10A
#define KEY_F1     0x110
#define KEY_F2     0x111
#define KEY_F3     0x112
#define KEY_F4     0x113
#define KEY_F5     0x114
#define KEY_F6     0x115
#define KEY_F7     0x116
#define KEY_F8     0x117
#define KEY_F9     0x118
#define KEY_F10    0x119

void keyboard_init(void);
int keyboard_poll(void);
int keyboard_pending(void);
void keyboard_set_break_hook(int (*hook)(void));
int keyboard_shift_down(void);
int keyboard_ctrl_down(void);
int keyboard_alt_down(void);
int keyboard_caps_on(void);

#endif
