#ifndef FELINOS_GDT_H
#define FELINOS_GDT_H

#include <stdint.h>

#define SEL_KCODE   0x08
#define SEL_KDATA   0x10
#define SEL_UCODE   0x1B   /* ring 3, 32-bit compatibility mode */
#define SEL_UDATA   0x23
#define SEL_UCODE64 0x2B   /* ring 3, 64-bit long mode */

void gdt_install(void);
void gdt_reload(void);
void tss_set_kernel_stack(uint64_t rsp0);

#endif
