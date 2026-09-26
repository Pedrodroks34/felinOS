#include "usys.h"

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "kmem";
    puts("crash: trying "); puts(m); puts("\n");
    if (streq(m, "cli")) {
        __asm__ volatile ("cli");
    } else if (streq(m, "null")) {
        *(volatile int *)0 = 1;
    } else {
        *(volatile int *)0x100000 = 1;
    }
    puts("crash: NOT REACHED\n");
    return 0;
}
