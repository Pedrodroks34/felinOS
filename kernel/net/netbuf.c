#include "net/netbuf.h"
#include <stddef.h>
#include "sync.h"

static struct netbuf pool[NETBUF_COUNT];
static struct netbuf *free_list;
static uint32_t free_count;
static spinlock_t pool_lock = SPINLOCK_INIT("netbuf-pool");

void netbuf_pool_init(void) {
    free_list = NULL;
    free_count = 0;
    for (int i = 0; i < NETBUF_COUNT; i++) {
        pool[i].free_next = free_list;
        free_list = &pool[i];
        free_count++;
    }
}

static struct netbuf *alloc_common(void) {
    uint32_t f = spin_lock_irqsave(&pool_lock);
    struct netbuf *nb = free_list;

    if (nb) {
        free_list = nb->free_next;
        free_count--;
    }
    spin_unlock_irqrestore(&pool_lock, f);
    if (nb) {
        nb->free_next = NULL;
        nb->len = 0;
    }
    return nb;
}

struct netbuf *netbuf_alloc(void) {
    struct netbuf *nb = alloc_common();

    if (nb) {
        nb->data = nb->storage + NETBUF_HEAD;
    }
    return nb;
}

struct netbuf *netbuf_alloc_raw(void) {
    struct netbuf *nb = alloc_common();

    if (nb) {
        nb->data = nb->storage;
    }
    return nb;
}

void netbuf_free(struct netbuf *nb) {
    if (!nb) {
        return;
    }
    uint32_t f = spin_lock_irqsave(&pool_lock);
    nb->free_next = free_list;
    free_list = nb;
    free_count++;
    spin_unlock_irqrestore(&pool_lock, f);
}

void *netbuf_push(struct netbuf *nb, uint32_t n) {
    if (n > (uint32_t)(nb->data - nb->storage)) {
        return NULL;
    }
    nb->data -= n;
    nb->len = (uint16_t)(nb->len + n);
    return nb->data;
}

void *netbuf_pull(struct netbuf *nb, uint32_t n) {
    uint8_t *old;

    if (n > nb->len) {
        return NULL;
    }
    old = nb->data;
    nb->data += n;
    nb->len = (uint16_t)(nb->len - n);
    return old;
}

uint32_t netbuf_headroom(struct netbuf *nb) {
    return (uint32_t)(nb->data - nb->storage);
}

uint32_t netbuf_free_count(void) {
    return free_count;
}
