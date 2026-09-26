#include "signal.h"
#include "usys.h"

#define XSTR(x) #x
#define STR(x) XSTR(x)

__attribute__((naked)) static void sig_trampoline(void) {
    __asm__("movl $" STR(SYS_SIGRETURN) ", %eax\n\tint $0x80");
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    struct k_sigaction kact;
    struct k_sigaction kold;
    struct k_sigaction *actp = 0;
    struct k_sigaction *oldp = 0;

    if (act) {
        kact.handler = (unsigned int)(unsigned long)act->sa_handler;
        kact.restorer = (unsigned int)(unsigned long)sig_trampoline;
        kact.mask = act->sa_mask;
        kact.flags = act->sa_flags;
        actp = &kact;
    }
    if (old) {
        oldp = &kold;
    }
    int r = (int)sys3(SYS_SIGACTION, sig, (sysint)(unsigned long)actp, (sysint)(unsigned long)oldp);
    if (r == 0 && old) {
        old->sa_handler = (sighandler_t)(unsigned long)kold.handler;
        old->sa_mask = kold.mask;
        old->sa_flags = kold.flags;
    }
    return r;
}

sighandler_t signal(int sig, sighandler_t handler) {
    struct sigaction act;
    struct sigaction old;

    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = 0;
    if (sigaction(sig, &act, &old) < 0) {
        return SIG_DFL;
    }
    return old.sa_handler;
}

int sigprocmask(int how, const unsigned int *set, unsigned int *old) {
    return (int)sys3(SYS_SIGPROCMASK, how, (sysint)(unsigned long)set, (sysint)(unsigned long)old);
}

int raise(int sig) {
    return kill(getpid(), sig);
}
