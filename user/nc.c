/* nc - a small netcat for FelinOS. Listens or connects over TCP, over UDP
 * with -u, and either pipes stdin/stdout or sends one line and prints the
 * answer. Useful for poking at the Gato TCP stack by hand. */
#include "net.h"
#include "usys.h"
#include "stdio.h"
#include "string.h"

#define BUFSZ 1024

static int udp_mode;
static int listen_mode;
static int verbose;
static int once_mode;
static uint16_t port = 0;

static void usage(const char *name) {
    printf("usage: %s [options] [host] [port]\n", name);
    printf("  -l           listen instead of connect\n");
    printf("  -u           use UDP instead of TCP\n");
    printf("  -1           send one line, print the reply and exit\n");
    printf("  -v           print what is happening\n");
    printf("  -p <port>    local port to bind (connect mode)\n");
}

static int parse_arg(const char *a) {
    return (int)parse_uint(a);
}

static void show_peer(const char *what, const struct sockaddr_in *a) {
    char b[32];
    sockaddr_in_print(a, b, sizeof(b));
    printf("%s %s\n", what, b);
}

static int resolve(const char *host, struct sockaddr_in *out) {
    struct in_addr in;

    if (gethostbyname(host, &in)) {
        sockaddr_in_set(out, ntohl(in.s_addr), port);
        return 0;
    }
    uint32_t ip = inet_addr(host);
    if (!ip) {
        printf("nc: cannot resolve %s\n", host);
        return -1;
    }
    sockaddr_in_set(out, ip, port);
    return 0;
}

/* Copies stdin into the socket and the socket into stdout until one side
 * closes. Runs in the foreground, so ^C from the shell stops it. */
static void pump(int fd) {
    char sbuf[BUFSZ];
    char rbuf[BUFSZ];

    for (;;) {
        int n = read(0, sbuf, sizeof(sbuf));
        if (n <= 0) {
            if (verbose) printf("nc: stdin closed\n");
            break;
        }
        int off = 0;
        while (off < n) {
            int w = send(fd, sbuf + off, n - off, 0);
            if (w <= 0) {
                return;
            }
            off += w;
        }
        int r = recv(fd, rbuf, sizeof(rbuf), 0);
        if (r <= 0) {
            if (verbose) printf("nc: peer closed\n");
            break;
        }
        write(1, rbuf, r);
    }
}

static int run_client(const char *host) {
    struct sockaddr_in srv;
    if (resolve(host, &srv) < 0) {
        return 1;
    }
    int type = udp_mode ? SOCK_DGRAM : SOCK_STREAM;
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) {
        printf("nc: socket failed\n");
        return 1;
    }
    if (verbose) show_peer("connecting to", &srv);
    if (connect(fd, &srv) < 0) {
        printf("nc: connect failed\n");
        return 1;
    }
    if (verbose) printf("nc: connected\n");

    if (once_mode) {
        char line[BUFSZ];
        int n = read(0, line, sizeof(line) - 1);
        if (n > 0) {
            if (line[n - 1] != '\n') {
                line[n++] = '\n';
            }
            send(fd, line, n, 0);
            char rbuf[BUFSZ];
            int r = recv(fd, rbuf, sizeof(rbuf) - 1, 0);
            if (r > 0) {
                rbuf[r] = 0;
                printf("%s", rbuf);
            }
        }
        close(fd);
        return 0;
    }
    pump(fd);
    close(fd);
    return 0;
}

static int run_server(void) {
    int type = udp_mode ? SOCK_DGRAM : SOCK_STREAM;
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) {
        printf("nc: socket failed\n");
        return 1;
    }
    struct sockaddr_in me;
    sockaddr_in_set(&me, INADDR_ANY, port);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, &me) < 0) {
        printf("nc: bind failed\n");
        return 1;
    }
    if (!udp_mode && listen(fd, 1) < 0) {
        printf("nc: listen failed\n");
        return 1;
    }
    if (verbose) printf("nc: listening on port %d (%s)\n", port, udp_mode ? "udp" : "tcp");

    int peer = fd;
    struct sockaddr_in from;
    if (!udp_mode) {
        peer = accept(fd, &from);
        if (peer < 0) {
            printf("nc: accept failed\n");
            return 1;
        }
        if (verbose) show_peer("connection from", &from);
    }
    pump(peer);
    close(peer);
    if (peer != fd) {
        close(fd);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *host = 0;
    int positional = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && positional == 0) {
            if (streq(a, "-l")) listen_mode = 1;
            else if (streq(a, "-u")) udp_mode = 1;
            else if (streq(a, "-1")) once_mode = 1;
            else if (streq(a, "-v")) verbose = 1;
            else if (streq(a, "-p") && i + 1 < argc) port = (uint16_t)parse_arg(argv[++i]);
            else { usage(argv[0]); return 1; }
        } else if (!host) {
            host = a;
        } else if (!port) {
            port = (uint16_t)parse_arg(a);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (listen_mode) {
        if (!port) port = 8080;
        return run_server();
    }
    if (!host || !port) {
        usage(argv[0]);
        return 1;
    }
    return run_client(host);
}
