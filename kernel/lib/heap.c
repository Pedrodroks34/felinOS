#include "lib/heap.h"
#include "lib/string.h"
#include "sync.h"

/* Legacy heap functions kept for compatibility. The actual kmalloc/kfree/
 * krealloc/kzalloc implementation moved to kernel/mm/slub.c.
 * This file now only provides heap initialization and growth bookkeeping
 * for the legacy heap region, plus stats. */

static mutex_t heap_mtx = MUTEX_INIT("heap");
static uint32_t heap_total;

void heap_init(uint32_t start, uint32_t size) {
    heap_total = size;
}

void heap_grow(uint32_t extra) {
    mutex_lock(&heap_mtx);
    heap_total += extra;
    mutex_unlock(&heap_mtx);
}

uint32_t heap_total_size(void) {
    return heap_total;
}

void heap_stats(uint32_t *total, uint32_t *used, uint32_t *largest_free, uint32_t *blocks) {
    mutex_lock(&heap_mtx);
    if (total) {
        *total = heap_total;
    }
    if (used) {
        *used = 0;
    }
    if (largest_free) {
        *largest_free = 0;
    }
    if (blocks) {
        *blocks = 0;
    }
    mutex_unlock(&heap_mtx);
}
