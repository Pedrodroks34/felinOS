#include "net/udp.h"
#include "net/ip.h"
#include "net/net.h"
#include "net/netif.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "sync.h"
#include "sched.h"
#include "drivers/pit.h"

struct udp_rx_msg {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    struct udp_rx_msg *next;
    uint8_t data[];
};

struct udp_pcb {
    int in_use;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    int connected;
    int closing;
    struct udp_rx_msg *rx_head, *rx_tail;
    int rx_count;
    spinlock_t lock;
};

static struct udp_pcb pcbs[UDP_MAX_PCB];
static uint16_t next_ephemeral;

void udp_init(void) {
    memset(pcbs, 0, sizeof(pcbs));
    for (int i = 0; i < UDP_MAX_PCB; i++) {
        spin_init(&pcbs[i].lock, "udp-pcb");
    }
    next_ephemeral = 49152;
}

static int port_in_use(uint16_t port) {
    for (int i = 0; i < UDP_MAX_PCB; i++) {
        if (pcbs[i].in_use && pcbs[i].local_port == port) {
            return 1;
        }
    }
    return 0;
}

static uint16_t alloc_ephemeral(void) {
    for (int i = 0; i < 20000; i++) {
        uint16_t port = next_ephemeral;
        next_ephemeral = (next_ephemeral == 65535) ? 49152 : (uint16_t)(next_ephemeral + 1);
        if (!port_in_use(port)) {
            return port;
        }
    }
    return 0;
}

struct udp_pcb *udp_open(void) {
    for (int i = 0; i < UDP_MAX_PCB; i++) {
        if (!pcbs[i].in_use) {
            pcbs[i].in_use = 1;
            pcbs[i].local_port = 0;
            pcbs[i].remote_ip = 0;
            pcbs[i].remote_port = 0;
            pcbs[i].connected = 0;
            pcbs[i].closing = 0;
            pcbs[i].rx_head = pcbs[i].rx_tail = NULL;
            pcbs[i].rx_count = 0;
            return &pcbs[i];
        }
    }
    return NULL;
}

void udp_close(struct udp_pcb *pcb) {
    if (!pcb) {
        return;
    }
    uint32_t f = spin_lock_irqsave(&pcb->lock);
    pcb->closing = 1;
    struct udp_rx_msg *m = pcb->rx_head;
    while (m) {
        struct udp_rx_msg *next = m->next;
        kfree(m);
        m = next;
    }
    pcb->rx_head = pcb->rx_tail = NULL;
    pcb->rx_count = 0;
    pcb->in_use = 0;
    spin_unlock_irqrestore(&pcb->lock, f);
    sched_wake(pcb);
}

int udp_bind(struct udp_pcb *pcb, uint16_t port) {
    if (port != 0 && port_in_use(port)) {
        return -1;
    }
    pcb->local_port = port ? port : alloc_ephemeral();
    return pcb->local_port ? 0 : -1;
}

int udp_connect(struct udp_pcb *pcb, uint32_t ip, uint16_t port) {
    if (pcb->local_port == 0) {
        pcb->local_port = alloc_ephemeral();
        if (!pcb->local_port) {
            return -1;
        }
    }
    pcb->remote_ip = ip;
    pcb->remote_port = port;
    pcb->connected = 1;
    return 0;
}

uint16_t udp_local_port(const struct udp_pcb *pcb) {
    return pcb->local_port;
}

int udp_has_data(const struct udp_pcb *pcb) {
    return pcb->rx_count > 0;
}

int udp_remote(const struct udp_pcb *pcb, uint32_t *ip, uint16_t *port) {
    if (!pcb->connected) {
        return 0;
    }
    if (ip) {
        *ip = pcb->remote_ip;
    }
    if (port) {
        *port = pcb->remote_port;
    }
    return 1;
}

int udp_sendto(struct udp_pcb *pcb, uint32_t ip, uint16_t port, const void *data, uint32_t len) {
    struct netif *nif = netif_default();

    if (!nif || len > UDP_MAX_DGRAM) {
        return -1;
    }
    if (pcb->local_port == 0) {
        pcb->local_port = alloc_ephemeral();
        if (!pcb->local_port) {
            return -1;
        }
    }
    struct netbuf *nb = netbuf_alloc();
    if (!nb) {
        return -1;
    }
    uint8_t *body = (uint8_t *)netbuf_push(nb, len);
    memcpy(body, data, len);
    struct udp_hdr *hdr = (struct udp_hdr *)netbuf_push(nb, UDP_HDR_LEN);
    hdr->src_port = net_htons(pcb->local_port);
    hdr->dst_port = net_htons(port);
    hdr->length = net_htons((uint16_t)(UDP_HDR_LEN + len));
    hdr->checksum = 0;

    struct {
        uint32_t src, dst;
        uint8_t zero, proto;
        uint16_t length;
    } __attribute__((packed)) pseudo;
    pseudo.src = net_htonl(nif->ip);
    pseudo.dst = net_htonl(ip);
    pseudo.zero = 0;
    pseudo.proto = IP_PROTO_UDP;
    pseudo.length = hdr->length;
    uint16_t csum = net_checksum2(&pseudo, sizeof(pseudo), nb->data, nb->len, 0);
    hdr->checksum = net_htons(csum ? csum : 0xFFFF);

    int r = ip_output(nb, ip, IP_PROTO_UDP);
    netbuf_free(nb);
    return r;
}

void udp_input(struct netbuf *nb, uint32_t src_ip, uint32_t dst_ip) {
    (void)dst_ip;
    if (nb->len < UDP_HDR_LEN) {
        return;
    }
    struct udp_hdr hdr;
    memcpy(&hdr, nb->data, UDP_HDR_LEN);
    uint16_t dport = net_ntohs(hdr.dst_port);
    uint16_t sport = net_ntohs(hdr.src_port);
    uint16_t total = net_ntohs(hdr.length);

    if (total < UDP_HDR_LEN || total > nb->len) {
        return;
    }
    uint32_t payload_len = total - UDP_HDR_LEN;

    for (int i = 0; i < UDP_MAX_PCB; i++) {
        struct udp_pcb *pcb = &pcbs[i];
        if (!pcb->in_use || pcb->local_port != dport) {
            continue;
        }
        if (pcb->connected && (pcb->remote_ip != src_ip || pcb->remote_port != sport)) {
            continue;
        }
        struct udp_rx_msg *msg = (struct udp_rx_msg *)kmalloc(sizeof(struct udp_rx_msg) + payload_len);
        if (!msg) {
            return;
        }
        msg->src_ip = src_ip;
        msg->src_port = sport;
        msg->len = (uint16_t)payload_len;
        msg->next = NULL;
        memcpy(msg->data, nb->data + UDP_HDR_LEN, payload_len);

        uint32_t f = spin_lock_irqsave(&pcb->lock);
        if (pcb->rx_count >= UDP_MAX_RXQ) {
            struct udp_rx_msg *old = pcb->rx_head;
            pcb->rx_head = old->next;
            if (!pcb->rx_head) {
                pcb->rx_tail = NULL;
            }
            pcb->rx_count--;
            kfree(old);
        }
        if (pcb->rx_tail) {
            pcb->rx_tail->next = msg;
        } else {
            pcb->rx_head = msg;
        }
        pcb->rx_tail = msg;
        pcb->rx_count++;
        spin_unlock_irqrestore(&pcb->lock, f);
        sched_wake(pcb);
        return;
    }
}

int udp_recvfrom(struct udp_pcb *pcb, void *buf, uint32_t len, uint32_t *src_ip, uint16_t *src_port,
                 uint32_t timeout_ticks) {
    uint32_t deadline = pit_ticks() + timeout_ticks;

    for (;;) {
        uint32_t f = spin_lock_irqsave(&pcb->lock);
        if (pcb->rx_head) {
            struct udp_rx_msg *msg = pcb->rx_head;
            pcb->rx_head = msg->next;
            if (!pcb->rx_head) {
                pcb->rx_tail = NULL;
            }
            pcb->rx_count--;
            spin_unlock_irqrestore(&pcb->lock, f);

            uint32_t n = msg->len < len ? msg->len : len;
            memcpy(buf, msg->data, n);
            if (src_ip) {
                *src_ip = msg->src_ip;
            }
            if (src_port) {
                *src_port = msg->src_port;
            }
            kfree(msg);
            return (int)n;
        }
        int closing = pcb->closing;
        spin_unlock_irqrestore(&pcb->lock, f);
        if (closing) {
            return -1;
        }
        if (timeout_ticks != 0xFFFFFFFFu && (int32_t)(pit_ticks() - deadline) >= 0) {
            return 0;
        }
        sched_wait_on_timeout(pcb, "udp-recv", 20);
    }
}
