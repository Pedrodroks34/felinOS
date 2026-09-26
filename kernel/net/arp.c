#include "net/arp.h"
#include "net/eth.h"
#include "net/netif.h"
#include "lib/string.h"
#include "sync.h"
#include "sched.h"
#include "drivers/pit.h"

#define ARP_HW_ETHER 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2
#define ARP_CACHE_SIZE 16
#define ARP_MAX_AGE_TICKS (300u * 100u)

struct arp_entry {
    uint32_t ip;
    uint8_t mac[MAC_LEN];
    uint32_t stamp;
    int valid;
};

static struct arp_entry cache[ARP_CACHE_SIZE];
static spinlock_t cache_lock = SPINLOCK_INIT("arp-cache");
static const int arp_wait_chan;

void arp_init(void) {
    memset(cache, 0, sizeof(cache));
}

static void cache_put(uint32_t ip, const uint8_t *mac) {
    uint32_t f = spin_lock_irqsave(&cache_lock);
    struct arp_entry *victim = &cache[0];

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            victim = &cache[i];
            break;
        }
        if (!cache[i].valid) {
            victim = &cache[i];
        } else if (cache[i].stamp < victim->stamp) {
            victim = &cache[i];
        }
    }
    victim->ip = ip;
    mac_copy(victim->mac, mac);
    victim->stamp = pit_ticks();
    victim->valid = 1;
    spin_unlock_irqrestore(&cache_lock, f);
    sched_wake(&arp_wait_chan);
}

int arp_lookup(uint32_t ip, uint8_t *mac_out) {
    uint32_t f = spin_lock_irqsave(&cache_lock);
    int found = 0;

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            mac_copy(mac_out, cache[i].mac);
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&cache_lock, f);
    return found;
}

void arp_request(uint32_t target_ip) {
    struct netif *nif = netif_default();
    struct netbuf *nb;

    if (!nif) {
        return;
    }
    nb = netbuf_alloc();
    if (!nb) {
        return;
    }
    struct arp_hdr *hdr = (struct arp_hdr *)netbuf_push(nb, sizeof(struct arp_hdr));
    hdr->htype = net_htons(ARP_HW_ETHER);
    hdr->ptype = net_htons(ETH_TYPE_IP4);
    hdr->hlen = MAC_LEN;
    hdr->plen = 4;
    hdr->oper = net_htons(ARP_OP_REQUEST);
    mac_copy(hdr->sha, nif->mac);
    hdr->spa = net_htonl(nif->ip);
    memset(hdr->tha, 0, MAC_LEN);
    hdr->tpa = net_htonl(target_ip);

    eth_output(nb, mac_broadcast, ETH_TYPE_ARP);
    netbuf_free(nb);
}

void arp_input(struct netbuf *nb, const uint8_t *src_mac) {
    (void)src_mac;
    if (nb->len < sizeof(struct arp_hdr)) {
        return;
    }
    struct arp_hdr hdr;
    memcpy(&hdr, nb->data, sizeof(hdr));

    if (net_ntohs(hdr.htype) != ARP_HW_ETHER || net_ntohs(hdr.ptype) != ETH_TYPE_IP4) {
        return;
    }
    uint32_t sender_ip = net_ntohl(hdr.spa);
    uint16_t oper = net_ntohs(hdr.oper);

    cache_put(sender_ip, hdr.sha);

    struct netif *nif = netif_default();
    if (!nif || nif->ip == 0) {
        return;
    }
    if (oper == ARP_OP_REQUEST && net_ntohl(hdr.tpa) == nif->ip) {
        struct netbuf *reply = netbuf_alloc();
        if (!reply) {
            return;
        }
        struct arp_hdr *rh = (struct arp_hdr *)netbuf_push(reply, sizeof(struct arp_hdr));
        rh->htype = net_htons(ARP_HW_ETHER);
        rh->ptype = net_htons(ETH_TYPE_IP4);
        rh->hlen = MAC_LEN;
        rh->plen = 4;
        rh->oper = net_htons(ARP_OP_REPLY);
        mac_copy(rh->sha, nif->mac);
        rh->spa = net_htonl(nif->ip);
        mac_copy(rh->tha, hdr.sha);
        rh->tpa = hdr.spa;
        eth_output(reply, hdr.sha, ETH_TYPE_ARP);
        netbuf_free(reply);
    }
}

int arp_resolve(uint32_t ip, uint8_t *mac_out, uint32_t timeout_ticks) {
    if (arp_lookup(ip, mac_out)) {
        return 1;
    }
    uint32_t deadline = pit_ticks() + timeout_ticks;
    int attempts = 0;

    while ((int32_t)(pit_ticks() - deadline) < 0) {
        arp_request(ip);
        attempts++;
        sched_wait_on_timeout(&arp_wait_chan, "arp-wait", 20);
        if (arp_lookup(ip, mac_out)) {
            return 1;
        }
        if (attempts >= 5) {
            break;
        }
    }
    return 0;
}

void arp_cache_dump(void (*cb)(uint32_t ip, const uint8_t *mac, uint32_t age_ticks, void *ctx), void *ctx) {
    uint32_t f = spin_lock_irqsave(&cache_lock);
    uint32_t now = pit_ticks();

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid) {
            cb(cache[i].ip, cache[i].mac, now - cache[i].stamp, ctx);
        }
    }
    spin_unlock_irqrestore(&cache_lock, f);
}

void arp_age_tick(void) {
    uint32_t f = spin_lock_irqsave(&cache_lock);
    uint32_t now = pit_ticks();

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && (now - cache[i].stamp) > ARP_MAX_AGE_TICKS) {
            cache[i].valid = 0;
        }
    }
    spin_unlock_irqrestore(&cache_lock, f);
}
