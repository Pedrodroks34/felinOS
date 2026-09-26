#ifndef FELINOS_CPU_H
#define FELINOS_CPU_H

#include <stdint.h>

void cpu_vendor(char *buf);
int cpu_brand(char *buf);
uint32_t cpu_family(void);
uint32_t cpu_model(void);
uint32_t cpu_stepping(void);
int cpu_has_feature(const char *name);
const char *cpu_feature_list(int index);
int cpu_feature_present(int index);

#endif
