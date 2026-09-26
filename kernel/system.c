#include "system.h"
#include "vmm.h"
#include "drivers/rtc.h"

extern uint32_t kernel_start;
extern uint32_t kernel_end;

static struct multiboot_info *boot_info;
static uint32_t mem_lower_kb;
static uint32_t mem_upper_kb;
static uint32_t boot_unix;

void system_init(struct multiboot_info *mbi) {
    boot_info = mbi;
    mem_lower_kb = 640;
    mem_upper_kb = 31744;

    if (mbi && (mbi->flags & 1)) {
        mem_lower_kb = mbi->mem_lower;
        mem_upper_kb = mbi->mem_upper;
    }

    boot_unix = rtc_unix();
}

uint32_t system_mem_lower_kb(void) {
    return mem_lower_kb;
}

uint32_t system_mem_upper_kb(void) {
    return mem_upper_kb;
}

uint32_t system_total_kb(void) {
    return mem_lower_kb + mem_upper_kb;
}

uint32_t system_kernel_start(void) {
    return (uint32_t)&kernel_start;
}

uint32_t system_kernel_end(void) {
    return ((uint32_t)&kernel_end + 0xFFFu) & ~0xFFFu;
}

uint32_t system_heap_start(void) {
    return vmm_heap_base();
}

uint32_t system_heap_size(void) {
    return vmm_heap_size();
}

uint32_t system_boot_unix(void) {
    return boot_unix;
}

struct multiboot_info *system_multiboot(void) {
    return boot_info;
}
