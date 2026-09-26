#include "net/eth.h"
#include "net/netif.h"
#include "net/arp.h"
#include "net/ip.h"
#include "lib/string.h"

void eth_input(struct netbuf *nb) {
    if (nb->len < ETH_HDR_LEN) {
        return;
    }
    struct eth_hdr hdr;
    memcpy(&hdr, nb->data, ETH_HDR_LEN);
    netbuf_pull(nb, ETH_HDR_LEN);

    uint16_t ethertype = net_ntohs(hdr.ethertype);
    if (ethertype == ETH_TYPE_ARP) {
        arp_input(nb, hdr.src);
    } else if (ethertype == ETH_TYPE_IP4) {
        ip_input(nb, hdr.src);
    }
}

int eth_output(struct netbuf *nb, const uint8_t *dst_mac, uint16_t ethertype) {
    struct netif *nif = netif_default();

    if (!nif || !nif->up) {
        return -1;
    }
    struct eth_hdr *hdr = (struct eth_hdr *)netbuf_push(nb, ETH_HDR_LEN);
    if (!hdr) {
        return -1;
    }
    mac_copy(hdr->dst, dst_mac);
    mac_copy(hdr->src, nif->mac);
    hdr->ethertype = net_htons(ethertype);

    int r = nif->transmit(nb);
    if (r == 0) {
        nif->tx_packets++;
        nif->tx_bytes += nb->len;
    } else {
        nif->tx_errors++;
    }
    return r;
}
