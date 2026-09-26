#include "net/ip.h"
#include "net/eth.h"
#include "net/arp.h"
#include "net/netif.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "lib/string.h"

static uint16_t ip_id_counter;

void ip_init(void) {
    ip_id_counter = 1;
}

uint32_t ip_route_next_hop(uint32_t dst_ip) {
    struct netif *nif = netif_default();

    if (!nif) {
        return 0;
    }
    if ((dst_ip & nif->netmask) == (nif->ip & nif->netmask)) {
        return dst_ip;
    }
    return nif->gateway;
}

void ip_input(struct netbuf *nb, const uint8_t *src_mac) {
    (void)src_mac;
    if (nb->len < IP_HDR_LEN) {
        return;
    }
    struct ip_hdr hdr;
    memcpy(&hdr, nb->data, IP_HDR_LEN);

    if ((hdr.ver_ihl >> 4) != 4) {
        return;
    }
    uint32_t ihl = (uint32_t)(hdr.ver_ihl & 0x0F) * 4;
    if (ihl < IP_HDR_LEN || ihl > nb->len) {
        return;
    }
    uint16_t total_len = net_ntohs(hdr.total_len);
    if (total_len < ihl || total_len > nb->len) {
        return;
    }

    struct netif *nif = netif_default();
    uint32_t dst_ip = net_ntohl(hdr.dst);
    if (nif && nif->ip != 0 && dst_ip != nif->ip && dst_ip != 0xFFFFFFFFu &&
        dst_ip != (nif->ip | ~nif->netmask)) {
        return;
    }

    netbuf_pull(nb, (uint32_t)ihl);
    nb->len = (uint16_t)(total_len - ihl);

    uint32_t src_ip = net_ntohl(hdr.src);

    switch (hdr.proto) {
    case IP_PROTO_ICMP:
        icmp_input(nb, src_ip);
        break;
    case IP_PROTO_UDP:
        udp_input(nb, src_ip, dst_ip);
        break;
    case IP_PROTO_TCP:
        tcp_input(nb, src_ip, dst_ip);
        break;
    default:
        break;
    }
}

int ip_output(struct netbuf *nb, uint32_t dst_ip, uint8_t proto) {
    struct netif *nif = netif_default();

    if (!nif || !nif->up) {
        return -1;
    }
    struct ip_hdr *hdr = (struct ip_hdr *)netbuf_push(nb, IP_HDR_LEN);
    if (!hdr) {
        return -1;
    }
    hdr->ver_ihl = 0x45;
    hdr->tos = 0;
    hdr->total_len = net_htons(nb->len);
    hdr->id = net_htons(ip_id_counter++);
    hdr->flags_frag = net_htons(0x4000);
    hdr->ttl = IP_DEFAULT_TTL;
    hdr->proto = proto;
    hdr->checksum = 0;
    hdr->src = net_htonl(nif->ip);
    hdr->dst = net_htonl(dst_ip);
    hdr->checksum = net_htons(net_checksum(hdr, IP_HDR_LEN, 0));

    uint32_t next_hop = ip_route_next_hop(dst_ip);
    if (next_hop == 0xFFFFFFFFu || dst_ip == 0xFFFFFFFFu ||
        (nif->netmask != 0 && next_hop == (nif->ip | ~nif->netmask))) {
        return eth_output(nb, mac_broadcast, ETH_TYPE_IP4);
    }

    uint8_t dst_mac[MAC_LEN];
    if (!arp_resolve(next_hop, dst_mac, 200)) {
        return -1;
    }
    return eth_output(nb, dst_mac, ETH_TYPE_IP4);
}
