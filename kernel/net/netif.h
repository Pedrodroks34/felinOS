#ifndef FELINOS_NETIF_H
#define FELINOS_NETIF_H

#include <stdint.h>
#include "net/net.h"
#include "net/netbuf.h"

struct netif {
    mac_addr_t mac;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
    int up;
    int dhcp_bound;
    char driver_name[16];
    int (*transmit)(struct netbuf *nb);
    const char *(*link_status)(void);
    uint32_t rx_packets, tx_packets;
    uint32_t rx_bytes, tx_bytes;
    uint32_t rx_errors, tx_errors;
};

void netif_register(const uint8_t *mac, const char *driver_name, int (*transmit)(struct netbuf *nb),
                    const char *(*link_status)(void));
struct netif *netif_default(void);
void netif_set_addr(uint32_t ip, uint32_t netmask, uint32_t gateway);
void netif_set_dns(uint32_t dns_server);
void netif_rx_enqueue(struct netbuf *nb);
void netif_start_rx_thread(void);

#endif
