#ifndef FELINOS_PIC_H
#define FELINOS_PIC_H

#include <stdint.h>

void pic_remap(void);
void pic_send_eoi(int irq);
void pic_set_mask(int irq);
void pic_clear_mask(int irq);

#endif
