#include "net/socket.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "net/net.h"
#include "lib/heap.h"
#include "lib/string.h"

#define SOCK_TIMEOUT_INFINITE 0xFFFFFFFFu

struct socket {
    int domain;
    int type;
    struct udp_pcb *udp;
    struct tcp_pcb *tcp;
};

static int sockfs_read(struct vfs_file *f, void *buf, uint32_t len);
static int sockfs_write(struct vfs_file *f, const void *buf, uint32_t len);
static void sockfs_close(struct vfs_file *f);

static const struct fs_ops sockfs_ops = {
    .name = "sockfs",
    .read = sockfs_read,
    .write = sockfs_write,
    .close = sockfs_close,
};

static struct vfs_mount sockfs_mount;

void socket_init(void) {
    memset(&sockfs_mount, 0, sizeof(sockfs_mount));
    sockfs_mount.used = 1;
    sockfs_mount.ops = &sockfs_ops;
}

int socket_is_socket(const struct vfs_file *f) {
    return f && f->mnt == &sockfs_mount;
}

struct vfs_file *socket_create(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM)) {
        return NULL;
    }
    struct socket *sock = (struct socket *)kmalloc(sizeof(struct socket));
    if (!sock) {
        return NULL;
    }
    memset(sock, 0, sizeof(*sock));
    sock->domain = domain;
    sock->type = type;

    if (type == SOCK_DGRAM) {
        sock->udp = udp_open();
        if (!sock->udp) {
            kfree(sock);
            return NULL;
        }
    } else {
        sock->tcp = tcp_open();
        if (!sock->tcp) {
            kfree(sock);
            return NULL;
        }
    }

    struct vfs_file *f = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
    if (!f) {
        if (sock->udp) {
            udp_close(sock->udp);
        }
        if (sock->tcp) {
            tcp_release(sock->tcp);
        }
        kfree(sock);
        return NULL;
    }
    f->mnt = &sockfs_mount;
    f->priv = sock;
    f->pos = 0;
    f->flags = VFS_O_READ | VFS_O_WRITE;
    f->refs = 1;
    return f;
}

int socket_bind(struct vfs_file *f, const struct sockaddr_in *addr) {
    struct socket *sock = (struct socket *)f->priv;
    uint16_t port = net_ntohs(addr->sin_port);

    if (sock->udp) {
        return udp_bind(sock->udp, port);
    }
    return tcp_bind(sock->tcp, port);
}

int socket_connect(struct vfs_file *f, const struct sockaddr_in *addr) {
    struct socket *sock = (struct socket *)f->priv;
    uint32_t ip = net_ntohl(addr->sin_addr.s_addr);
    uint16_t port = net_ntohs(addr->sin_port);

    if (sock->udp) {
        return udp_connect(sock->udp, ip, port);
    }
    return tcp_connect(sock->tcp, ip, port, 500);
}

int socket_listen(struct vfs_file *f, int backlog) {
    struct socket *sock = (struct socket *)f->priv;

    if (!sock->tcp) {
        return -1;
    }
    return tcp_listen(sock->tcp, backlog);
}

struct vfs_file *socket_accept(struct vfs_file *f, struct sockaddr_in *addr_out) {
    struct socket *sock = (struct socket *)f->priv;

    if (!sock->tcp) {
        return NULL;
    }
    uint32_t remote_ip = 0;
    uint16_t remote_port = 0;
    struct tcp_pcb *child = tcp_accept(sock->tcp, SOCK_TIMEOUT_INFINITE, &remote_ip, &remote_port);
    if (!child) {
        return NULL;
    }
    struct socket *csock = (struct socket *)kmalloc(sizeof(struct socket));
    if (!csock) {
        tcp_release(child);
        return NULL;
    }
    memset(csock, 0, sizeof(*csock));
    csock->domain = AF_INET;
    csock->type = SOCK_STREAM;
    csock->tcp = child;

    struct vfs_file *cf = (struct vfs_file *)kmalloc(sizeof(struct vfs_file));
    if (!cf) {
        tcp_release(child);
        kfree(csock);
        return NULL;
    }
    cf->mnt = &sockfs_mount;
    cf->priv = csock;
    cf->pos = 0;
    cf->flags = VFS_O_READ | VFS_O_WRITE;
    cf->refs = 1;

    if (addr_out) {
        memset(addr_out, 0, sizeof(*addr_out));
        addr_out->sin_family = AF_INET;
        addr_out->sin_port = net_htons(remote_port);
        addr_out->sin_addr.s_addr = net_htonl(remote_ip);
    }
    return cf;
}

static int sockfs_read(struct vfs_file *f, void *buf, uint32_t len) {
    struct socket *sock = (struct socket *)f->priv;

    if (sock->udp) {
        return udp_recvfrom(sock->udp, buf, len, NULL, NULL, SOCK_TIMEOUT_INFINITE);
    }
    return tcp_recv(sock->tcp, buf, len, SOCK_TIMEOUT_INFINITE);
}

static int sockfs_write(struct vfs_file *f, const void *buf, uint32_t len) {
    struct socket *sock = (struct socket *)f->priv;

    if (sock->udp) {
        uint32_t ip;
        uint16_t port;
        if (!udp_remote(sock->udp, &ip, &port)) {
            return -1;
        }
        return udp_sendto(sock->udp, ip, port, buf, len) == 0 ? (int)len : -1;
    }
    return tcp_send(sock->tcp, buf, len);
}

static void sockfs_close(struct vfs_file *f) {
    struct socket *sock = (struct socket *)f->priv;

    if (sock->udp) {
        udp_close(sock->udp);
    }
    if (sock->tcp) {
        tcp_close(sock->tcp);
        tcp_release(sock->tcp);
    }
    kfree(sock);
}
