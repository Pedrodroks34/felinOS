#ifndef FELINOS_HEAP_H
#define FELINOS_HEAP_H

#include <stdint.h>
#include <stddef.h>

void heap_init(uint32_t start, uint32_t size);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
void heap_grow(uint32_t extra);
uint32_t heap_total_size(void);
void heap_stats(uint32_t *total, uint32_t *used, uint32_t *largest_free, uint32_t *blocks);

#endif
