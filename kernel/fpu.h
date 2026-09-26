#ifndef FELINOS_FPU_H
#define FELINOS_FPU_H

#include <stdint.h>

#define FPU_STATE_SIZE 512

/* x87/MMX/SSE state (fxsave64 image). Saved and restored on every context
   switch, so ring 3 code may use floating point and SSE freely. The kernel is
   built with -mgeneral-regs-only and never touches this state itself. */
void fpu_init(void);
int fpu_available(void);
void fpu_save(void *area);
void fpu_restore(const void *area);
void fpu_default(void *area);

#endif
