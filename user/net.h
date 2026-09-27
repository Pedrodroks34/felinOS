#ifndef USER_NET_H
#define USER_NET_H

#include <stdint.h>
#include "syscall.h"

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2

#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define INADDR_ANY   0x00000000u

#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_ERROR     4

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

struct hostent {
    char *h_name;
    struct in_addr h_addr;
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr_in *addr);
int connect(int fd, const struct sockaddr_in *addr);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr_in *addr);
int send(int fd, const void *buf, int len, int flags);
int recv(int fd, void *buf, int len, int flags);
int sendto(int fd, const void *buf, int len, int flags, const struct sockaddr_in *addr);
int recvfrom(int fd, void *buf, int len, int flags, struct sockaddr_in *addr);
int getsockname(int fd, struct sockaddr_in *addr);
int setsockopt(int fd, int level, int optname, const void *val, int optlen);
void closesocket(int fd);

uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
uint32_t htonl(uint32_t v);
uint32_t ntohl(uint32_t v);

uint32_t inet_addr(const char *s);
const char *inet_ntoa(uint32_t addr);
int inet_pton(int af, const char *s, void *dst);

int gethostbyname(const char *name, struct in_addr *out);
struct hostent *gethostbyname_r(const char *name, struct hostent *buf, struct in_addr *store);

int net_getifinfo(struct k_ifinfo *out);

/* Fills addr with a sockaddr_in for ip/port, both given in host byte order. */
void sockaddr_in_set(struct sockaddr_in *addr, uint32_t ip, uint16_t port);
void sockaddr_in_print(const struct sockaddr_in *addr, char *buf, int size);

#endif
