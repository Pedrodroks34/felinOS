#ifndef FELINOS_NETBUF_H
#define FELINOS_NETBUF_H

#include <stdint.h>

#define NETBUF_COUNT 64
#define NETBUF_HEAD  128
#define NETBUF_CAP   1600

struct netbuf {
    uint8_t storage[NETBUF_CAP];
    uint8_t *data;
    uint16_t len;
    struct netbuf *free_next;
};

void netbuf_pool_init(void);
struct netbuf *netbuf_alloc(void);
struct netbuf *netbuf_alloc_raw(void);
void netbuf_free(struct netbuf *nb);
void *netbuf_push(struct netbuf *nb, uint32_t n);
void *netbuf_put(struct netbuf *nb, uint32_t n);
void *netbuf_pull(struct netbuf *nb, uint32_t n);
uint32_t netbuf_headroom(struct netbuf *nb);
uint32_t netbuf_free_count(void);

#endif
