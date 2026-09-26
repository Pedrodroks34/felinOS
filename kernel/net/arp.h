#ifndef FELINOS_ARP_H
#define FELINOS_ARP_H

#include <stdint.h>
#include "net/net.h"
#include "net/netbuf.h"

struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[MAC_LEN];
    uint32_t spa;
    uint8_t tha[MAC_LEN];
    uint32_t tpa;
} __attribute__((packed));

void arp_init(void);
void arp_input(struct netbuf *nb, const uint8_t *src_mac);
void arp_request(uint32_t target_ip);
int arp_lookup(uint32_t ip, uint8_t *mac_out);
int arp_resolve(uint32_t ip, uint8_t *mac_out, uint32_t timeout_ticks);
void arp_cache_dump(void (*cb)(uint32_t ip, const uint8_t *mac, uint32_t age_ticks, void *ctx), void *ctx);
void arp_age_tick(void);

#endif
