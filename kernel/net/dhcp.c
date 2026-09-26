#include "net/dhcp.h"
#include "net/udp.h"
#include "net/net.h"
#include "net/netif.h"
#include "lib/string.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"

#define DHCP_MAGIC 0x63825363u
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_BUF_SIZE 576

#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY   2

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

struct dhcp_hdr {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
} __attribute__((packed));

static uint32_t build_base(uint8_t *buf, uint32_t xid, uint32_t ciaddr) {
    struct dhcp_hdr *hdr = (struct dhcp_hdr *)buf;
    struct netif *nif = netif_default();

    memset(hdr, 0, sizeof(*hdr));
    hdr->op = DHCP_OP_REQUEST;
    hdr->htype = 1;
    hdr->hlen = 6;
    hdr->xid = net_htonl(xid);
    hdr->flags = net_htons(0x8000);
    hdr->ciaddr = net_htonl(ciaddr);
    memcpy(hdr->chaddr, nif->mac, 6);
    hdr->magic = net_htonl(DHCP_MAGIC);
    return sizeof(struct dhcp_hdr);
}

static uint32_t append_discover_options(uint8_t *buf, uint32_t off) {
    buf[off++] = 53; buf[off++] = 1; buf[off++] = DHCP_MSG_DISCOVER;
    buf[off++] = 55; buf[off++] = 4; buf[off++] = 1; buf[off++] = 3; buf[off++] = 6; buf[off++] = 51;
    buf[off++] = 255;
    return off;
}

static uint32_t append_request_options(uint8_t *buf, uint32_t off, uint32_t req_ip, uint32_t server_id) {
    buf[off++] = 53; buf[off++] = 1; buf[off++] = DHCP_MSG_REQUEST;
    buf[off++] = 50; buf[off++] = 4;
    memcpy(buf + off, &req_ip, 4); off += 4;
    buf[off++] = 54; buf[off++] = 4;
    memcpy(buf + off, &server_id, 4); off += 4;
    buf[off++] = 255;
    return off;
}

static int parse_reply(const uint8_t *buf, uint32_t len, uint32_t xid, int *msg_type,
                        struct dhcp_lease *lease) {
    if (len < sizeof(struct dhcp_hdr)) {
        return -1;
    }
    struct dhcp_hdr hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (net_ntohl(hdr.magic) != DHCP_MAGIC || net_ntohl(hdr.xid) != xid || hdr.op != DHCP_OP_REPLY) {
        return -1;
    }
    lease->ip = net_ntohl(hdr.yiaddr);
    *msg_type = -1;

    uint32_t off = sizeof(struct dhcp_hdr);
    while (off < len) {
        uint8_t code = buf[off++];
        if (code == 255) {
            break;
        }
        if (code == 0) {
            continue;
        }
        if (off >= len) {
            break;
        }
        uint8_t olen = buf[off++];
        if (off + olen > len) {
            break;
        }
        switch (code) {
        case 53:
            if (olen >= 1) {
                *msg_type = buf[off];
            }
            break;
        case 1:
            if (olen >= 4) {
                uint32_t v; memcpy(&v, buf + off, 4); lease->netmask = net_ntohl(v);
            }
            break;
        case 3:
            if (olen >= 4) {
                uint32_t v; memcpy(&v, buf + off, 4); lease->gateway = net_ntohl(v);
            }
            break;
        case 6:
            if (olen >= 4) {
                uint32_t v; memcpy(&v, buf + off, 4); lease->dns_server = net_ntohl(v);
            }
            break;
        case 54:
            if (olen >= 4) {
                uint32_t v; memcpy(&v, buf + off, 4); lease->server_id = net_ntohl(v);
            }
            break;
        case 51:
            if (olen >= 4) {
                uint32_t v; memcpy(&v, buf + off, 4); lease->lease_seconds = net_ntohl(v);
            }
            break;
        default:
            break;
        }
        off += olen;
    }
    return 0;
}

int dhcp_client_run(uint32_t timeout_ticks, struct dhcp_lease *out) {
    struct netif *nif = netif_default();
    uint8_t buf[DHCP_BUF_SIZE];
    uint8_t rxbuf[DHCP_BUF_SIZE];

    if (!nif) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    struct udp_pcb *pcb = udp_open();
    if (!pcb) {
        return -1;
    }
    if (udp_bind(pcb, DHCP_CLIENT_PORT) != 0) {
        udp_close(pcb);
        return -1;
    }

    uint32_t xid = pit_ticks() * 2654435761u ^ rtc_unix();
    uint32_t deadline = pit_ticks() + timeout_ticks;
    int got_offer = 0;
    struct dhcp_lease offer;
    memset(&offer, 0, sizeof(offer));

    for (int attempt = 0; attempt < 4 && !got_offer && (int32_t)(pit_ticks() - deadline) < 0; attempt++) {
        uint32_t len = build_base(buf, xid, 0);
        len = append_discover_options(buf, len);
        udp_sendto(pcb, 0xFFFFFFFFu, DHCP_SERVER_PORT, buf, len);

        uint32_t src_ip;
        uint16_t src_port;
        int n = udp_recvfrom(pcb, rxbuf, sizeof(rxbuf), &src_ip, &src_port, 100);
        if (n > 0) {
            int msg_type;
            if (parse_reply(rxbuf, (uint32_t)n, xid, &msg_type, &offer) == 0 && msg_type == DHCP_MSG_OFFER) {
                got_offer = 1;
            }
        }
    }

    if (!got_offer) {
        udp_close(pcb);
        return -1;
    }

    int acked = 0;
    for (int attempt = 0; attempt < 4 && !acked && (int32_t)(pit_ticks() - deadline) < 0; attempt++) {
        uint32_t len = build_base(buf, xid, 0);
        uint32_t req_ip = net_htonl(offer.ip);
        uint32_t server_id = net_htonl(offer.server_id);
        len = append_request_options(buf, len, req_ip, server_id);
        udp_sendto(pcb, 0xFFFFFFFFu, DHCP_SERVER_PORT, buf, len);

        uint32_t src_ip;
        uint16_t src_port;
        int n = udp_recvfrom(pcb, rxbuf, sizeof(rxbuf), &src_ip, &src_port, 100);
        if (n > 0) {
            int msg_type;
            struct dhcp_lease ack = offer;
            if (parse_reply(rxbuf, (uint32_t)n, xid, &msg_type, &ack) == 0) {
                if (msg_type == DHCP_MSG_ACK) {
                    *out = ack;
                    acked = 1;
                } else if (msg_type == DHCP_MSG_NAK) {
                    break;
                }
            }
        }
    }

    udp_close(pcb);
    return acked ? 0 : -1;
}
