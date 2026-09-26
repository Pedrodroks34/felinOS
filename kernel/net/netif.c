#include "net/netif.h"
#include "net/eth.h"
#include "lib/string.h"
#include "sync.h"
#include "sched.h"

#define RXQ_SIZE 32

static struct netif g_netif;
static int g_netif_present;

static struct netbuf *rxq[RXQ_SIZE];
static uint32_t rxq_head, rxq_tail, rxq_count;
static spinlock_t rxq_lock = SPINLOCK_INIT("netif-rxq");
static const int rxq_chan;

void netif_register(const uint8_t *mac, const char *driver_name, int (*transmit)(struct netbuf *nb)) {
    memset(&g_netif, 0, sizeof(g_netif));
    mac_copy(g_netif.mac, mac);
    strlcpy(g_netif.driver_name, driver_name, sizeof(g_netif.driver_name));
    g_netif.transmit = transmit;
    g_netif.up = 1;
    g_netif_present = 1;
}

struct netif *netif_default(void) {
    return g_netif_present ? &g_netif : NULL;
}

void netif_set_addr(uint32_t ip, uint32_t netmask, uint32_t gateway) {
    g_netif.ip = ip;
    g_netif.netmask = netmask;
    g_netif.gateway = gateway;
}

void netif_set_dns(uint32_t dns_server) {
    g_netif.dns_server = dns_server;
}

void netif_rx_enqueue(struct netbuf *nb) {
    uint32_t f = spin_lock_irqsave(&rxq_lock);
    if (rxq_count >= RXQ_SIZE) {
        spin_unlock_irqrestore(&rxq_lock, f);
        netbuf_free(nb);
        return;
    }
    rxq[rxq_tail] = nb;
    rxq_tail = (rxq_tail + 1) % RXQ_SIZE;
    rxq_count++;
    spin_unlock_irqrestore(&rxq_lock, f);
    sched_wake(&rxq_chan);
}

static struct netbuf *rxq_pop(void) {
    uint32_t f = spin_lock_irqsave(&rxq_lock);
    struct netbuf *nb = NULL;

    if (rxq_count > 0) {
        nb = rxq[rxq_head];
        rxq_head = (rxq_head + 1) % RXQ_SIZE;
        rxq_count--;
    }
    spin_unlock_irqrestore(&rxq_lock, f);
    return nb;
}

static void netif_rx_thread(void *arg) {
    (void)arg;
    for (;;) {
        struct netbuf *nb = rxq_pop();
        if (!nb) {
            sched_wait_on_timeout(&rxq_chan, "net-rx", 50);
            continue;
        }
        if (g_netif_present) {
            g_netif.rx_packets++;
            g_netif.rx_bytes += nb->len;
        }
        eth_input(nb);
        netbuf_free(nb);
    }
}

void netif_start_rx_thread(void) {
    kthread_create("netrx", netif_rx_thread, NULL);
}
