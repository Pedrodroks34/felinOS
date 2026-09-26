#ifndef FELINOS_SYSCALL_H
#define FELINOS_SYSCALL_H

#include <stdint.h>

/* Gato syscall ABI: int 0x80, eax = number, ebx/ecx/edx = args, result in eax. */
#define SYS_EXIT      1   /* exit(code)                     */
#define SYS_WRITE     2   /* write(fd, buf, len)            */
#define SYS_READ      3   /* read(fd, buf, len)             */
#define SYS_OPEN      4   /* open(path, flags)              */
#define SYS_CLOSE     5   /* close(fd)                      */
#define SYS_GETPID    6   /* getpid()                       */
#define SYS_UPTIME    7   /* uptime() -> milliseconds       */
#define SYS_SLEEP     8   /* sleep(ms)                      */
#define SYS_SBRK      9   /* sbrk(incr) -> old break        */
#define SYS_YIELD     10
#define SYS_GETPPID   11
#define SYS_SPAWN     12
#define SYS_WAIT      13
#define SYS_KILL      14
#define SYS_LSEEK     15
#define SYS_STAT      16
#define SYS_READDIR   17
#define SYS_MKDIR     18
#define SYS_UNLINK    19
#define SYS_RENAME    20
#define SYS_CHDIR     21
#define SYS_GETCWD    22
#define SYS_DUP       23
#define SYS_DUP2      24
#define SYS_PIPE      25
#define SYS_TIME      26
#define SYS_MUNMAP    27
#define SYS_FORK      28
#define SYS_EXECVE    29
#define SYS_SIGACTION   30  /* sigaction(sig, k_sigaction*, k_sigaction*)   */
#define SYS_SIGRETURN   31  /* sigreturn(), only ever reached from the trampoline */
#define SYS_SIGPROCMASK 32  /* sigprocmask(how, uint32_t*, uint32_t*)      */
#define SYS_SOCKET    33  /* socket(domain, type, protocol) -> fd         */
#define SYS_BIND      34  /* bind(fd, sockaddr_in*, addrlen)              */
#define SYS_CONNECT   35  /* connect(fd, sockaddr_in*, addrlen)           */
#define SYS_LISTEN    36  /* listen(fd, backlog)                          */
#define SYS_ACCEPT    37  /* accept(fd, sockaddr_in*, addrlen*)           */

#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17
#define SIGCONT    18
#define SIGSTOP    19
#define SIGTSTP    20
#define NSIG       32

#define SIG_DFL_VAL 0u
#define SIG_IGN_VAL 1u

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

struct k_sigaction {
    uint32_t handler;
    uint32_t restorer;
    uint32_t mask;
    uint32_t flags;
};

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define USER_BASE      0x40000000u
#define USER_HEAP_BASE 0x50000000u
#define USER_HEAP_MAX  0x01000000u
#define USER_STACK_TOP 0x80000000u
#define USER_STACK_SIZE 0x10000u

#endif
