#include "fpu.h"
#include "lib/string.h"

static uint8_t fpu_template[FPU_STATE_SIZE] __attribute__((aligned(16)));
static int fpu_ready;

void fpu_init(void) {
    uint32_t eax, ebx, ecx, edx;
    uint64_t cr0, cr4;

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    if (!(edx & (1u << 24))) {          /* no FXSR: cannot save state, leave off */
        fpu_ready = 0;
        return;
    }
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ull << 2) | (1ull << 3));    /* EM = 0 (no emulation), TS = 0 */
    cr0 |= (1ull << 1) | (1ull << 5);       /* MP, NE */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10);      /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");

    uint32_t mxcsr = 0x1F80u;               /* all SIMD exceptions masked */
    __asm__ volatile ("fninit");
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
    memset(fpu_template, 0, sizeof(fpu_template));
    __asm__ volatile ("fxsave64 %0" : "=m"(*(uint8_t (*)[FPU_STATE_SIZE])fpu_template) : : "memory");
    fpu_ready = 1;
}

int fpu_available(void) {
    return fpu_ready;
}

void fpu_save(void *area) {
    if (fpu_ready) {
        __asm__ volatile ("fxsave64 %0" : "=m"(*(uint8_t (*)[FPU_STATE_SIZE])area) : : "memory");
    }
}

void fpu_restore(const void *area) {
    if (fpu_ready) {
        __asm__ volatile ("fxrstor64 %0" : : "m"(*(const uint8_t (*)[FPU_STATE_SIZE])area) : "memory");
    }
}

void fpu_default(void *area) {
    memcpy(area, fpu_template, FPU_STATE_SIZE);
}
