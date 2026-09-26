#ifndef FELINOS_UDP_H
#define FELINOS_UDP_H

#include <stdint.h>
#include "net/netbuf.h"

#define UDP_HDR_LEN 8
#define UDP_MAX_PCB 32
#define UDP_MAX_RXQ 16
#define UDP_MAX_DGRAM 1472

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct udp_pcb;

void udp_init(void);
void udp_input(struct netbuf *nb, uint32_t src_ip, uint32_t dst_ip);

struct udp_pcb *udp_open(void);
void udp_close(struct udp_pcb *pcb);
int udp_bind(struct udp_pcb *pcb, uint16_t port);
int udp_connect(struct udp_pcb *pcb, uint32_t ip, uint16_t port);
int udp_sendto(struct udp_pcb *pcb, uint32_t ip, uint16_t port, const void *data, uint32_t len);
int udp_recvfrom(struct udp_pcb *pcb, void *buf, uint32_t len, uint32_t *src_ip, uint16_t *src_port,
                 uint32_t timeout_ticks);
uint16_t udp_local_port(const struct udp_pcb *pcb);
int udp_has_data(const struct udp_pcb *pcb);
int udp_remote(const struct udp_pcb *pcb, uint32_t *ip, uint16_t *port);

#endif
