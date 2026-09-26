#ifndef FELINOS_USER_SIGNAL_H
#define FELINOS_USER_SIGNAL_H

#include "syscall.h"

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

struct sigaction {
    sighandler_t sa_handler;
    unsigned int sa_mask;
    unsigned int sa_flags;
};

sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const unsigned int *set, unsigned int *old);
int raise(int sig);

#endif
