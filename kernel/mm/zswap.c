#include "mm/zswap.h"
#include "mm/pagecache.h"
#include "pmm.h"
#include "vmm.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "sync.h"

#define ZSWAP_POOL_PAGES 4096
#define ZSWAP_POOL_SIZE (ZSWAP_POOL_PAGES * PAGECACHE_PAGE_SIZE)

struct zswap_entry {
    uint32_t *data;
    uint32_t compressed_size;
    uint32_t original_size;
    uint32_t checksum;
    uint8_t *owner_page;
    int in_use;
};

static struct zswap_entry zswap_pool[ZSWAP_POOL_PAGES];
static uint8_t *zswap_pool_memory = NULL;
static uint32_t zswap_pool_used = 0;
static uint32_t zswap_pool_next = 0;
static int zswap_inited = 0;
static mutex_t zswap_mtx = MUTEX_INIT("zswap");

/* Simplified LZ4-like compression */
static uint32_t lz4_compress(const uint8_t *src, uint8_t *dst, uint32_t src_size) {
    /* Simplified: RLE + literal copy */
    uint32_t dst_pos = 0;
    uint32_t src_pos = 0;

    while (src_pos < src_size) {
        /* Check for run of 4+ identical bytes */
        if (src_pos + 3 < src_size &&
            src[src_pos] == src[src_pos+1] &&
            src[src_pos] == src[src_pos+2] &&
            src[src_pos] == src[src_pos+3]) {
            /* RLE run */
            uint32_t run_len = 4;
            while (src_pos + run_len < src_size &&
                   src[src_pos + run_len] == src[src_pos] &&
                   run_len < 255) {
                run_len++;
            }
            dst[dst_pos++] = 0xFF;  /* RLE marker */
            dst[dst_pos++] = src[src_pos];
            dst[dst_pos++] = run_len;
            src_pos += run_len;
        } else {
            /* Literal copy - find next run or end */
            uint32_t literal_start = src_pos;
            while (src_pos < src_size) {
                if (src_pos + 3 < src_size &&
                    src[src_pos] == src[src_pos+1] &&
                    src[src_pos] == src[src_pos+2] &&
                    src[src_pos] == src[src_pos+3]) {
                    break;
                }
                src_pos++;
                if (src_pos - literal_start >= 255) break;
            }
            uint32_t literal_len = src_pos - literal_start;
            if (literal_len > 0) {
                dst[dst_pos++] = literal_len;
                memcpy(&dst[dst_pos], &src[literal_start], literal_len);
                dst_pos += literal_len;
            }
        }
    }
    return dst_pos;
}

static uint32_t lz4_decompress(const uint8_t *src, uint8_t *dst, uint32_t src_size, uint32_t max_dst) {
    uint32_t src_pos = 0;
    uint32_t dst_pos = 0;

    while (src_pos < src_size && dst_pos < max_dst) {
        uint8_t token = src[src_pos++];
        if (token == 0xFF) {
            /* RLE */
            if (src_pos + 1 >= src_size) break;
            uint8_t byte = src[src_pos++];
            uint8_t run_len = src[src_pos++];
            for (int i = 0; i < run_len && dst_pos < max_dst; i++) {
                dst[dst_pos++] = byte;
            }
        } else {
            /* Literal */
            uint32_t len = token;
            if (src_pos + len > src_size) len = src_size - src_pos;
            if (dst_pos + len > max_dst) len = max_dst - dst_pos;
            memcpy(&dst[dst_pos], &src[src_pos], len);
            src_pos += len;
            dst_pos += len;
        }
    }
    return dst_pos;
}

/* zbud-style allocation: allocate from pool */
static void *zbud_alloc_internal(size_t size) {
    size = (size + 3) & ~3;  /* Align to 4 bytes */

    if (zswap_pool_used + size > ZSWAP_POOL_SIZE) {
        /* Try to find free space by scanning from beginning */
        for (uint32_t i = 0; i < ZSWAP_POOL_PAGES; i++) {
            if (!zswap_pool[i].in_use) {
                zswap_pool_used = 0;
                zswap_pool_next = 0;
                break;
            }
        }
        if (zswap_pool_used + size > ZSWAP_POOL_SIZE) {
            return NULL;
        }
    }

    void *ptr = &zswap_pool_memory[zswap_pool_next];
    zswap_pool_next += size;
    zswap_pool_used += size;
    return ptr;
}

static void zbud_free_internal(void *ptr, size_t size) {
    /* In a real implementation, we'd track free blocks.
     * For now, we just mark the pool as having free space when entries are freed. */
    (void)ptr;
    (void)size;
    /* Pool compaction would happen during reclaim */
}

void zswap_init(void) {
    if (zswap_inited) return;

    /* Allocate pool memory */
    uint32_t pool_pages = (ZSWAP_POOL_SIZE + PAGECACHE_PAGE_SIZE - 1) / PAGECACHE_PAGE_SIZE;
    zswap_pool_memory = (uint8_t *)vmm_alloc(pool_pages * PAGECACHE_PAGE_SIZE, VM_READ | VM_WRITE, "zswap-pool");
    if (!zswap_pool_memory) {
        /* Fallback: use physical pages directly */
        zswap_pool_memory = (uint8_t *)pmm_alloc_frame();
        if (!zswap_pool_memory) return;
    }

    memset(zswap_pool, 0, sizeof(zswap_pool));
    zswap_pool_used = 0;
    zswap_pool_next = 0;
    zswap_inited = 1;
}

int zswap_store(struct page *page) {
    if (!page || !page->present || !zswap_inited) return -1;

    mutex_lock(&zswap_mtx);

    /* Compress the page */
    uint8_t compressed[PAGECACHE_PAGE_SIZE];
    uint32_t compressed_size = lz4_compress((uint8_t *)page->phys, compressed, PAGECACHE_PAGE_SIZE);

    /* Only store if we achieve good compression */
    if (compressed_size >= PAGECACHE_PAGE_SIZE / 2) {
        mutex_unlock(&zswap_mtx);
        return -1;  /* Not worth compressing */
    }

    /* Allocate space in pool */
    void *pool_ptr = zbud_alloc_internal(compressed_size);
    if (!pool_ptr) {
        mutex_unlock(&zswap_mtx);
        return -1;
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < ZSWAP_POOL_PAGES; i++) {
        if (!zswap_pool[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        zbud_free_internal(pool_ptr, compressed_size);
        mutex_unlock(&zswap_mtx);
        return -1;
    }

    /* Store compressed data */
    memcpy(pool_ptr, compressed, compressed_size);
    zswap_pool[slot].data = (uint32_t *)pool_ptr;
    zswap_pool[slot].compressed_size = compressed_size;
    zswap_pool[slot].original_size = PAGECACHE_PAGE_SIZE;
    zswap_pool[slot].checksum = 0;
    for (uint32_t i = 0; i < PAGECACHE_PAGE_SIZE / 4; i++) {
        zswap_pool[slot].checksum += ((uint32_t *)page->phys)[i];
    }
    zswap_pool[slot].owner_page = (uint8_t *)page;
    zswap_pool[slot].in_use = 1;

    /* Free the original page frame */
    pmm_free_frame(page->phys);
    page->phys = 0;
    page->present = 0;
    page->dirty = 0;  /* Now stored in zswap */

    mutex_unlock(&zswap_mtx);
    return 0;
}

int zswap_load(struct page *page) {
    if (!page || page->present || !zswap_inited) return -1;

    mutex_lock(&zswap_mtx);

    /* Find our slot */
    int slot = -1;
    for (int i = 0; i < ZSWAP_POOL_PAGES; i++) {
        if (zswap_pool[i].in_use && zswap_pool[i].owner_page == (uint8_t *)page) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        mutex_unlock(&zswap_mtx);
        return -1;
    }

    /* Allocate new page frame */
    uint32_t frame = pmm_alloc_frame();
    if (!frame) {
        mutex_unlock(&zswap_mtx);
        return -1;
    }

    /* Decompress */
    uint32_t decompressed = lz4_decompress(
        (uint8_t *)zswap_pool[slot].data,
        (uint8_t *)frame,
        zswap_pool[slot].compressed_size,
        PAGECACHE_PAGE_SIZE
    );

    if (decompressed != PAGECACHE_PAGE_SIZE) {
        pmm_free_frame(frame);
        mutex_unlock(&zswap_mtx);
        return -1;
    }

    /* Verify checksum */
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < PAGECACHE_PAGE_SIZE / 4; i++) {
        checksum += ((uint32_t *)frame)[i];
    }
    if (checksum != zswap_pool[slot].checksum) {
        pmm_free_frame(frame);
        mutex_unlock(&zswap_mtx);
        return -1;
    }

    /* Restore page */
    page->phys = frame;
    page->present = 1;
    page->dirty = 0;

    /* Free pool entry */
    zbud_free_internal(zswap_pool[slot].data, zswap_pool[slot].compressed_size);
    zswap_pool[slot].in_use = 0;
    zswap_pool[slot].owner_page = NULL;

    mutex_unlock(&zswap_mtx);
    return 0;
}

void zswap_invalidate(struct page *page) {
    if (!page || !zswap_inited) return;

    mutex_lock(&zswap_mtx);

    for (int i = 0; i < ZSWAP_POOL_PAGES; i++) {
        if (zswap_pool[i].in_use && zswap_pool[i].owner_page == (uint8_t *)page) {
            zbud_free_internal(zswap_pool[i].data, zswap_pool[i].compressed_size);
            zswap_pool[i].in_use = 0;
            zswap_pool[i].owner_page = NULL;
            break;
        }
    }

    mutex_unlock(&zswap_mtx);
}

void zswap_get_stats(struct zswap_stats *out) {
    if (!out) return;

    mutex_lock(&zswap_mtx);

    out->pool_total_bytes = ZSWAP_POOL_SIZE;
    out->pool_used_bytes = zswap_pool_used;
    out->pages_stored = 0;
    for (int i = 0; i < ZSWAP_POOL_PAGES; i++) {
        if (zswap_pool[i].in_use) out->pages_stored++;
    }
    out->compression_ratio = zswap_pool_used > 0 ?
        (PAGECACHE_PAGE_SIZE * out->pages_stored * 100) / zswap_pool_used : 0;

    mutex_unlock(&zswap_mtx);
}

/* Public zbud interface */
void *zbud_alloc(size_t size) {
    if (!zswap_inited) zswap_init();
    mutex_lock(&zswap_mtx);
    void *ptr = zbud_alloc_internal(size);
    mutex_unlock(&zswap_mtx);
    return ptr;
}

void zbud_free(void *ptr, size_t size) {
    if (!zswap_inited) return;
    mutex_lock(&zswap_mtx);
    zbud_free_internal(ptr, size);
    mutex_unlock(&zswap_mtx);
}