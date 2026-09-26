#ifndef FELINOS_INPUT_H
#define FELINOS_INPUT_H

#include "drivers/keyboard.h"

int input_getkey(void);
void input_init(void);
int input_poll(void);
void input_flush(void);

#endif
