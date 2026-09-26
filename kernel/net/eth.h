#ifndef FELINOS_ETH_H
#define FELINOS_ETH_H

#include <stdint.h>
#include "net/net.h"
#include "net/netbuf.h"

#define ETH_HDR_LEN   14
#define ETH_TYPE_IP4  0x0800
#define ETH_TYPE_ARP  0x0806

struct eth_hdr {
    uint8_t dst[MAC_LEN];
    uint8_t src[MAC_LEN];
    uint16_t ethertype;
} __attribute__((packed));

void eth_input(struct netbuf *nb);
int eth_output(struct netbuf *nb, const uint8_t *dst_mac, uint16_t ethertype);

#endif
