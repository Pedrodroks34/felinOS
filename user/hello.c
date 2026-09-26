#include "usys.h"

int main(int argc, char **argv) {
    unsigned cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    puts("Hello from user mode!\n");
    puts("  ring (cs & 3): "); putnum(cs & 3);
    puts("\n  pid: ");         putnum(getpid());
    puts("\n  uptime ms: ");   putnum(uptime());
    puts("\n  args:");
    for (int i = 0; i < argc; i++) { puts(" "); puts(argv[i]); }
    char *heap = sbrk(4096);
    puts("\n  sbrk ok: "); putnum(heap != (char *)-1);
    puts("\n  name? ");
    char buf[64];
    int n = read(0, buf, sizeof(buf) - 1);
    if (n > 0) { buf[n > 0 && buf[n - 1] == '\n' ? n - 1 : n] = 0; puts("hi, "); puts(buf); puts("!\n"); }
    sleep_ms(300);
    return 0;
}
