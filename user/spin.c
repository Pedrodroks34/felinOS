#include "usys.h"

int main(int argc, char **argv) {
    unsigned seconds = argc > 1 ? parse_uint(argv[1]) : 0;
    unsigned start = (unsigned)uptime();
    unsigned rounds = 0;
    volatile unsigned counter = 0;

    for (;;) {
        for (unsigned i = 0; i < 200000; i++) counter++;
        rounds++;
        if (seconds && (unsigned)uptime() - start >= seconds * 1000u) break;
    }
    puts("spin pid "); putnum(getpid());
    puts(": "); putnum((int)rounds);
    puts(" rounds in "); putnum((int)seconds);
    puts("s\n");
    return 0;
}
