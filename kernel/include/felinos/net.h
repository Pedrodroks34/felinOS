#ifndef FELINOS_NET_H
#define FELINOS_NET_H

#include <stdint.h>
#include <stddef.h>

/* Network byte order conversion */
static inline uint16_t net_htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint16_t net_ntohs(uint16_t v) {
    return net_htons(v);
}

static inline uint32_t net_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static inline uint32_t net_ntohl(uint32_t v) {
    return net_htonl(v);
}

#define MAC_LEN 6

typedef uint8_t mac_addr_t[MAC_LEN];

extern const mac_addr_t mac_broadcast;
extern const mac_addr_t mac_zero;

int mac_is_zero(const uint8_t *m);
int mac_is_broadcast(const uint8_t *m);
int mac_equal(const uint8_t *a, const uint8_t *b);
void mac_copy(uint8_t *dst, const uint8_t *src);
void mac_to_str(const uint8_t *m, char *out);

void ip4_to_str(uint32_t ip_host, char *out);
int ip4_from_str(const char *s, uint32_t *out_host);

uint16_t net_checksum(const void *data, uint32_t len, uint32_t seed);
uint16_t net_checksum2(const void *a, uint32_t alen, const void *b, uint32_t blen, uint32_t seed);

/* Socket address structures (from kernel/net/socket.h) */
#define AF_UNSPEC     0
#define AF_UNIX       1
#define AF_INET       2
#define AF_INET6      10
#define AF_NETLINK    16

#define PF_UNSPEC     AF_UNSPEC
#define PF_UNIX       AF_UNIX
#define PF_INET       AF_INET
#define PF_INET6      AF_INET6
#define PF_NETLINK    AF_NETLINK

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_RDM       4
#define SOCK_SEQPACKET 5

#define SOCK_CLOEXEC   02000000
#define SOCK_NONBLOCK  00004000

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_storage {
    uint16_t ss_family;
    uint8_t  __ss_pad1[6];
    unsigned long __ss_align;
    char     __ss_padding[112];
};

#define sockaddr_storage_align 16

#define SOL_SOCKET    1

#define SO_DEBUG       1
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_DONTROUTE   5
#define SO_BROADCAST   6
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_SNDLOWAT    9
#define SO_RCVLOWAT    10
#define SO_SNDTIMEO    11
#define SO_RCVTIMEO    12
#define SO_ACCEPTCONN  13
#define SO_LINGER      14
#define SO_REUSEPORT   15

#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

#define MSG_OOB        0x01
#define MSG_PEEK       0x02
#define MSG_DONTROUTE  0x04
#define MSG_CTRUNC     0x08
#define MSG_PROXY      0x10
#define MSG_TRUNC      0x20
#define MSG_DONTWAIT   0x40
#define MSG_EOR        0x80
#define MSG_WAITALL    0x100
#define MSG_FIN        0x200
#define MSG_SYN        0x400
#define MSG_CONFIRM    0x800
#define MSG_RST        0x1000
#define MSG_ERRQUEUE   0x2000
#define MSG_NOSIGNAL   0x4000
#define MSG_MORE       0x8000
#define MSG_WAITFORONE 0x10000
#define MSG_BATCH      0x40000
#define MSG_ZEROCOPY   0x4000000
#define MSG_FASTOPEN   0x20000000
#define MSG_CMSG_CLOEXEC 0x40000000

#endif