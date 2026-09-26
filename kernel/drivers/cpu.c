#include "drivers/cpu.h"
#include "lib/string.h"

struct feature {
    const char *name;
    int reg;
    int bit;
};

static const struct feature features[] = {
    { "fpu", 3, 0 },
    { "vme", 3, 1 },
    { "de", 3, 2 },
    { "pse", 3, 3 },
    { "tsc", 3, 4 },
    { "msr", 3, 5 },
    { "pae", 3, 6 },
    { "apic", 3, 9 },
    { "sep", 3, 11 },
    { "mtrr", 3, 12 },
    { "pge", 3, 13 },
    { "cmov", 3, 15 },
    { "pat", 3, 16 },
    { "clflush", 3, 19 },
    { "mmx", 3, 23 },
    { "fxsr", 3, 24 },
    { "sse", 3, 25 },
    { "sse2", 3, 26 },
    { "htt", 3, 28 },
    { "sse3", 2, 0 },
    { "pclmul", 2, 1 },
    { "ssse3", 2, 9 },
    { "fma", 2, 12 },
    { "cx16", 2, 13 },
    { "sse4_1", 2, 19 },
    { "sse4_2", 2, 20 },
    { "x2apic", 2, 21 },
    { "popcnt", 2, 23 },
    { "aes", 2, 25 },
    { "xsave", 2, 26 },
    { "avx", 2, 28 },
    { "rdrand", 2, 30 },
    { "hypervisor", 2, 31 },
    { NULL, 0, 0 }
};

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(0));
}

void cpu_vendor(char *buf) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    memcpy(buf, &ebx, 4);
    memcpy(buf + 4, &edx, 4);
    memcpy(buf + 8, &ecx, 4);
    buf[12] = '\0';
}

int cpu_brand(char *buf) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000000u, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000004u) {
        buf[0] = '\0';
        return 0;
    }
    for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        cpuid(leaf, &eax, &ebx, &ecx, &edx);
        uint32_t offset = (leaf - 0x80000002u) * 16;
        memcpy(buf + offset, &eax, 4);
        memcpy(buf + offset + 4, &ebx, 4);
        memcpy(buf + offset + 8, &ecx, 4);
        memcpy(buf + offset + 12, &edx, 4);
    }
    buf[48] = '\0';
    return 1;
}

static uint32_t signature(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return eax;
}

uint32_t cpu_family(void) {
    uint32_t sig = signature();
    uint32_t family = (sig >> 8) & 0xF;
    if (family == 0xF) {
        family += (sig >> 20) & 0xFF;
    }
    return family;
}

uint32_t cpu_model(void) {
    uint32_t sig = signature();
    uint32_t family = (sig >> 8) & 0xF;
    uint32_t model = (sig >> 4) & 0xF;
    if (family == 0x6 || family == 0xF) {
        model |= ((sig >> 16) & 0xF) << 4;
    }
    return model;
}

uint32_t cpu_stepping(void) {
    return signature() & 0xF;
}

int cpu_feature_present(int index) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t regs[4];

    if (features[index].name == NULL) {
        return 0;
    }
    cpuid(1, &eax, &ebx, &ecx, &edx);
    regs[0] = eax;
    regs[1] = ebx;
    regs[2] = ecx;
    regs[3] = edx;
    return (regs[features[index].reg] >> features[index].bit) & 1;
}

const char *cpu_feature_list(int index) {
    return features[index].name;
}

int cpu_has_feature(const char *name) {
    for (int i = 0; features[i].name; i++) {
        if (strcmp(features[i].name, name) == 0) {
            return cpu_feature_present(i);
        }
    }
    return 0;
}
