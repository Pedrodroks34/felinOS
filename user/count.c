#include "usys.h"

int main(int argc, char **argv) {
    int n = argc > 1 ? (int)parse_uint(argv[1]) : 5;
    int delay = argc > 2 ? (int)parse_uint(argv[2]) : 500;

    for (int i = 1; i <= n; i++) {
        puts("[count "); putnum(getpid()); puts("] ");
        putnum(i); puts("/"); putnum(n); puts("\n");
        sleep_ms(delay);
    }
    return 0;
}
