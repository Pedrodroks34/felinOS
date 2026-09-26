#ifndef FELINOS_IP_H
#define FELINOS_IP_H

#include <stdint.h>
#include "net/net.h"
#include "net/netbuf.h"

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define IP_HDR_LEN 20
#define IP_DEFAULT_TTL 64

struct ip_hdr {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

void ip_init(void);
void ip_input(struct netbuf *nb, const uint8_t *src_mac);
int ip_output(struct netbuf *nb, uint32_t dst_ip, uint8_t proto);
uint32_t ip_route_next_hop(uint32_t dst_ip);

#endif
