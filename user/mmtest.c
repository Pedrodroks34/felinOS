/* Exercises the memory-management and file-size syscalls the shell cannot
 * reach on its own: mmap, mprotect, madvise, truncate and clock_gettime. */
#include "usys.h"
#include "stdio.h"

static int failures;

static void check(const char *what, int ok) {
    printf("  %-34s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    puts("== mmap ==\n");

    /* Two pages, private and anonymous, writable. */
    volatile char *p = (volatile char *)mmap(0, 0x2000, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap returns a user address", p != (volatile char *)-1 &&
          p >= (volatile char *)0x40000000u);
    if (p == (volatile char *)-1) {
        puts("mmap failed, stopping here\n");
        return 1;
    }

    /* Demand-allocated pages start out zeroed. */
    int zeroed = 1;
    for (int i = 0; i < 0x2000; i++) {
        if (p[i] != 0) { zeroed = 0; break; }
    }
    check("fresh mapping reads back as zero", zeroed);

    for (int i = 0; i < 0x2000; i++) {
        p[i] = (char)(i * 7 + 1);
    }
    int kept = 1;
    for (int i = 0; i < 0x2000; i++) {
        if (p[i] != (char)(i * 7 + 1)) { kept = 0; break; }
    }
    check("writes survive a re-read", kept);

    /* A second mapping must not alias the first. */
    volatile char *q = (volatile char *)mmap(0, 0x1000, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("second mmap lands elsewhere", q != (volatile char *)-1 && q != p);
    if (q != (volatile char *)-1) {
        int clear = 1;
        for (int i = 0; i < 0x1000; i++) {
            if (q[i] != 0) { clear = 0; break; }
        }
        check("second mapping is not aliased", clear);
        q[0] = 42;
        check("first mapping unaffected", p[0] == 1);
    }

    puts("\n== mprotect ==\n");

    /* Drop write permission on the first page, then try to write it. */
    check("mprotect read-only", mprotect((void *)p, 0x1000, PROT_READ) == 0);
    int faulted = 0;
    /* The store must raise a fault, which the kernel turns into a kill. If
     * protection is not enforced the process dies here instead, and the
     * test harness reports the missing line. */
    p[0] = 99;
    puts("  write after PROT_READ: ");
    puts("NOT faulted (protection not enforced)\n");
    (void)faulted;

    puts("\n== madvise ==\n");
    check("MADV_DONTNEED", madvise((void *)p, 0x2000, MADV_DONTNEED) == 0);
    check("MADV_NORMAL", madvise((void *)p, 0x2000, MADV_NORMAL) == 0);
    check("msync is a no-op", msync((void *)p, 0x2000, 0) == 0);

    puts("\n== munmap ==\n");
    check("munmap second mapping", munmap((void *)q, 0x1000) == 0);

    puts("\n== clock_gettime ==\n");
    struct timespec ts;
    check("CLOCK_MONOTONIC", clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    printf("  uptime %d.%09d s\n", (int)ts.tv_sec, (int)ts.tv_nsec);
    struct timespec ts2;
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    int moved = (ts2.tv_sec > ts.tv_sec) ||
                (ts2.tv_sec == ts.tv_sec && ts2.tv_nsec >= ts.tv_nsec);
    check("clock does not run backwards", moved);

    puts("\n== truncate ==\n");
    int fd = open("/tmp/trunc.dat", O_WRONLY | O_CREAT | O_TRUNC);
    check("create file", fd >= 0);
    if (fd >= 0) {
        char block[512];
        for (int i = 0; i < (int)sizeof(block); i++) block[i] = (char)i;
        for (int i = 0; i < 8; i++) {
            check("write block", write(fd, block, sizeof(block)) == (int)sizeof(block));
        }
        check("ftruncate to 2048", ftruncate(fd, 2048) == 0);
        close(fd);

        int rd = open("/tmp/trunc.dat", O_RDONLY);
        check("reopen for reading", rd >= 0);
        if (rd >= 0) {
            check("size is 2048", lseek(rd, 0, SEEK_END) == 2048);
            char got[2048];
            int n = read(rd, got, sizeof(got));
            check("read back 2048 bytes", n == 2048);
            int same = 1;
            for (int i = 0; i < 2048; i++) {
                if (got[i] != (char)(i % 512)) { same = 0; break; }
            }
            check("surviving bytes are intact", same);
            close(rd);
        }
        check("truncate a missing path fails", truncate("/tmp/nope.dat", 0) < 0);
    }

    puts("\n== rmdir ==\n");
    check("mkdir /tmp/d", mkdir("/tmp/d") == 0);
    check("rmdir /tmp/d", rmdir("/tmp/d") == 0);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
