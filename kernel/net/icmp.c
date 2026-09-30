#include "net/icmp.h"
#include "net/ip.h"
#include "net/net.h"
#include "lib/string.h"
#include "sync.h"
#include "sched.h"
#include "drivers/pit.h"

#define ICMP_MAX_PENDING 8
#define ICMP_PAYLOAD_LEN 32

struct pending_ping {
    int in_use;
    uint16_t id;
    uint16_t seq;
    int replied;
    uint32_t src_ip;
    uint32_t sent_tick;
    uint32_t reply_tick;
};

static struct pending_ping pending[ICMP_MAX_PENDING];
static spinlock_t pending_lock = SPINLOCK_INIT("icmp-pending");
static const int ping_chan;

void icmp_input(struct netbuf *nb, uint32_t src_ip) {
    if (nb->len < sizeof(struct icmp_hdr)) {
        return;
    }
    struct icmp_hdr hdr;
    memcpy(&hdr, nb->data, sizeof(hdr));

    if (hdr.type == ICMP_ECHO_REQUEST) {
        struct netbuf *reply = netbuf_alloc();
        if (!reply) {
            return;
        }
        uint32_t payload_len = nb->len - sizeof(struct icmp_hdr);
        uint8_t *body = (uint8_t *)netbuf_put(reply, payload_len);
        if (!body) {
            netbuf_free(reply);
            return;
        }
        if (payload_len > 0) {
            memcpy(body, nb->data + sizeof(struct icmp_hdr), payload_len);
        }
        struct icmp_hdr *rh = (struct icmp_hdr *)netbuf_push(reply, sizeof(struct icmp_hdr));
        rh->type = ICMP_ECHO_REPLY;
        rh->code = 0;
        rh->checksum = 0;
        rh->id = hdr.id;
        rh->seq = hdr.seq;
        rh->checksum = net_htons(net_checksum(reply->data, reply->len, 0));
        ip_output(reply, src_ip, IP_PROTO_ICMP);
        netbuf_free(reply);
        return;
    }
    if (hdr.type == ICMP_ECHO_REPLY) {
        uint16_t id = net_ntohs(hdr.id);
        uint16_t seq = net_ntohs(hdr.seq);
        uint32_t f = spin_lock_irqsave(&pending_lock);
        for (int i = 0; i < ICMP_MAX_PENDING; i++) {
            if (pending[i].in_use && !pending[i].replied &&
                pending[i].id == id && pending[i].seq == seq) {
                pending[i].replied = 1;
                pending[i].src_ip = src_ip;
                pending[i].reply_tick = pit_ticks();
                break;
            }
        }
        spin_unlock_irqrestore(&pending_lock, f);
        sched_wake(&ping_chan);
    }
}

int icmp_ping(uint32_t dst_ip, uint16_t id, uint16_t seq, uint32_t timeout_ticks,
              struct ping_result *result) {
    struct netbuf *nb = netbuf_alloc();
    int slot = -1;

    result->replied = 0;
    if (!nb) {
        return -1;
    }

    uint32_t f = spin_lock_irqsave(&pending_lock);
    for (int i = 0; i < ICMP_MAX_PENDING; i++) {
        if (!pending[i].in_use) {
            slot = i;
            pending[i].in_use = 1;
            pending[i].id = id;
            pending[i].seq = seq;
            pending[i].replied = 0;
            pending[i].sent_tick = pit_ticks();
            break;
        }
    }
    spin_unlock_irqrestore(&pending_lock, f);
    if (slot < 0) {
        netbuf_free(nb);
        return -1;
    }

    uint8_t *payload = (uint8_t *)netbuf_put(nb, ICMP_PAYLOAD_LEN);
    for (int i = 0; i < ICMP_PAYLOAD_LEN; i++) {
        payload[i] = (uint8_t)('a' + (i % 23));
    }
    struct icmp_hdr *hdr = (struct icmp_hdr *)netbuf_push(nb, sizeof(struct icmp_hdr));
    hdr->type = ICMP_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = net_htons(id);
    hdr->seq = net_htons(seq);
    hdr->checksum = net_htons(net_checksum(nb->data, nb->len, 0));

    int sent = ip_output(nb, dst_ip, IP_PROTO_ICMP) == 0;
    netbuf_free(nb);

    if (sent) {
        uint32_t deadline = pit_ticks() + timeout_ticks;
        while ((int32_t)(pit_ticks() - deadline) < 0) {
            if (pending[slot].replied) {
                break;
            }
            sched_wait_on_timeout(&ping_chan, "ping-wait", 20);
        }
    }

    f = spin_lock_irqsave(&pending_lock);
    if (pending[slot].replied) {
        result->replied = 1;
        result->src_ip = pending[slot].src_ip;
        result->rtt_ticks = pending[slot].reply_tick - pending[slot].sent_tick;
    }
    pending[slot].in_use = 0;
    spin_unlock_irqrestore(&pending_lock, f);

    return sent ? 0 : -1;
}
