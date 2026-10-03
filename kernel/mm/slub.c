/* Minimal SLUB-like allocator. Not a full SLUB rewrite yet: it provides
 * power-of-two caches backed by VMM pages, then large allocs by whole
 * pages. Existing heap structures stay in place; kmalloc/kfree/krealloc/
 * kzalloc are wired to this on Phase 1.
 */
#include "lib/heap.h"
#include "vmm.h"
#include "lib/string.h"
#include "sync.h"
#include <stdint.h>
#include <stddef.h>

#define SLUB_CHAIN_SIZES 10
#define SLUB_MAX_CACHE 2048
#define SLUB_PAGE 4096u

struct slab_hdr {
    uint32_t size;
    uint32_t capacity;
    void *free;
};

struct large_hdr {
    void *base;
    uint32_t size;
};

static mutex_t slub_mtx = MUTEX_INIT("slub");
static struct {
    uint32_t size;
    void *free;
} caches[SLUB_CHAIN_SIZES];

static int slub_inited;

static void slub_init(void) {
    if (slub_inited) {
        return;
    }
    uint32_t s = 8;
    for (int i = 0; i < SLUB_CHAIN_SIZES; i++) {
        caches[i].size = s;
        caches[i].free = NULL;
        s <<= 1;
    }
    slub_inited = 1;
}

static void slab_page_new(int idx) {
    uint32_t csize = caches[idx].size;
    void *page = vmm_alloc(SLUB_PAGE, VM_READ | VM_WRITE | VM_DEMAND, "slub");
    if (!page) {
        return;
    }
    struct slab_hdr *h = (struct slab_hdr *)page;
    h->size = csize;
    h->capacity = (SLUB_PAGE - sizeof(*h)) / csize;
    if (h->capacity == 0) {
        h->free = NULL;
        return;
    }
    uint8_t *p = (uint8_t *)page + sizeof(*h);
    for (uint32_t i = 0; i < h->capacity; i++) {
        *(void **)p = h->free;
        h->free = p;
        p += csize;
    }
    /* Objects are handed out from the slab's free chain; when empty, a new
     * slab page is allocated. We keep separate pages rather than linking
     * slabs, which is sufficient for this early implementation. */
    void *obj = h->free;
    h->free = *(void **)obj;
    *(void **)obj = NULL;
    /* Put the first object directly in the cache free head? Actually, for
     * simplicity, push all objects back and let kmalloc pop one. */
    h->free = (uint8_t *)page + sizeof(*h);
    for (uint32_t i = 0; i < h->capacity; i++) {
        void *cur = (uint8_t *)h->free + (i == 0 ? 0 : i * csize);
        *(void **)cur = (i + 1 < h->capacity) ? (uint8_t *)h->free + (i + 1) * csize : NULL;
    }
    caches[idx].free = h->free;
}

static void *slub_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    slub_init();
    if (size > SLUB_MAX_CACHE) {
        uint32_t pages = (size + SLUB_PAGE - 1) / SLUB_PAGE + 1;
        void *base = vmm_alloc(pages * SLUB_PAGE, VM_READ | VM_WRITE | VM_DEMAND, "slub-large");
        if (!base) {
            return NULL;
        }
        struct large_hdr *h = (struct large_hdr *)base;
        h->base = base;
        h->size = (uint32_t)size;
        return (uint8_t *)base + sizeof(*h);
    }
    int idx = 0;
    while (idx < SLUB_CHAIN_SIZES && caches[idx].size < size) {
        idx++;
    }
    if (idx >= SLUB_CHAIN_SIZES) {
        return NULL;
    }
    mutex_lock(&slub_mtx);
    if (!caches[idx].free) {
        slab_page_new(idx);
    }
    void *p = caches[idx].free;
    if (p) {
        caches[idx].free = *(void **)p;
    }
    mutex_unlock(&slub_mtx);
    return p;
}

static void slub_free(void *ptr) {
    if (!ptr) {
        return;
    }
    slub_init();
    /* large allocations: header sits just before returned pointer */
    struct large_hdr *lh = (struct large_hdr *)((uint8_t *)ptr - sizeof(struct large_hdr));
    if (lh->base && lh->size > SLUB_MAX_CACHE) {
        vmm_free(lh->base);
        return;
    }
    uint8_t *page = (uint8_t *)((uintptr_t)ptr & ~(SLUB_PAGE - 1));
    struct slab_hdr *h = (struct slab_hdr *)page;
    mutex_lock(&slub_mtx);
    *(void **)ptr = h->free;
    h->free = ptr;
    mutex_unlock(&slub_mtx);
}

static void *slub_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return slub_alloc(size);
    }
    if (size == 0) {
        slub_free(ptr);
        return NULL;
    }
    slub_init();
    size_t old = 0;
    struct large_hdr *lh = (struct large_hdr *)((uint8_t *)ptr - sizeof(struct large_hdr));
    if (lh->base && lh->size > SLUB_MAX_CACHE) {
        old = lh->size;
    } else {
        uint8_t *page = (uint8_t *)((uintptr_t)ptr & ~(SLUB_PAGE - 1));
        old = ((struct slab_hdr *)page)->size;
    }
    if (size <= old) {
        return ptr;
    }
    void *np = slub_alloc(size);
    if (!np) {
        return NULL;
    }
    memcpy(np, ptr, old);
    slub_free(ptr);
    return np;
}

void *kmalloc(size_t size) {
    return slub_alloc(size);
}

void *kzalloc(size_t size) {
    void *p = slub_alloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void *krealloc(void *ptr, size_t size) {
    return slub_realloc(ptr, size);
}

void kfree(void *ptr) {
    slub_free(ptr);
}
