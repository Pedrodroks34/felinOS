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

/* Linux-compatible syscalls */
#define SYS_GETUID    38  /* getuid()                       */
#define SYS_GETGID    39  /* getgid()                       */
#define SYS_GETEUID   40  /* geteuid()                      */
#define SYS_GETEGID   41  /* getegid()                      */
#define SYS_SETUID    42  /* setuid(uid)                    */
#define SYS_SETGID    43  /* setgid(gid)                    */
#define SYS_GETTID    44  /* gettid()                       */
#define SYS_TIMES     45  /* times(struct tms*)             */
#define SYS_UNAME     46  /* uname(struct utsname*)         */
#define SYS_GETRLIMIT 47  /* getrlimit(resource, rlimit*)   */
#define SYS_SETRLIMIT 48  /* setrlimit(resource, rlimit*)   */
#define SYS_GETRUSAGE 49  /* getrusage(who, rusage*)        */
#define SYS_GETTIMEOFDAY 50 /* gettimeofday(timeval*, timezone*) */
#define SYS_SETTIMEOFDAY 51 /* settimeofday(timeval*, timezone*) */
#define SYS_NANOSLEEP 52  /* nanosleep(timespec*, timespec*) */
#define SYS_CLOCK_GETTIME 53 /* clock_gettime(clk_id, timespec*) */
#define SYS_CLOCK_SETTIME 54 /* clock_settime(clk_id, timespec*) */
#define SYS_MMAP      55  /* mmap(addr, len, prot, flags, fd, off) */
#define SYS_MPROTECT  56  /* mprotect(addr, len, prot)      */
#define SYS_MSYNC     57  /* msync(addr, len, flags)        */
#define SYS_MADVISE   58  /* madvise(addr, len, advice)     */
#define SYS_GETDENTS  59  /* getdents(fd, dirent*, count)   */
#define SYS_FCNTL     60  /* fcntl(fd, cmd, arg)            */
#define SYS_IOCTL     61  /* ioctl(fd, request, arg)        */
#define SYS_FSYNC     62  /* fsync(fd)                      */
#define SYS_FDATASYNC 63  /* fdatasync(fd)                  */
#define SYS_TRUNCATE  64  /* truncate(path, length)         */
#define SYS_FTRUNCATE 65  /* ftruncate(fd, length)          */
#define SYS_FCHDIR    66  /* fchdir(fd)                     */
#define SYS_RMDIR     67  /* rmdir(path)                    */
#define SYS_CREAT     68  /* creat(path, mode)              */
#define SYS_LINK      69  /* link(oldpath, newpath)         */
#define SYS_SYMLINK   70  /* symlink(target, linkpath)      */
#define SYS_READLINK  71  /* readlink(path, buf, bufsiz)    */
#define SYS_CHMOD     72  /* chmod(path, mode)              */
#define SYS_FCHMOD    73  /* fchmod(fd, mode)               */
#define SYS_CHOWN     74  /* chown(path, uid, gid)          */
#define SYS_FCHOWN    75  /* fchown(fd, uid, gid)           */
#define SYS_LCHOWN    76  /* lchown(path, uid, gid)         */
#define SYS_UMASK     77  /* umask(mask)                    */
#define SYS_GETPGID   78  /* getpgid(pid)                   */
#define SYS_SETPGID   79  /* setpgid(pid, pgid)             */
#define SYS_GETSID    80  /* getsid(pid)                    */
#define SYS_SETSID    81  /* setsid()                       */
#define SYS_GETGROUPS 82  /* getgroups(size, list)          */
#define SYS_SETGROUPS 83  /* setgroups(size, list)          */
#define SYS_GETRESUID 84  /* getresuid(ruid, euid, suid)    */
#define SYS_GETRESGID 85  /* getresgid(rgid, egid, sgid)    */
#define SYS_SETRESUID 86  /* setresuid(ruid, euid, suid)    */
#define SYS_SETRESGID 87  /* setresgid(rgid, egid, sgid)    */
#define SYS_SEND      88  /* send(fd, buf, len, flags)                  */
#define SYS_RECV      89  /* recv(fd, buf, len, flags)                  */
#define SYS_SENDTO    90  /* sendto(fd, buf, len, flags, sockaddr_in*)  */
#define SYS_RECVFROM  91  /* recvfrom(fd, buf, len, flags, sockaddr_in*) */
#define SYS_GETSOCKNAME 92 /* getsockname(fd, sockaddr_in*, addrlen)     */
#define SYS_SETSOCKOPT 93  /* setsockopt(fd, level, optname, optval, optlen) */
#define SYS_GETHOSTBYNAME 94 /* gethostbyname(name, char *addr_out) -> ip or -1 */
#define SYS_GETIFADDR   95  /* getifaddr(struct k_ifinfo*)                */
#define SYS_GETIFADDRS  96  /* getifaddrs(struct k_ifinfo*, int max) -> n  */

/* NUMA syscalls */
#define SYS_MBIND       97  /* mbind(addr, len, mode, nmask, maxnode, flags) */
#define SYS_SET_MEMPOLICY 98 /* set_mempolicy(mode, nmask, maxnode) */
#define SYS_GET_MEMPOLICY 99 /* get_mempolicy(policy, nmask, maxnode, addr) */

struct k_ifinfo {
    uint32_t ip;         /* host byte order, 0 when the link is down */
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
    uint8_t mac[6];
    uint8_t up;
    uint8_t dhcp_bound;
    char driver[16];
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_errors;
    uint32_t tx_errors;
};

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

struct timeval {
    uint32_t tv_sec;
    uint32_t tv_usec;
};

struct timespec {
    uint32_t tv_sec;
    uint32_t tv_nsec;
};

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* mmap() protection bits. */
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

/* mmap() flags. */
#define MAP_SHARED    0x0001
#define MAP_PRIVATE   0x0002
#define MAP_FIXED     0x0010
#define MAP_ANONYMOUS 0x0020

/* madvise() advice values. */
#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       5

/* clock_gettime() clock ids. */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_BOOTTIME           7

/* getrlimit()/setrlimit() resources. */
#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_AS      9
#define RLIMIT_NOFILE  7
#define RLIMIT_NPROC   6

/* fcntl() commands. */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define F_SETSIZE 5
#define F_GETLK   6
#define F_SETLK   7

#define USER_BASE      0x40000000u
#define USER_HEAP_BASE 0x50000000u
#define USER_HEAP_MAX  0x01000000u
#define USER_STACK_TOP 0x80000000u
#define USER_STACK_SIZE 0x10000u

#endif
