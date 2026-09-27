#ifndef FELINOS_TCP_H
#define FELINOS_TCP_H

#include <stdint.h>
#include "net/netbuf.h"

#define TCP_HDR_LEN 20
#define TCP_MAX_PCB 32
#define TCP_BACKLOG 4
#define TCP_SEND_MSS 536
#define TCP_RX_CAP 8192

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_off;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_CLOSING,
    TCP_TIME_WAIT
};

struct tcp_pcb;

void tcp_init(void);
void tcp_input(struct netbuf *nb, uint32_t src_ip, uint32_t dst_ip);
void tcp_timer_tick(void);

struct tcp_pcb *tcp_open(void);
void tcp_release(struct tcp_pcb *pcb);
int tcp_bind(struct tcp_pcb *pcb, uint16_t port);
int tcp_listen(struct tcp_pcb *pcb, int backlog);
int tcp_connect(struct tcp_pcb *pcb, uint32_t ip, uint16_t port, uint32_t timeout_ticks);
struct tcp_pcb *tcp_accept(struct tcp_pcb *pcb, uint32_t timeout_ticks, uint32_t *remote_ip, uint16_t *remote_port);
int tcp_send(struct tcp_pcb *pcb, const void *data, uint32_t len);
int tcp_recv(struct tcp_pcb *pcb, void *buf, uint32_t len, uint32_t timeout_ticks);
int tcp_close(struct tcp_pcb *pcb);
int tcp_state_of(const struct tcp_pcb *pcb);
uint16_t tcp_local_port(const struct tcp_pcb *pcb);
int tcp_remote(const struct tcp_pcb *pcb, uint32_t *ip, uint16_t *port);
const char *tcp_state_name(int state);

#endif
