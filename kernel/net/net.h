#ifndef FELINOS_NET_H
#define FELINOS_NET_H

#include <stdint.h>
#include <stddef.h>

static inline uint16_t net_htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint16_t net_ntohs(uint16_t v) {
    return net_htons(v);
}

static inline uint32_t net_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static inline uint32_t net_ntohl(uint32_t v) {
    return net_htonl(v);
}

#define MAC_LEN 6

typedef uint8_t mac_addr_t[MAC_LEN];

extern const mac_addr_t mac_broadcast;
extern const mac_addr_t mac_zero;

int mac_is_zero(const uint8_t *m);
int mac_is_broadcast(const uint8_t *m);
int mac_equal(const uint8_t *a, const uint8_t *b);
void mac_copy(uint8_t *dst, const uint8_t *src);
void mac_to_str(const uint8_t *m, char *out);

void ip4_to_str(uint32_t ip_host, char *out);
int ip4_from_str(const char *s, uint32_t *out_host);

uint16_t net_checksum(const void *data, uint32_t len, uint32_t seed);
uint16_t net_checksum2(const void *a, uint32_t alen, const void *b, uint32_t blen, uint32_t seed);

#endif
