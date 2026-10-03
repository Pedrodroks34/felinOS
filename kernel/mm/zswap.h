#ifndef FELINOS_ZSWAP_H
#define FELINOS_ZSWAP_H

#include <stdint.h>
#include <stddef.h>
#include "mm/pagecache.h"

/* zswap - compressed swap in RAM
 *
 * Compresses pages before writing to swap, stores in a compressed pool.
 * Uses LZ4-like compression (simplified) with zbud/z3fold-style allocation.
 */

#define ZSWAP_POOL_SIZE_PAGES 4096  /* 16 MB compressed pool */
#define ZSWAP_MAX_COMPRESSION_RATIO 4  /* Max 4:1 compression */

/* zswap pool stats */
struct zswap_stats {
    uint64_t pages_stored;
    uint64_t pages_rejected;
    uint64_t pages_reclaimed;
    uint64_t pool_used_bytes;
    uint64_t pool_total_bytes;
    uint64_t compression_ratio;  /* Fixed point: ratio * 100 */
};

void zswap_init(void);
int zswap_store(struct page *page);
int zswap_load(struct page *page);
void zswap_invalidate(struct page *page);
void zswap_get_stats(struct zswap_stats *out);

/* zbud-style allocation for compressed pages */
void *zbud_alloc(size_t size);
void zbud_free(void *ptr, size_t size);

#endif