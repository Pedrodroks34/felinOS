#include "usys.h"

int main(int argc, char **argv) {
    char buf[256];
    int n;
    if (argc < 2) {
        while ((n = read(0, buf, sizeof(buf))) > 0) write(1, buf, n);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { puts("cat: cannot open "); puts(argv[i]); puts("\n"); return 1; }
        while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, n);
        close(fd);
    }
    return 0;
}
