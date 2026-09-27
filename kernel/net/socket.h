#ifndef FELINOS_SOCKET_H
#define FELINOS_SOCKET_H

#include <stdint.h>
#include "fs/vfs.h"

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

void socket_init(void);
struct vfs_file *socket_create(int domain, int type, int protocol);
int socket_bind(struct vfs_file *f, const struct sockaddr_in *addr);
int socket_connect(struct vfs_file *f, const struct sockaddr_in *addr);
int socket_listen(struct vfs_file *f, int backlog);
struct vfs_file *socket_accept(struct vfs_file *f, struct sockaddr_in *addr_out);
int socket_is_socket(const struct vfs_file *f);

/* Data transfer. timeout_ms of 0xFFFFFFFF blocks until a task is killable,
 * any other value bounds the wait in milliseconds. */
int socket_send(struct vfs_file *f, const void *buf, uint32_t len, int flags);
int socket_recv(struct vfs_file *f, void *buf, uint32_t len, int flags);
int socket_sendto(struct vfs_file *f, const void *buf, uint32_t len, int flags,
                  const struct sockaddr_in *addr);
int socket_recvfrom(struct vfs_file *f, void *buf, uint32_t len, int flags,
                    struct sockaddr_in *addr_out);
int socket_type(const struct vfs_file *f);
uint16_t socket_local_port(const struct vfs_file *f);

#endif
