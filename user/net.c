#include "net.h"
#include "usys.h"
#include "string.h"
#include "stdio.h"

int socket(int domain, int type, int protocol) {
    return sys3(SYS_SOCKET, domain, type, protocol);
}

int bind(int fd, const struct sockaddr_in *addr) {
    return sys3(SYS_BIND, fd, (sysint)addr, sizeof(struct sockaddr_in));
}

int connect(int fd, const struct sockaddr_in *addr) {
    return sys3(SYS_CONNECT, fd, (sysint)addr, sizeof(struct sockaddr_in));
}

int listen(int fd, int backlog) {
    return sys3(SYS_LISTEN, fd, backlog, 0);
}

int accept(int fd, struct sockaddr_in *addr) {
    return sys3(SYS_ACCEPT, fd, (sysint)addr, 0);
}

int send(int fd, const void *buf, int len, int flags) {
    return sys4(SYS_SEND, fd, (sysint)buf, len, flags);
}

int recv(int fd, void *buf, int len, int flags) {
    return sys4(SYS_RECV, fd, (sysint)buf, len, flags);
}

int sendto(int fd, const void *buf, int len, int flags, const struct sockaddr_in *addr) {
    return sys5(SYS_SENDTO, fd, (sysint)buf, len, flags, (sysint)addr);
}

int recvfrom(int fd, void *buf, int len, int flags, struct sockaddr_in *addr) {
    return sys5(SYS_RECVFROM, fd, (sysint)buf, len, flags, (sysint)addr);
}

int getsockname(int fd, struct sockaddr_in *addr) {
    return sys3(SYS_GETSOCKNAME, fd, (sysint)addr, 0);
}

int setsockopt(int fd, int level, int optname, const void *val, int optlen) {
    return sys5(SYS_SETSOCKOPT, fd, level, optname, (sysint)val, optlen);
}

void closesocket(int fd) {
    close(fd);
}

uint16_t htons(uint16_t v) {
    return (uint16_t)(((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu));
}

uint16_t ntohs(uint16_t v) {
    return htons(v);
}

uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

int inet_pton(int af, const char *s, void *dst) {
    if (af != AF_INET || !s || !dst) {
        return 0;
    }
    uint32_t parts[4];
    int idx = 0;
    uint32_t acc = 0;
    int digits = 0;

    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            if (digits == 3) {
                return 0;
            }
            acc = acc * 10 + (uint32_t)(*s - '0');
            digits++;
        } else if (*s == '.' || *s == 0) {
            if (digits == 0 || idx > 3) {
                return 0;
            }
            if (acc > 255) {
                return 0;
            }
            parts[idx++] = acc;
            acc = 0;
            digits = 0;
            if (*s == 0) {
                break;
            }
        } else {
            return 0;
        }
    }
    if (idx != 4) {
        return 0;
    }
    *(uint32_t *)dst = htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
    return 1;
}

static char ntoa_buf[4][16];
static int ntoa_slot;

uint32_t inet_addr(const char *s) {
    uint32_t v;
    if (!inet_pton(AF_INET, s, &v)) {
        return 0;
    }
    return ntohl(v);
}

const char *inet_ntoa(uint32_t addr) {
    char *out = ntoa_buf[ntoa_slot];
    ntoa_slot = (ntoa_slot + 1) & 3;

    addr = ntohl(addr);
    int i = 0;
    int shift;
    for (shift = 24; shift >= 0; shift -= 8) {
        int octet = (int)((addr >> shift) & 0xFF);
        if (i) {
            out[i++] = '.';
        }
        if (octet >= 100) out[i++] = (char)('0' + octet / 100);
        if (octet >= 10)  out[i++] = (char)('0' + octet / 10 % 10);
        out[i++] = (char)('0' + octet % 10);
    }
    out[i] = 0;
    return out;
}

int gethostbyname(const char *name, struct in_addr *out) {
    uint32_t ip = 0;
    if (!name || sys3(SYS_GETHOSTBYNAME, (sysint)name, (sysint)&ip, 0) != 0) {
        return 0;
    }
    if (out) {
        out->s_addr = ip;
    }
    return 1;
}

struct hostent *gethostbyname_r(const char *name, struct hostent *buf, struct in_addr *store) {
    if (!name || !buf || !store || !gethostbyname(name, store)) {
        return 0;
    }
    buf->h_name = (char *)name;
    buf->h_addr = *store;
    return buf;
}

int net_getifinfo(struct k_ifinfo *out) {
    return sys3(SYS_GETIFADDR, (sysint)out, 0, 0);
}

void sockaddr_in_set(struct sockaddr_in *addr, uint32_t ip, uint16_t port) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = htonl(ip);
}

void sockaddr_in_print(const struct sockaddr_in *addr, char *buf, int size) {
    const char *ip = inet_ntoa(addr->sin_addr.s_addr);
    int i = 0;
    while (ip[i] && i < size - 6) {
        buf[i] = ip[i];
        i++;
    }
    buf[i++] = ':';
    int port = ntohs(addr->sin_port);
    if (port >= 10000) buf[i++] = (char)('0' + port / 10000 % 10);
    if (port >= 1000)  buf[i++] = (char)('0' + port / 1000 % 10);
    if (port >= 100)   buf[i++] = (char)('0' + port / 100 % 10);
    if (port >= 10)    buf[i++] = (char)('0' + port / 10 % 10);
    buf[i++] = (char)('0' + port % 10);
    buf[i] = 0;
}
