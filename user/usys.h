#ifndef USYS_H
#define USYS_H
#include <stdint.h>
#include "syscall.h"

#ifdef __x86_64__
typedef long sysint;
static inline sysint sys3(sysint n, sysint a, sysint b, sysint c) {
    sysint r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}
static inline sysint sys4(sysint n, sysint a, sysint b, sysint c, sysint d) {
    sysint r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "c"(d) : "memory");
    return r;
}
static inline sysint sys5(sysint n, sysint a, sysint b, sysint c, sysint d, sysint e) {
    sysint r;
    register sysint r8 asm("r8") = e;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "c"(d), "r"(r8) : "memory");
    return r;
}
static inline sysint sys6(sysint n, sysint a, sysint b, sysint c, sysint d, sysint e, sysint f) {
    sysint r;
    register sysint r8 asm("r8") = e;
    register sysint r9 asm("r9") = f;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "c"(d), "r"(r8), "r"(r9) : "memory");
    return r;
}
#else
typedef int sysint;
static inline sysint sys3(sysint n, sysint a, sysint b, sysint c) {
    sysint r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static inline sysint sys4(sysint n, sysint a, sysint b, sysint c, sysint d) {
    sysint r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}
static inline sysint sys5(sysint n, sysint a, sysint b, sysint c, sysint d, sysint e) {
    sysint r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e) : "memory");
    return r;
}
static inline sysint sys6(sysint n, sysint a, sysint b, sysint c, sysint d, sysint e, sysint f) {
    sysint r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "R"(f)
                      : "memory");
    return r;
}
#endif
static inline void exit(int c)            { sys3(SYS_EXIT, c, 0, 0); for (;;); }
static inline int write(int fd, const void *b, int n) { return sys3(SYS_WRITE, fd, (sysint)b, n); }
static inline int read(int fd, void *b, int n)        { return sys3(SYS_READ, fd, (sysint)b, n); }
static inline int open(const char *p, int fl)         { return sys3(SYS_OPEN, (sysint)p, fl, 0); }
static inline int close(int fd)           { return sys3(SYS_CLOSE, fd, 0, 0); }
static inline int getpid(void)            { return sys3(SYS_GETPID, 0, 0, 0); }
static inline int uptime(void)            { return sys3(SYS_UPTIME, 0, 0, 0); }
static inline void sleep_ms(int ms)       { sys3(SYS_SLEEP, ms, 0, 0); }
static inline void *sbrk(int n)           { return (void *)sys3(SYS_SBRK, n, 0, 0); }
static inline void yield(void)            { sys3(SYS_YIELD, 0, 0, 0); }
static inline int getppid(void)           { return sys3(SYS_GETPPID, 0, 0, 0); }
static inline int spawn(const char *p, char *const *argv) { return sys3(SYS_SPAWN, (sysint)p, (sysint)argv, 0); }
static inline int waitpid(int pid, int *st)               { return sys3(SYS_WAIT, pid, (sysint)st, 0); }
static inline int kill(int pid, int sig)  { return sys3(SYS_KILL, pid, sig, 0); }
static inline int lseek(int fd, int off, int whence)  { return sys3(SYS_LSEEK, fd, off, whence); }
static inline int stat(const char *p, void *st)       { return sys3(SYS_STAT, (sysint)p, (sysint)st, 0); }
static inline int readdir_raw(const char *p, int i, void *out) { return sys3(SYS_READDIR, (sysint)p, i, (sysint)out); }
static inline int mkdir(const char *p)    { return sys3(SYS_MKDIR, (sysint)p, 0, 0); }
static inline int unlink(const char *p)   { return sys3(SYS_UNLINK, (sysint)p, 0, 0); }
static inline int rename(const char *a, const char *b) { return sys3(SYS_RENAME, (sysint)a, (sysint)b, 0); }
static inline int chdir(const char *p)    { return sys3(SYS_CHDIR, (sysint)p, 0, 0); }
static inline int getcwd(char *buf, int size) { return sys3(SYS_GETCWD, (sysint)buf, size, 0); }
static inline int dup(int fd)             { return sys3(SYS_DUP, fd, 0, 0); }
static inline int dup2(int oldfd, int newfd) { return sys3(SYS_DUP2, oldfd, newfd, 0); }
static inline int pipe(int fds[2])        { return sys3(SYS_PIPE, (sysint)fds, 0, 0); }
static inline int time(void)              { return sys3(SYS_TIME, 0, 0, 0); }
static inline int munmap(void *addr, unsigned len) { return sys3(SYS_MUNMAP, (sysint)addr, len, 0); }
static inline void *mmap(void *addr, unsigned len, int prot, int flags, int fd, long off) {
    return (void *)sys6(SYS_MMAP, (sysint)addr, len, prot, flags, fd, off);
}
static inline int mprotect(void *addr, unsigned len, int prot) {
    return sys3(SYS_MPROTECT, (sysint)addr, len, prot);
}
static inline int madvise(void *addr, unsigned len, int advice) {
    return sys3(SYS_MADVISE, (sysint)addr, len, advice);
}
static inline int msync(void *addr, unsigned len, int flags) {
    return sys3(SYS_MSYNC, (sysint)addr, len, flags);
}
static inline int truncate(const char *path, unsigned len) { return sys3(SYS_TRUNCATE, (sysint)path, len, 0); }
static inline int ftruncate(int fd, unsigned len)         { return sys3(SYS_FTRUNCATE, fd, len, 0); }
static inline int rmdir(const char *path)                  { return sys3(SYS_RMDIR, (sysint)path, 0, 0); }
static inline int gettimeofday(struct timeval *tv)         { return sys3(SYS_GETTIMEOFDAY, (sysint)tv, 0, 0); }
static inline int clock_gettime(int clk, struct timespec *ts) {
    return sys3(SYS_CLOCK_GETTIME, clk, (sysint)ts, 0);
}
static inline int getrlimit(int res, unsigned long *lim)  { return sys3(SYS_GETRLIMIT, res, (sysint)lim, 0); }
static inline int setrlimit(int res, const unsigned long *lim) { return sys3(SYS_SETRLIMIT, res, (sysint)lim, 0); }
static inline int fork(void)              { return sys3(SYS_FORK, 0, 0, 0); }
static inline int execve(const char *p, char *const *argv, char *const *envp) {
    (void)envp;
    return sys3(SYS_EXECVE, (sysint)p, (sysint)argv, 0);
}

static inline int strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline void puts(const char *s)  { write(1, s, strlen(s)); }
static inline void putnum(int v) {
    char b[12]; int i = 11; unsigned u = v < 0 ? -(unsigned)v : (unsigned)v;
    b[i] = 0;
    do { b[--i] = '0' + u % 10; u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    puts(b + i);
}
static inline unsigned parse_uint(const char *s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}
static inline int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
#endif
