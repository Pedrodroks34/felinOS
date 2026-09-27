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
#define SYS_GETPID    6   /* getpid()                       */
#define SYS_GETPPID   11  /* getppid()                      */
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
#define SYS_MUNMAP    27  /* munmap(addr, len)              */
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
#define SYS_GETCWD    22  /* getcwd(buf, size)              */
#define SYS_CHDIR     21  /* chdir(path)                    */
#define SYS_FCHDIR    66  /* fchdir(fd)                     */
#define SYS_RENAME    20  /* rename(old, new)               */
#define SYS_MKDIR     18  /* mkdir(path, mode)              */
#define SYS_RMDIR     67  /* rmdir(path)                    */
#define SYS_CREAT     68  /* creat(path, mode)              */
#define SYS_LINK      69  /* link(oldpath, newpath)         */
#define SYS_UNLINK    19  /* unlink(path)                   */
#define SYS_SYMLINK   70  /* symlink(target, linkpath)      */
#define SYS_READLINK  71  /* readlink(path, buf, bufsiz)    */
#define SYS_CHMOD     72  /* chmod(path, mode)              */
#define SYS_FCHMOD    73  /* fchmod(fd, mode)               */
#define SYS_CHOWN     74  /* chown(path, uid, gid)          */
#define SYS_FCHOWN    75  /* fchown(fd, uid, gid)           */
#define SYS_LCHOWN    76  /* lchown(path, uid, gid)         */
#define SYS_UMASK     77  /* umask(mask)                    */
#define SYS_GETUID    38  /* getuid()                       */
#define SYS_GETGID    39  /* getgid()                       */
#define SYS_SETUID    42  /* setuid(uid)                    */
#define SYS_SETGID    43  /* setgid(gid)                    */
#define SYS_GETEUID   40  /* geteuid()                      */
#define SYS_GETEGID   41  /* getegid()                      */
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
#define SYS_GETPGID   78  /* getpgid(pid)                   */
#define SYS_SETPGID   79  /* setpgid(pid, pgid)             */

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

#define USER_BASE      0x40000000u
#define USER_HEAP_BASE 0x50000000u
#define USER_HEAP_MAX  0x01000000u
#define USER_STACK_TOP 0x80000000u
#define USER_STACK_SIZE 0x10000u

#endif
