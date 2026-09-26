#ifndef FELINOS_SYSTEM_H
#define FELINOS_SYSTEM_H

#include <stdint.h>
#include "multiboot.h"

#define FELINOS_NAME    "FelinOS"
#define FELINOS_VERSION "0.2"
#define KERNEL_NAME    "Gato"
#define KERNEL_VERSION "0.2"
#define KERNEL_ARCH    "x86_64"

void system_init(struct multiboot_info *mbi);
uint32_t system_mem_lower_kb(void);
uint32_t system_mem_upper_kb(void);
uint32_t system_total_kb(void);
uint32_t system_kernel_start(void);
uint32_t system_kernel_end(void);
uint32_t system_heap_start(void);
uint32_t system_heap_size(void);
uint32_t system_boot_unix(void);
struct multiboot_info *system_multiboot(void);

#endif
