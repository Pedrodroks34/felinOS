/* wget - a minimal HTTP/1.0 client for FelinOS. Fetches a URL over the Gato
 * TCP stack and writes the body to a file or to stdout. */
#include "net.h"
#include "usys.h"
#include "stdio.h"
#include "string.h"
#include "malloc.h"

#define HTTP_PORT 80
#define HDRBUF    1024
#define BUFSZ     1024

/* strncmp() on the prefix, so a URL scheme can be recognised without a
 * dedicated helper in the libc. */
static int has_prefix(const char *s, const char *prefix) {
    int i = 0;
    while (prefix[i] && s[i] && s[i] == prefix[i]) {
        i++;
    }
    return prefix[i] == 0;
}

static char *url_host(const char *url, uint16_t *port) {
    static char host[256];
    const char *p = url;
    int i = 0;

    *port = HTTP_PORT;
    if (has_prefix(p, "http://")) {
        p += 7;
    } else if (has_prefix(p, "https://")) {
        printf("wget: https is not supported, use http\n");
        return 0;
    }
    while (p[i] && p[i] != '/' && p[i] != ':' && i < (int)sizeof(host) - 1) {
        host[i] = p[i];
        i++;
    }
    host[i] = 0;
    if (!i) {
        return 0;
    }
    if (p[i] == ':') {
        uint32_t v = 0;
        i++;
        while (p[i] >= '0' && p[i] <= '9') {
            v = v * 10 + (uint32_t)(p[i] - '0');
            i++;
        }
        *port = (uint16_t)v;
    }
    return host;
}

static const char *url_path(const char *url) {
    const char *p = url;
    if (has_prefix(p, "http://")) p += 7;
    while (*p && *p != '/') p++;
    return *p ? p : "/";
}

/* Reads until the end of the header block, keeping whatever follows it. */
static int read_headers(int fd, char *buf, int size, char *body, int *body_len) {
    int used = 0;
    *body_len = 0;

    for (;;) {
        int n = recv(fd, buf + used, (uint32_t)(size - used), 0);
        if (n <= 0) {
            return -1;
        }
        used += n;
        for (int i = 0; i + 3 < used; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                int rest = used - (i + 4);
                if (rest > 0) {
                    memcpy(body, buf + i + 4, (size_t)rest);
                    *body_len = rest;
                }
                buf[i] = 0;
                return i;
            }
        }
        if (used >= size) {
            return -1;
        }
    }
}

static int status_of(const char *hdr) {
    if (hdr[0] != 'H' || hdr[1] != 'T') {
        return 0;
    }
    const char *sp = hdr;
    while (*sp && *sp != ' ') sp++;
    while (*sp == ' ') sp++;
    int code = 0;
    while (*sp >= '0' && *sp <= '9') {
        code = code * 10 + (*sp - '0');
        sp++;
    }
    return code;
}

static void usage(const char *name) {
    printf("usage: %s [-O file] <url>\n", name);
    printf("  -O <file>   write the body to <file> (default: stdout)\n");
}

int main(int argc, char **argv) {
    const char *url = 0;
    const char *outfile = 0;

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "-O") && i + 1 < argc) {
            outfile = argv[++i];
        } else if (streq(argv[i], "-h") || streq(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!url) {
            url = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!url) {
        usage(argv[0]);
        return 1;
    }

    uint16_t port;
    const char *host = url_host(url, &port);
    if (!host) {
        return 1;
    }
    const char *path = url_path(url);

    struct in_addr in;
    uint32_t ip;
    if (gethostbyname(host, &in)) {
        ip = ntohl(in.s_addr);
        printf("wget: %s -> %s\n", host, inet_ntoa(in.s_addr));
    } else {
        ip = inet_addr(host);
        if (!ip) {
            printf("wget: cannot resolve %s\n", host);
            return 1;
        }
        printf("wget: %s -> %s\n", host, inet_ntoa(htonl(ip)));
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("wget: socket failed\n");
        return 1;
    }
    struct sockaddr_in srv;
    sockaddr_in_set(&srv, ip, port);
    if (connect(fd, &srv) < 0) {
        printf("wget: connect to port %d failed\n", port);
        return 1;
    }

    char req[512];
    int n = sprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FelinOS-wget/0.3\r\n"
                          "Connection: close\r\n\r\n", path, host);
    if (send(fd, req, n, 0) != n) {
        printf("wget: request failed\n");
        return 1;
    }

    char *hdr = (char *)malloc(HDRBUF);
    char *body = (char *)malloc(BUFSZ);
    if (!hdr || !body) {
        printf("wget: out of memory\n");
        return 1;
    }
    int body_len = 0;
    int hlen = read_headers(fd, hdr, HDRBUF - 1, body, &body_len);
    if (hlen < 0) {
        printf("wget: no response\n");
        return 1;
    }
    int code = status_of(hdr);
    printf("wget: HTTP %d, %d bytes of headers\n", code, hlen);
    if (code / 100 != 2) {
        printf("wget: request failed\n");
        return 1;
    }

    int out = 1;
    if (outfile) {
        out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) {
            printf("wget: cannot open %s\n", outfile);
            return 1;
        }
        printf("wget: saving to %s\n", outfile);
    }

    if (body_len) {
        write(out, body, body_len);
    }
    for (;;) {
        int r = recv(fd, body, BUFSZ, 0);
        if (r <= 0) {
            break;
        }
        write(out, body, r);
    }
    close(fd);
    if (out != 1) {
        close(out);
    }
    printf("wget: done\n");
    return 0;
}
