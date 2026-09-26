#ifndef FELINOS_SCHED_H
#define FELINOS_SCHED_H

#include <stdint.h>
#include "idt.h"
#include "vmm.h"
#include "fpu.h"
#include "syscall.h"

#define SCHED_MAX_TASKS       32
#define SCHED_NAME_LEN        16
#define SCHED_DEFAULT_QUANTUM 5
#define SCHED_MAX_QUANTUM     200
#define SCHED_MIN_NICE        (-6)
#define SCHED_MAX_NICE        6
#define SCHED_SIGINT          2
#define SCHED_SIGKILL         9
#define SCHED_SIGTERM         15

enum task_state {
    TASK_UNUSED = 0,
    TASK_NEW,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_ZOMBIE,
    TASK_DYING
};

struct task {
    int pid;
    int ppid;
    int state;
    int user;
    int nice;
    uint16_t uid;
    uint16_t gid;
    uint32_t sig_pending;
    uint32_t sig_blocked;
    uint64_t sig_handler[NSIG];
    uint64_t sig_restorer[NSIG];
    uint32_t sig_mask[NSIG];
    int nokill;      /* >0: in an uninterruptible kernel section, signals are deferred */
    int wait_timed;  /* blocked with a deadline in wake_tick (sched_wait_on_timeout) */
    int exit_code;
    char name[SCHED_NAME_LEN];
    const char *wait_reason;
    const void *wait_chan;
    uint32_t wake_tick;
    uint32_t slice_left;
    uint32_t cpu_ticks;
    uint32_t start_tick;
    uint32_t switches;
    uint64_t ksp;
    uint32_t kstack;
    uint32_t kstack_size;
    struct vm_space *space;
    void (*entry)(void *);
    void *arg;
    void *priv;
    struct task *next;
    int preempt;         /* >0: kernel preemption disabled (spinlock held) */
    int exiting;         /* task_exit() in progress: no more kill checks */
    int sync_grant;      /* semaphore token handed over by sem_post() */
    uint32_t wait_seq;   /* FIFO order among tasks blocked on one channel */
    uint32_t kpreempted; /* times this task was preempted inside the kernel */
    uint8_t fpu[FPU_STATE_SIZE] __attribute__((aligned(16)));
};

struct task_info {
    int pid;
    int ppid;
    int state;
    int user;
    int nice;
    char name[SCHED_NAME_LEN];
    const char *wait_reason;
    uint32_t cpu_ticks;
    uint32_t lifetime_ticks;
    uint32_t switches;
};

struct sched_stats {
    uint32_t switches;
    uint32_t preemptions;
    uint32_t kernel_preemptions;
    uint32_t atomic_sleep_bugs;
    int kpreempt;
    uint32_t total_ticks;
    uint32_t idle_ticks;
    uint32_t quantum;
    uint32_t ready;
    uint32_t tasks;
};

void sched_init(void);
void sched_run(void (*shell_entry)(void *)) __attribute__((noreturn));
int sched_active(void);
int sched_can_sleep(void);

struct task *sched_current(void);
struct task *sched_idle_task(void);
struct task *task_find(int pid);
int task_slot(const struct task *t);

struct task *task_new(const char *name, void (*entry)(void *), void *arg,
                      uint32_t stack_bytes, struct vm_space *space, int user);
void task_start(struct task *t);
void task_discard(struct task *t);
void task_exit(int code) __attribute__((noreturn));
void sched_set_exit_hook(void (*fn)(struct task *));
int task_signal(int pid, int sig);
int task_kill(int pid, int sig);
int task_wait(int pid, int *code);
int task_collect(int ppid, int *pid, int *code, char *name, uint32_t size);
int task_set_nice(int pid, int nice);

void schedule(void);
void sched_yield(void);
void sched_sleep_ticks(uint32_t ticks);
void sched_wait_on(const void *chan, const char *reason);
void sched_wait_on_timeout(const void *chan, const char *reason, uint32_t ticks);
void task_uninterruptible_enter(void);
void task_uninterruptible_leave(void);
void sched_wake(const void *chan);
struct task *sched_wake_one(const void *chan);
void irq_enter(void);
void irq_exit(void);
int in_irq(void);
void preempt_disable(void);
void preempt_enable(void);
int preempt_count(void);
int sched_kpreempt_enabled(void);
void sched_set_kpreempt(int on);
const char *sched_atomic_bug_task(void);
void sched_set_space(struct vm_space *space);
struct task *kthread_create(const char *name, void (*entry)(void *), void *arg);
void sched_tick(void);
void sched_irq_return(struct regs *r);
void sched_syscall_return(struct regs *r);

const void *sched_input_channel(void);
void sched_wake_input(void);
void sched_set_foreground(int pid);
int sched_foreground(void);
int sched_is_foreground(const struct task *t);
int sched_shell_pid(void);
int sched_break(void);

uint32_t sched_quantum(void);
void sched_set_quantum(uint32_t ticks);
int sched_snapshot(struct task_info *out, int max);
void sched_get_stats(struct sched_stats *out);
const char *sched_state_name(int state);

extern void switch_context(uint64_t *old_sp, uint64_t new_sp);

#endif
