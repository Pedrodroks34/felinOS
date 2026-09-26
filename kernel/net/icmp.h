#ifndef FELINOS_ICMP_H
#define FELINOS_ICMP_H

#include <stdint.h>
#include "net/netbuf.h"

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct ping_result {
    int replied;
    uint32_t src_ip;
    uint32_t rtt_ticks;
};

void icmp_input(struct netbuf *nb, uint32_t src_ip);
int icmp_ping(uint32_t dst_ip, uint16_t id, uint16_t seq, uint32_t timeout_ticks,
              struct ping_result *result);

#endif
