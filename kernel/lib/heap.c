#include "lib/heap.h"
#include "lib/string.h"
#include "sync.h"

#define HEAP_MAGIC 0x564C4845
#define HEAP_ALIGN 8

struct block {
    uint32_t magic;
    uint32_t size;
    uint8_t used;
    struct block *next;
    struct block *prev;
};

static mutex_t heap_mtx = MUTEX_INIT("heap");
static struct block *heap_head;
static uint32_t heap_total;

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

void heap_init(uint32_t start, uint32_t size) {
    start = align_up(start, HEAP_ALIGN);
    heap_head = (struct block *)start;
    heap_head->magic = HEAP_MAGIC;
    heap_head->size = size - sizeof(struct block);
    heap_head->used = 0;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    heap_total = size;
}

static void split_block(struct block *b, uint32_t size) {
    if (b->size < size + sizeof(struct block) + HEAP_ALIGN * 2) {
        return;
    }
    struct block *nb = (struct block *)((uint8_t *)b + sizeof(struct block) + size);
    nb->magic = HEAP_MAGIC;
    nb->size = b->size - size - sizeof(struct block);
    nb->used = 0;
    nb->next = b->next;
    nb->prev = b;
    if (b->next) {
        b->next->prev = nb;
    }
    b->next = nb;
    b->size = size;
}

static void *kmalloc_locked(size_t size) {
    if (size == 0 || !heap_head) {
        return NULL;
    }
    uint32_t want = align_up((uint32_t)size, HEAP_ALIGN);

    for (struct block *b = heap_head; b; b = b->next) {
        if (!b->used && b->size >= want) {
            split_block(b, want);
            b->used = 1;
            return (uint8_t *)b + sizeof(struct block);
        }
    }
    return NULL;
}



static void coalesce(struct block *b) {
    while (b->next && !b->next->used) {
        struct block *n = b->next;
        b->size += n->size + sizeof(struct block);
        b->next = n->next;
        if (n->next) {
            n->next->prev = b;
        }
    }
}

static void kfree_locked(void *ptr) {
    if (!ptr) {
        return;
    }
    struct block *b = (struct block *)((uint8_t *)ptr - sizeof(struct block));
    if (b->magic != HEAP_MAGIC) {
        return;
    }
    b->used = 0;
    coalesce(b);
    if (b->prev && !b->prev->used) {
        coalesce(b->prev);
    }
}

static void *krealloc_locked(void *ptr, size_t size) {
    if (!ptr) {
        return kmalloc_locked(size);
    }
    if (size == 0) {
        kfree_locked(ptr);
        return NULL;
    }
    struct block *b = (struct block *)((uint8_t *)ptr - sizeof(struct block));
    if (b->magic != HEAP_MAGIC) {
        return NULL;
    }
    if (b->size >= size) {
        return ptr;
    }
    if (b->next && !b->next->used &&
        b->size + sizeof(struct block) + b->next->size >= size) {
        coalesce(b);
        split_block(b, align_up((uint32_t)size, HEAP_ALIGN));
        return ptr;
    }
    void *np = kmalloc_locked(size);
    if (!np) {
        return NULL;
    }
    memcpy(np, ptr, b->size);
    kfree_locked(ptr);
    return np;
}

static void heap_grow_locked(uint32_t extra) {
    if (!heap_head || !extra) {
        return;
    }
    struct block *last = heap_head;
    while (last->next) {
        last = last->next;
    }
    if (!last->used) {
        last->size += extra;
    } else {
        struct block *nb = (struct block *)((uint8_t *)last + sizeof(struct block) + last->size);
        nb->magic = HEAP_MAGIC;
        nb->size = extra - sizeof(struct block);
        nb->used = 0;
        nb->next = NULL;
        nb->prev = last;
        last->next = nb;
    }
    heap_total += extra;
}

uint32_t heap_total_size(void) {
    return heap_total;
}

static void heap_stats_locked(uint32_t *total, uint32_t *used, uint32_t *largest_free, uint32_t *blocks) {
    uint32_t u = 0;
    uint32_t largest = 0;
    uint32_t count = 0;

    for (struct block *b = heap_head; b; b = b->next) {
        count++;
        if (b->used) {
            u += b->size + sizeof(struct block);
        } else if (b->size > largest) {
            largest = b->size;
        }
    }
    if (total) {
        *total = heap_total;
    }
    if (used) {
        *used = u;
    }
    if (largest_free) {
        *largest_free = largest;
    }
    if (blocks) {
        *blocks = count;
    }
}

/* Every entry point takes the heap mutex. It is a sleeping lock on purpose:
   the heap lives in demand-paged, swappable memory, so touching a block header
   can fault and block on disk I/O, which a spinlock could not tolerate. */
void *kmalloc(size_t size) {
    mutex_lock(&heap_mtx);
    void *p = kmalloc_locked(size);
    mutex_unlock(&heap_mtx);
    return p;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);

    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void kfree(void *ptr) {
    mutex_lock(&heap_mtx);
    kfree_locked(ptr);
    mutex_unlock(&heap_mtx);
}

void *krealloc(void *ptr, size_t size) {
    mutex_lock(&heap_mtx);
    void *p = krealloc_locked(ptr, size);
    mutex_unlock(&heap_mtx);
    return p;
}

void heap_grow(uint32_t extra) {
    mutex_lock(&heap_mtx);
    heap_grow_locked(extra);
    mutex_unlock(&heap_mtx);
}

void heap_stats(uint32_t *total, uint32_t *used, uint32_t *largest_free, uint32_t *blocks) {
    mutex_lock(&heap_mtx);
    heap_stats_locked(total, used, largest_free, blocks);
    mutex_unlock(&heap_mtx);
}
