#include <stdint.h>
#include "malloc.h"
#include "usys.h"

struct blk {
    size_t size;
    struct blk *next;
    int free;
};

static struct blk *heap_head = NULL;

static struct blk *grow(size_t size) {
    size_t need = size + sizeof(struct blk);
    need = (need + 4095u) & ~(size_t)4095u;
    void *p = sbrk((int)need);
    if (p == (void *)(intptr_t)-1) {
        return NULL;
    }
    struct blk *b = (struct blk *)p;
    b->size = need - sizeof(struct blk);
    b->free = 0;
    b->next = NULL;
    return b;
}

void *malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size = (size + 15u) & ~(size_t)15u;

    struct blk *b = heap_head, *prev = NULL;
    while (b) {
        if (b->free && b->size >= size) {
            b->free = 0;
            return (void *)(b + 1);
        }
        prev = b;
        b = b->next;
    }
    b = grow(size);
    if (!b) {
        return NULL;
    }
    if (prev) {
        prev->next = b;
    } else {
        heap_head = b;
    }
    return (void *)(b + 1);
}

void free(void *ptr) {
    if (!ptr) {
        return;
    }
    struct blk *b = (struct blk *)ptr - 1;
    b->free = 1;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        uint8_t *d = (uint8_t *)p;
        for (size_t i = 0; i < total; i++) {
            d[i] = 0;
        }
    }
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    struct blk *b = (struct blk *)ptr - 1;
    if (b->size >= size) {
        return ptr;
    }
    void *n = malloc(size);
    if (!n) {
        return NULL;
    }
    uint8_t *d = (uint8_t *)n;
    const uint8_t *s = (const uint8_t *)ptr;
    for (size_t i = 0; i < b->size; i++) {
        d[i] = s[i];
    }
    free(ptr);
    return n;
}
