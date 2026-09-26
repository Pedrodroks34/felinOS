#include "net/dns.h"
#include "net/udp.h"
#include "net/net.h"
#include "lib/string.h"
#include "drivers/pit.h"

#define DNS_PORT 53
#define DNS_BUF_SIZE 512

struct dns_hdr {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

static uint32_t encode_name(uint8_t *buf, uint32_t off, const char *hostname) {
    const char *label = hostname;

    while (*label) {
        const char *dot = label;
        while (*dot && *dot != '.') {
            dot++;
        }
        uint32_t label_len = (uint32_t)(dot - label);
        if (label_len == 0 || label_len > 63) {
            return 0;
        }
        buf[off++] = (uint8_t)label_len;
        memcpy(buf + off, label, label_len);
        off += label_len;
        label = (*dot == '.') ? dot + 1 : dot;
    }
    buf[off++] = 0;
    return off;
}

static uint32_t skip_name(const uint8_t *buf, uint32_t len, uint32_t pos) {
    while (pos < len) {
        uint8_t b = buf[pos];
        if ((b & 0xC0) == 0xC0) {
            return pos + 2;
        }
        if (b == 0) {
            return pos + 1;
        }
        pos += (uint32_t)b + 1;
    }
    return pos;
}

int dns_resolve(uint32_t dns_server, const char *hostname, uint32_t timeout_ticks, uint32_t *out_ip) {
    uint8_t buf[DNS_BUF_SIZE];
    uint8_t rxbuf[DNS_BUF_SIZE];

    if (dns_server == 0) {
        return -1;
    }
    struct udp_pcb *pcb = udp_open();
    if (!pcb) {
        return -1;
    }
    udp_bind(pcb, 0);

    uint16_t qid = (uint16_t)pit_ticks();
    struct dns_hdr *hdr = (struct dns_hdr *)buf;
    hdr->id = net_htons(qid);
    hdr->flags = net_htons(0x0100);
    hdr->qdcount = net_htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    uint32_t off = sizeof(struct dns_hdr);
    off = encode_name(buf, off, hostname);
    if (off == 0) {
        udp_close(pcb);
        return -1;
    }
    buf[off++] = 0; buf[off++] = 1;
    buf[off++] = 0; buf[off++] = 1;

    uint32_t deadline = pit_ticks() + timeout_ticks;
    int found = 0;
    uint32_t result_ip = 0;

    for (int attempt = 0; attempt < 3 && !found && (int32_t)(pit_ticks() - deadline) < 0; attempt++) {
        udp_sendto(pcb, dns_server, DNS_PORT, buf, off);

        uint32_t src_ip;
        uint16_t src_port;
        int n = udp_recvfrom(pcb, rxbuf, sizeof(rxbuf), &src_ip, &src_port, 150);
        if (n <= (int)sizeof(struct dns_hdr)) {
            continue;
        }
        struct dns_hdr rh;
        memcpy(&rh, rxbuf, sizeof(rh));
        if (net_ntohs(rh.id) != qid || !(net_ntohs(rh.flags) & 0x8000)) {
            continue;
        }
        uint16_t qdcount = net_ntohs(rh.qdcount);
        uint16_t ancount = net_ntohs(rh.ancount);
        uint32_t pos = sizeof(struct dns_hdr);

        for (int i = 0; i < qdcount; i++) {
            pos = skip_name(rxbuf, (uint32_t)n, pos);
            pos += 4;
        }
        for (int i = 0; i < ancount && pos < (uint32_t)n; i++) {
            pos = skip_name(rxbuf, (uint32_t)n, pos);
            if (pos + 10 > (uint32_t)n) {
                break;
            }
            uint16_t rtype, rclass, rdlen;
            memcpy(&rtype, rxbuf + pos, 2); rtype = net_ntohs(rtype); pos += 2;
            memcpy(&rclass, rxbuf + pos, 2); rclass = net_ntohs(rclass); pos += 2;
            pos += 4;
            memcpy(&rdlen, rxbuf + pos, 2); rdlen = net_ntohs(rdlen); pos += 2;
            if (pos + rdlen > (uint32_t)n) {
                break;
            }
            if (rtype == 1 && rclass == 1 && rdlen == 4) {
                result_ip = ((uint32_t)rxbuf[pos] << 24) | ((uint32_t)rxbuf[pos + 1] << 16) |
                            ((uint32_t)rxbuf[pos + 2] << 8) | (uint32_t)rxbuf[pos + 3];
                found = 1;
                break;
            }
            pos += rdlen;
        }
    }

    udp_close(pcb);
    if (!found) {
        return -1;
    }
    *out_ip = result_ip;
    return 0;
}
