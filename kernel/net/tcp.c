#include "net/tcp.h"
#include "net/ip.h"
#include "net/net.h"
#include "net/netif.h"
#include "lib/string.h"
#include "lib/heap.h"
#include "sync.h"
#include "sched.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"

#define TCP_RTO_INITIAL   50
#define TCP_RTO_MAX       800
#define TCP_MAX_RETRIES   6

struct tcp_pcb {
    int in_use;
    enum tcp_state state;

    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;

    uint8_t *out_data;
    uint32_t out_len;
    uint32_t out_seq;
    uint8_t out_flags;
    int out_pending;
    uint32_t retries;
    uint32_t rto;
    uint32_t rto_deadline;

    uint8_t *rx_buf;
    uint32_t rx_head, rx_tail, rx_count;

    struct tcp_pcb *backlog[TCP_BACKLOG];
    int backlog_count;
    int listening;
    int max_backlog;

    int peer_reset;
    int active_close_done;

    const void *rx_chan;
    const void *tx_chan;
    const void *acc_chan;
};

static struct tcp_pcb pcbs[TCP_MAX_PCB];
static spinlock_t tcp_lock = SPINLOCK_INIT("tcp-pcbs");
static uint16_t next_ephemeral;

void tcp_init(void) {
    memset(pcbs, 0, sizeof(pcbs));
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        pcbs[i].rx_chan = &pcbs[i].rx_head;
        pcbs[i].tx_chan = &pcbs[i].out_pending;
        pcbs[i].acc_chan = &pcbs[i].backlog_count;
    }
    next_ephemeral = 50000;
}

const char *tcp_state_name(int state) {
    static const char *names[] = {
        "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED",
        "FIN_WAIT1", "FIN_WAIT2", "CLOSE_WAIT", "LAST_ACK", "CLOSING", "TIME_WAIT"
    };
    if (state < 0 || state > TCP_TIME_WAIT) {
        return "?";
    }
    return names[state];
}

int tcp_state_of(const struct tcp_pcb *pcb) {
    return pcb->state;
}

uint16_t tcp_local_port(const struct tcp_pcb *pcb) {
    return pcb->local_port;
}

int tcp_remote(const struct tcp_pcb *pcb, uint32_t *ip, uint16_t *port) {
    if (!pcb || !pcb->in_use) {
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

static int port_in_use(uint16_t port) {
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        if (pcbs[i].in_use && pcbs[i].local_port == port) {
            return 1;
        }
    }
    return 0;
}

static uint16_t alloc_ephemeral(void) {
    for (int i = 0; i < 20000; i++) {
        uint16_t port = next_ephemeral;
        next_ephemeral = (next_ephemeral == 65535) ? 50000 : (uint16_t)(next_ephemeral + 1);
        if (!port_in_use(port)) {
            return port;
        }
    }
    return 0;
}

static uint32_t tcp_isn(void) {
    return (pit_ticks() * 250009u) ^ (uint32_t)(uintptr_t)&pcbs ^ rtc_unix();
}

struct tcp_pcb *tcp_open(void) {
    uint32_t f = spin_lock_irqsave(&tcp_lock);
    struct tcp_pcb *pcb = NULL;

    for (int i = 0; i < TCP_MAX_PCB; i++) {
        if (!pcbs[i].in_use) {
            pcb = &pcbs[i];
            pcb->in_use = 1;
            pcb->state = TCP_CLOSED;
            pcb->local_port = 0;
            pcb->remote_ip = 0;
            pcb->remote_port = 0;
            pcb->snd_una = pcb->snd_nxt = pcb->rcv_nxt = 0;
            pcb->out_data = NULL;
            pcb->out_len = pcb->out_seq = 0;
            pcb->out_flags = 0;
            pcb->out_pending = 0;
            pcb->retries = 0;
            pcb->rx_buf = NULL;
            pcb->rx_head = pcb->rx_tail = pcb->rx_count = 0;
            pcb->backlog_count = 0;
            pcb->listening = 0;
            pcb->max_backlog = 0;
            pcb->peer_reset = 0;
            pcb->active_close_done = 0;
            memset(pcb->backlog, 0, sizeof(pcb->backlog));
            break;
        }
    }
    spin_unlock_irqrestore(&tcp_lock, f);
    return pcb;
}

void tcp_release(struct tcp_pcb *pcb) {
    if (!pcb) {
        return;
    }
    uint32_t f = spin_lock_irqsave(&tcp_lock);
    if (pcb->out_data) {
        kfree(pcb->out_data);
        pcb->out_data = NULL;
    }
    if (pcb->rx_buf) {
        kfree(pcb->rx_buf);
        pcb->rx_buf = NULL;
    }
    pcb->state = TCP_CLOSED;
    pcb->in_use = 0;
    spin_unlock_irqrestore(&tcp_lock, f);
    sched_wake(pcb->rx_chan);
    sched_wake(pcb->tx_chan);
    sched_wake(pcb->acc_chan);
}

int tcp_bind(struct tcp_pcb *pcb, uint16_t port) {
    if (port != 0 && port_in_use(port)) {
        return -1;
    }
    pcb->local_port = port ? port : alloc_ephemeral();
    return pcb->local_port ? 0 : -1;
}

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *seg, uint32_t seg_len) {
    struct {
        uint32_t src, dst;
        uint8_t zero, proto;
        uint16_t length;
    } __attribute__((packed)) pseudo;

    pseudo.src = net_htonl(src_ip);
    pseudo.dst = net_htonl(dst_ip);
    pseudo.zero = 0;
    pseudo.proto = IP_PROTO_TCP;
    pseudo.length = net_htons((uint16_t)seg_len);
    return net_checksum2(&pseudo, sizeof(pseudo), seg, seg_len, 0);
}

static int tcp_transmit(struct tcp_pcb *pcb, uint32_t seq, uint8_t flags,
                         const void *data, uint32_t len) {
    struct netif *nif = netif_default();

    if (!nif) {
        return -1;
    }
    struct netbuf *nb = netbuf_alloc();
    if (!nb) {
        return -1;
    }
    if (len > 0) {
        memcpy(netbuf_push(nb, len), data, len);
    }
    struct tcp_hdr *hdr = (struct tcp_hdr *)netbuf_push(nb, TCP_HDR_LEN);
    hdr->src_port = net_htons(pcb->local_port);
    hdr->dst_port = net_htons(pcb->remote_port);
    hdr->seq = net_htonl(seq);
    hdr->ack = net_htonl((flags & TCP_FLAG_ACK) ? pcb->rcv_nxt : 0);
    hdr->data_off = (uint8_t)((TCP_HDR_LEN / 4) << 4);
    hdr->flags = flags;
    hdr->window = net_htons((uint16_t)(TCP_RX_CAP - pcb->rx_count));
    hdr->checksum = 0;
    hdr->urgent = 0;
    hdr->checksum = net_htons(tcp_checksum(nif->ip, pcb->remote_ip, nb->data, nb->len));

    int r = ip_output(nb, pcb->remote_ip, IP_PROTO_TCP);
    netbuf_free(nb);
    return r;
}

static void tcp_send_reset_for(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                                uint32_t seq, uint32_t ack, int ack_valid) {
    struct netif *nif = netif_default();

    if (!nif) {
        return;
    }
    struct netbuf *nb = netbuf_alloc();
    if (!nb) {
        return;
    }
    struct tcp_hdr *hdr = (struct tcp_hdr *)netbuf_push(nb, TCP_HDR_LEN);
    hdr->src_port = net_htons(src_port);
    hdr->dst_port = net_htons(dst_port);
    hdr->seq = net_htonl(ack_valid ? ack : 0);
    hdr->ack = net_htonl(seq);
    hdr->data_off = (uint8_t)((TCP_HDR_LEN / 4) << 4);
    hdr->flags = TCP_FLAG_RST | (ack_valid ? TCP_FLAG_ACK : 0);
    hdr->window = 0;
    hdr->checksum = 0;
    hdr->urgent = 0;
    hdr->checksum = net_htons(tcp_checksum(nif->ip, dst_ip, nb->data, nb->len));
    ip_output(nb, dst_ip, IP_PROTO_TCP);
    netbuf_free(nb);
}

static void arm_retransmit(struct tcp_pcb *pcb) {
    pcb->out_pending = 1;
    pcb->retries = 0;
    pcb->rto = TCP_RTO_INITIAL;
    pcb->rto_deadline = pit_ticks() + pcb->rto;
}

static int send_unit(struct tcp_pcb *pcb, uint32_t seq, uint8_t flags, const void *data, uint32_t len) {
    if (pcb->out_data) {
        kfree(pcb->out_data);
        pcb->out_data = NULL;
    }
    if (len > 0) {
        pcb->out_data = (uint8_t *)kmalloc(len);
        if (!pcb->out_data) {
            return -1;
        }
        memcpy(pcb->out_data, data, len);
    }
    pcb->out_seq = seq;
    pcb->out_len = len + (((flags & TCP_FLAG_SYN) || (flags & TCP_FLAG_FIN)) ? 1 : 0);
    pcb->out_flags = flags;
    arm_retransmit(pcb);
    return tcp_transmit(pcb, seq, flags, data, len);
}

int tcp_listen(struct tcp_pcb *pcb, int backlog) {
    pcb->listening = 1;
    pcb->max_backlog = backlog > TCP_BACKLOG ? TCP_BACKLOG : (backlog < 1 ? 1 : backlog);
    pcb->state = TCP_LISTEN;
    return 0;
}

int tcp_connect(struct tcp_pcb *pcb, uint32_t ip, uint16_t port, uint32_t timeout_ticks) {
    struct netif *nif = netif_default();

    if (!nif || !nif->ip) {
        return -1;
    }
    if (pcb->local_port == 0) {
        pcb->local_port = alloc_ephemeral();
        if (!pcb->local_port) {
            return -1;
        }
    }
    pcb->remote_ip = ip;
    pcb->remote_port = port;
    pcb->local_ip = nif->ip;

    uint32_t iss = tcp_isn();
    pcb->snd_una = iss;
    pcb->snd_nxt = iss;
    pcb->state = TCP_SYN_SENT;
    send_unit(pcb, iss, TCP_FLAG_SYN, NULL, 0);
    pcb->snd_nxt = iss + 1;

    uint32_t deadline = pit_ticks() + timeout_ticks;
    while (pcb->state == TCP_SYN_SENT && !pcb->peer_reset &&
           (int32_t)(pit_ticks() - deadline) < 0) {
        sched_wait_on_timeout(pcb->tx_chan, "tcp-connect", 20);
    }
    if (pcb->peer_reset) {
        return -1;
    }
    return pcb->state == TCP_ESTABLISHED ? 0 : -1;
}

struct tcp_pcb *tcp_accept(struct tcp_pcb *pcb, uint32_t timeout_ticks, uint32_t *remote_ip, uint16_t *remote_port) {
    uint32_t deadline = pit_ticks() + timeout_ticks;

    for (;;) {
        uint32_t f = spin_lock_irqsave(&tcp_lock);
        struct tcp_pcb *child = NULL;
        if (pcb->backlog_count > 0) {
            child = pcb->backlog[0];
            for (int i = 1; i < pcb->backlog_count; i++) {
                pcb->backlog[i - 1] = pcb->backlog[i];
            }
            pcb->backlog_count--;
        }
        spin_unlock_irqrestore(&tcp_lock, f);
        if (child) {
            if (remote_ip) {
                *remote_ip = child->remote_ip;
            }
            if (remote_port) {
                *remote_port = child->remote_port;
            }
            return child;
        }
        if (timeout_ticks != 0xFFFFFFFFu && (int32_t)(pit_ticks() - deadline) >= 0) {
            return NULL;
        }
        sched_wait_on_timeout(pcb->acc_chan, "tcp-accept", 20);
    }
}

static void rx_buf_alloc(struct tcp_pcb *pcb) {
    if (!pcb->rx_buf) {
        pcb->rx_buf = (uint8_t *)kmalloc(TCP_RX_CAP);
        pcb->rx_head = pcb->rx_tail = pcb->rx_count = 0;
    }
}

static uint32_t rx_buf_space(struct tcp_pcb *pcb) {
    return TCP_RX_CAP - pcb->rx_count;
}

static void rx_buf_put(struct tcp_pcb *pcb, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        pcb->rx_buf[pcb->rx_tail] = data[i];
        pcb->rx_tail = (pcb->rx_tail + 1) % TCP_RX_CAP;
    }
    pcb->rx_count += len;
}

static struct tcp_pcb *find_pcb(uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
    struct tcp_pcb *listener = NULL;

    for (int i = 0; i < TCP_MAX_PCB; i++) {
        struct tcp_pcb *p = &pcbs[i];
        if (!p->in_use) {
            continue;
        }
        if (p->local_port == dst_port && p->remote_ip == src_ip && p->remote_port == src_port &&
            p->state != TCP_LISTEN) {
            return p;
        }
        if (p->listening && p->local_port == dst_port) {
            listener = p;
        }
    }
    return listener;
}

void tcp_input(struct netbuf *nb, uint32_t src_ip, uint32_t dst_ip) {
    if (nb->len < TCP_HDR_LEN) {
        return;
    }
    struct tcp_hdr hdr;
    memcpy(&hdr, nb->data, TCP_HDR_LEN);
    uint32_t hlen = (uint32_t)((hdr.data_off >> 4) * 4);
    if (hlen < TCP_HDR_LEN || hlen > nb->len) {
        return;
    }
    netbuf_pull(nb, hlen);

    uint16_t src_port = net_ntohs(hdr.src_port);
    uint16_t dst_port = net_ntohs(hdr.dst_port);
    uint32_t seq = net_ntohl(hdr.seq);
    uint32_t ack = net_ntohl(hdr.ack);

    uint32_t f = spin_lock_irqsave(&tcp_lock);
    struct tcp_pcb *pcb = find_pcb(src_ip, src_port, dst_port);
    spin_unlock_irqrestore(&tcp_lock, f);

    if (!pcb) {
        if (!(hdr.flags & TCP_FLAG_RST)) {
            tcp_send_reset_for(src_ip, src_port, dst_port, seq + nb->len +
                                ((hdr.flags & TCP_FLAG_SYN) ? 1 : 0), ack, (hdr.flags & TCP_FLAG_ACK) != 0);
        }
        return;
    }

    if (hdr.flags & TCP_FLAG_RST) {
        pcb->peer_reset = 1;
        pcb->state = TCP_CLOSED;
        sched_wake(pcb->rx_chan);
        sched_wake(pcb->tx_chan);
        sched_wake(pcb->acc_chan);
        return;
    }

    if (pcb->listening) {
        if (!(hdr.flags & TCP_FLAG_SYN)) {
            return;
        }
        f = spin_lock_irqsave(&tcp_lock);
        if (pcb->backlog_count >= pcb->max_backlog) {
            spin_unlock_irqrestore(&tcp_lock, f);
            return;
        }
        struct tcp_pcb *child = NULL;
        for (int i = 0; i < TCP_MAX_PCB; i++) {
            if (!pcbs[i].in_use) {
                child = &pcbs[i];
                child->in_use = 1;
                break;
            }
        }
        spin_unlock_irqrestore(&tcp_lock, f);
        if (!child) {
            return;
        }
        child->state = TCP_SYN_RCVD;
        child->local_port = dst_port;
        child->local_ip = dst_ip;
        child->remote_ip = src_ip;
        child->remote_port = src_port;
        child->rcv_nxt = seq + 1;
        child->out_data = NULL;
        child->rx_buf = NULL;
        child->rx_head = child->rx_tail = child->rx_count = 0;
        child->backlog_count = 0;
        child->listening = 0;
        child->peer_reset = 0;
        child->active_close_done = 0;
        child->rx_chan = &child->rx_head;
        child->tx_chan = &child->out_pending;
        child->acc_chan = &child->backlog_count;

        uint32_t iss = tcp_isn();
        child->snd_una = iss;
        child->snd_nxt = iss;
        send_unit(child, iss, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
        child->snd_nxt = iss + 1;

        f = spin_lock_irqsave(&tcp_lock);
        pcb->backlog[pcb->backlog_count++] = child;
        spin_unlock_irqrestore(&tcp_lock, f);
        return;
    }

    if (pcb->state == TCP_SYN_SENT) {
        if ((hdr.flags & TCP_FLAG_SYN) && (hdr.flags & TCP_FLAG_ACK) && ack == pcb->snd_nxt) {
            pcb->rcv_nxt = seq + 1;
            pcb->snd_una = ack;
            pcb->out_pending = 0;
            pcb->state = TCP_ESTABLISHED;
            rx_buf_alloc(pcb);
            tcp_transmit(pcb, pcb->snd_nxt, TCP_FLAG_ACK, NULL, 0);
            sched_wake(pcb->tx_chan);
        }
        return;
    }

    if (pcb->out_pending && ack == pcb->snd_una + pcb->out_len && (hdr.flags & TCP_FLAG_ACK)) {
        pcb->snd_una = ack;
        pcb->out_pending = 0;
        if (pcb->out_data) {
            kfree(pcb->out_data);
            pcb->out_data = NULL;
        }
        if (pcb->state == TCP_SYN_RCVD) {
            pcb->state = TCP_ESTABLISHED;
            rx_buf_alloc(pcb);
        } else if (pcb->state == TCP_FIN_WAIT1) {
            pcb->state = TCP_FIN_WAIT2;
        } else if (pcb->state == TCP_LAST_ACK) {
            pcb->state = TCP_CLOSED;
        } else if (pcb->state == TCP_CLOSING) {
            pcb->state = TCP_TIME_WAIT;
        }
        sched_wake(pcb->tx_chan);
    }

    uint32_t payload_len = nb->len;
    if (payload_len > 0 && seq == pcb->rcv_nxt) {
        if (pcb->rx_buf && rx_buf_space(pcb) >= payload_len) {
            rx_buf_put(pcb, nb->data, payload_len);
            pcb->rcv_nxt += payload_len;
            sched_wake(pcb->rx_chan);
        } else {
            payload_len = 0;
        }
    } else if (payload_len > 0) {
        payload_len = 0;
    }

    if ((hdr.flags & TCP_FLAG_FIN) && seq + payload_len == pcb->rcv_nxt) {
        pcb->rcv_nxt++;
        if (pcb->state == TCP_ESTABLISHED) {
            pcb->state = TCP_CLOSE_WAIT;
        } else if (pcb->state == TCP_FIN_WAIT1) {
            pcb->state = TCP_CLOSING;
        } else if (pcb->state == TCP_FIN_WAIT2) {
            pcb->state = TCP_TIME_WAIT;
        }
        sched_wake(pcb->rx_chan);
    }

    if (payload_len > 0 || (hdr.flags & (TCP_FLAG_FIN | TCP_FLAG_SYN))) {
        tcp_transmit(pcb, pcb->snd_nxt, TCP_FLAG_ACK, NULL, 0);
    }
}

int tcp_send(struct tcp_pcb *pcb, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sent = 0;

    while (sent < len) {
        if (pcb->state != TCP_ESTABLISHED && pcb->state != TCP_CLOSE_WAIT) {
            return sent > 0 ? (int)sent : -1;
        }
        uint32_t chunk = len - sent;
        if (chunk > TCP_SEND_MSS) {
            chunk = TCP_SEND_MSS;
        }
        uint32_t seq = pcb->snd_nxt;
        send_unit(pcb, seq, TCP_FLAG_ACK | TCP_FLAG_PSH, p + sent, chunk);
        pcb->snd_nxt = seq + chunk;

        while (pcb->out_pending && !pcb->peer_reset) {
            sched_wait_on_timeout(pcb->tx_chan, "tcp-send", 20);
        }
        if (pcb->peer_reset) {
            return sent > 0 ? (int)sent : -1;
        }
        sent += chunk;
    }
    return (int)sent;
}

int tcp_recv(struct tcp_pcb *pcb, void *buf, uint32_t len, uint32_t timeout_ticks) {
    uint32_t deadline = pit_ticks() + timeout_ticks;
    uint8_t *out = (uint8_t *)buf;

    for (;;) {
        if (pcb->rx_buf && pcb->rx_count > 0) {
            uint32_t n = pcb->rx_count < len ? pcb->rx_count : len;
            for (uint32_t i = 0; i < n; i++) {
                out[i] = pcb->rx_buf[pcb->rx_head];
                pcb->rx_head = (pcb->rx_head + 1) % TCP_RX_CAP;
            }
            pcb->rx_count -= n;
            return (int)n;
        }
        if (pcb->state == TCP_CLOSE_WAIT || pcb->state == TCP_CLOSING ||
            pcb->state == TCP_TIME_WAIT || pcb->state == TCP_CLOSED || pcb->peer_reset) {
            return 0;
        }
        if (timeout_ticks != 0xFFFFFFFFu && (int32_t)(pit_ticks() - deadline) >= 0) {
            return 0;
        }
        sched_wait_on_timeout(pcb->rx_chan, "tcp-recv", 20);
    }
}

int tcp_close(struct tcp_pcb *pcb) {
    if (pcb->active_close_done) {
        return 0;
    }
    pcb->active_close_done = 1;
    if (pcb->state == TCP_ESTABLISHED) {
        uint32_t seq = pcb->snd_nxt;
        send_unit(pcb, seq, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
        pcb->snd_nxt = seq + 1;
        pcb->state = TCP_FIN_WAIT1;
    } else if (pcb->state == TCP_CLOSE_WAIT) {
        uint32_t seq = pcb->snd_nxt;
        send_unit(pcb, seq, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
        pcb->snd_nxt = seq + 1;
        pcb->state = TCP_LAST_ACK;
    } else if (pcb->state == TCP_SYN_SENT || pcb->state == TCP_LISTEN) {
        pcb->state = TCP_CLOSED;
    }
    return 0;
}

void tcp_timer_tick(void) {
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        struct tcp_pcb *pcb = &pcbs[i];
        if (!pcb->in_use || !pcb->out_pending) {
            continue;
        }
        if ((int32_t)(pit_ticks() - pcb->rto_deadline) < 0) {
            continue;
        }
        pcb->retries++;
        if (pcb->retries > TCP_MAX_RETRIES) {
            pcb->peer_reset = 1;
            pcb->out_pending = 0;
            pcb->state = TCP_CLOSED;
            sched_wake(pcb->rx_chan);
            sched_wake(pcb->tx_chan);
            sched_wake(pcb->acc_chan);
            continue;
        }
        pcb->rto = pcb->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : pcb->rto * 2;
        pcb->rto_deadline = pit_ticks() + pcb->rto;
        tcp_transmit(pcb, pcb->out_seq, pcb->out_flags, pcb->out_data,
                     pcb->out_len - (((pcb->out_flags & TCP_FLAG_SYN) || (pcb->out_flags & TCP_FLAG_FIN)) ? 1 : 0));
    }
}
