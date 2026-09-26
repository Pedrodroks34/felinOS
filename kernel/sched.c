#include <stdint.h>
#include "sched.h"
#include "gdt.h"
#include "io.h"
#include "pmm.h"
#include "drivers/pit.h"
#include "lib/string.h"
#include "fpu.h"

#define SHELL_STACK_BYTES 32768u

static struct task tasks[SCHED_MAX_TASKS];
static struct task *current;
static struct task *idle_task;
static struct task *shell_task;
static struct task *runq_head;
static struct task *runq_tail;

static volatile int need_resched;
static int started;
static int next_pid;
static int foreground;
static uint32_t default_quantum;
static uint32_t total_switches;
static uint32_t total_preemptions;
static uint32_t total_ticks;
static uint32_t idle_ticks;
static char input_anchor;
static uint32_t total_kpreempt;
static int kpreempt_enabled = 1;
static int early_preempt;
static uint32_t wait_seq_counter;
static uint32_t atomic_bugs;
static char atomic_bug_task[SCHED_NAME_LEN];

static void runq_push(struct task *t) {
    t->state = TASK_READY;
    t->wait_timed = 0;
    t->next = NULL;
    if (runq_tail) {
        runq_tail->next = t;
    } else {
        runq_head = t;
    }
    runq_tail = t;
}

static struct task *runq_pop(void) {
    struct task *t = runq_head;

    if (t) {
        runq_head = t->next;
        if (!runq_head) {
            runq_tail = NULL;
        }
        t->next = NULL;
    }
    return t;
}

static uint32_t slice_for(const struct task *t) {
    int32_t q = ((int32_t)default_quantum * (8 - t->nice) + 4) / 8;

    return q < 1 ? 1u : (uint32_t)q;
}

static void check_killed(void) {
    struct task *t = current;

    /* Inside an uninterruptible section (e.g. holding a disk channel) the
       task must run to the end of the section so it can release what it
       holds; the pending signal is acted on at the next check after that. */
    if (!t->user || t->nokill || t->exiting) {
        return;
    }
    for (int s = 1; s < NSIG; s++) {
        uint32_t bit = 1u << (s - 1);

        if (!(t->sig_pending & bit)) {
            continue;
        }
        if (s != SIGKILL && (t->sig_blocked & bit)) {
            continue;
        }
        uint64_t h = t->sig_handler[s - 1];
        if (s != SIGKILL && h != SIG_DFL_VAL && h != SIG_IGN_VAL) {
            continue;
        }
        t->sig_pending &= ~bit;
        if (h == SIG_IGN_VAL && s != SIGKILL) {
            continue;
        }
        task_exit(128 + s);
    }
}

static void deliver_one(struct task *t, struct regs *r, int sig, uint64_t handler) {
    struct sigframe {
        struct regs regs;
        uint64_t prev_blocked;
        uint64_t reserved;
    };
    uint32_t sp = (uint32_t)r->rsp & ~0xFu;

    sp -= sizeof(struct sigframe);
    struct sigframe *frame = (struct sigframe *)(uintptr_t)sp;
    frame->regs = *r;
    frame->prev_blocked = t->sig_blocked;
    frame->reserved = 0;
    sp -= 8;
    *(uint64_t *)(uintptr_t)sp = t->sig_restorer[sig - 1];

    r->rdi = (uint64_t)sig;
    r->rsi = 0;
    r->rdx = 0;
    r->rip = handler;
    r->rsp = sp;
    t->sig_blocked |= (1u << (sig - 1)) | t->sig_mask[sig - 1];
    if (sig != SIGKILL) {
        t->sig_blocked &= ~(1u << (SIGKILL - 1));
    }
}

static void sched_try_deliver(struct regs *r) {
    struct task *t = current;

    if (!t->user || t->nokill || t->exiting || r->cs != 0x2B) {
        return;
    }
    for (int s = 1; s < NSIG; s++) {
        uint32_t bit = 1u << (s - 1);

        if (!(t->sig_pending & bit) || (t->sig_blocked & bit)) {
            continue;
        }
        uint64_t h = t->sig_handler[s - 1];
        if (h == SIG_DFL_VAL || h == SIG_IGN_VAL) {
            continue;
        }
        t->sig_pending &= ~bit;
        deliver_one(t, r, s, h);
        return;
    }
}

static void kthread_start(void) {
    struct task *t = current;

    fpu_restore(t->fpu);
    check_killed();
    sti();
    t->entry(t->arg);
    task_exit(0);
}

void sched_init(void) {
    memset(tasks, 0, sizeof(tasks));
    idle_task = &tasks[0];
    idle_task->pid = 0;
    idle_task->ppid = 0;
    idle_task->state = TASK_RUNNING;
    idle_task->space = vmm_kernel_space();
    fpu_init();
    fpu_default(idle_task->fpu);
    idle_task->start_tick = pit_ticks();
    strlcpy(idle_task->name, "idle", SCHED_NAME_LEN);
    current = idle_task;
    shell_task = NULL;
    runq_head = NULL;
    runq_tail = NULL;
    need_resched = 0;
    next_pid = 1;
    foreground = 0;
    default_quantum = SCHED_DEFAULT_QUANTUM;
    total_switches = 0;
    total_preemptions = 0;
    total_ticks = 0;
    idle_ticks = 0;
    started = 1;
}

int sched_active(void) {
    return started;
}

int sched_can_sleep(void) {
    return started && current != idle_task;
}

struct task *sched_current(void) {
    return current;
}

struct task *sched_idle_task(void) {
    return idle_task;
}

struct task *task_find(int pid) {
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].pid == pid) {
            return &tasks[i];
        }
    }
    return NULL;
}

int task_slot(const struct task *t) {
    return (int)(t - tasks);
}

static void task_release(struct task *t) {
    struct vm_space *sp = t->space;
    uint32_t stack = t->kstack;
    uint32_t size = t->kstack_size;

    if (sp && sp != vmm_kernel_space()) {
        vmm_space_destroy(sp);
    }
    for (uint32_t off = 0; stack && off < size; off += PMM_FRAME_SIZE) {
        pmm_free_frame(stack + off);
    }
    memset(t, 0, sizeof(*t));
}

static void reap_orphans(void) {
    for (;;) {
        uint32_t f = irq_save();
        struct task *zombie = NULL;

        for (int i = 1; i < SCHED_MAX_TASKS; i++) {
            if (tasks[i].state == TASK_ZOMBIE && tasks[i].ppid == 0) {
                zombie = &tasks[i];
                break;
            }
        }
        if (!zombie) {
            irq_restore(f);
            return;
        }
        zombie->state = TASK_DYING;
        irq_restore(f);
        task_release(zombie);
    }
}

struct task *task_new(const char *name, void (*entry)(void *), void *arg,
                      uint32_t stack_bytes, struct vm_space *space, int user) {
    uint32_t frames = (stack_bytes + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    struct task *t = NULL;

    reap_orphans();
    uint32_t f = irq_save();

    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            t = &tasks[i];
            break;
        }
    }
    if (!t) {
        irq_restore(f);
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    fpu_default(t->fpu);
    t->state = TASK_NEW;
    t->pid = next_pid++;
    irq_restore(f);

    uint32_t stack = pmm_alloc_contiguous(frames);
    if (!stack) {
        memset(t, 0, sizeof(*t));
        return NULL;
    }

    uint64_t *sp = (uint64_t *)(uintptr_t)(stack + frames * PMM_FRAME_SIZE);
    *--sp = 0;
    *--sp = (uint64_t)(uintptr_t)kthread_start;
    for (int i = 0; i < 6; i++) {
        *--sp = 0;   /* rbp rbx r12 r13 r14 r15 */
    }

    t->ppid = current->pid;
    t->user = user;
    t->nice = 0;
    t->uid = current->uid;
    t->gid = current->gid;
    t->entry = entry;
    t->arg = arg;
    t->space = space ? space : vmm_kernel_space();
    t->kstack = stack;
    t->kstack_size = frames * PMM_FRAME_SIZE;
    t->ksp = (uint64_t)(uintptr_t)sp;
    strlcpy(t->name, name ? name : "task", SCHED_NAME_LEN);
    return t;
}

void task_start(struct task *t) {
    uint32_t f = irq_save();

    t->start_tick = pit_ticks();
    runq_push(t);
    irq_restore(f);
}

void task_discard(struct task *t) {
    uint32_t f = irq_save();

    t->state = TASK_DYING;
    irq_restore(f);
    task_release(t);
}

static void do_schedule(void) {
    uint32_t flags = irq_save();
    struct task *prev = current;
    struct task *next;

    if (prev->preempt > 0 && prev->state != TASK_RUNNING) {
        atomic_bugs++;
        strlcpy(atomic_bug_task, prev->name, sizeof(atomic_bug_task));
    }
    need_resched = 0;
    if (prev->state == TASK_RUNNING) {
        if (prev == idle_task) {
            prev->state = TASK_READY;
        } else {
            runq_push(prev);
        }
    }
    next = runq_pop();
    if (!next) {
        next = idle_task;
    }
    next->state = TASK_RUNNING;
    next->slice_left = slice_for(next);

    if (next != prev) {
        current = next;
        next->switches++;
        total_switches++;
        if (vmm_current_space() != next->space) {
            vmm_space_switch(next->space);
        }
        if (next->user) {
            tss_set_kernel_stack(next->kstack + next->kstack_size);
        }
        fpu_save(prev->fpu);
        switch_context(&prev->ksp, next->ksp);
        fpu_restore(prev->fpu);   /* running again as `prev`: reload its own state */
    }
    irq_restore(flags);
}

void schedule(void) {
    do_schedule();
    check_killed();
}

void sched_yield(void) {
    if (started) {
        schedule();
    }
}

void sched_sleep_ticks(uint32_t ticks) {
    if (!sched_can_sleep()) {
        return;
    }
    if (!ticks) {
        schedule();
        return;
    }
    uint32_t f = irq_save();

    current->wake_tick = pit_ticks() + ticks;
    current->state = TASK_SLEEPING;
    schedule();
    irq_restore(f);
}

void sched_wait_on(const void *chan, const char *reason) {
    struct task *t = current;
    uint32_t f = irq_save();

    if (t == idle_task) {
        irq_restore(f);
        return;
    }
    t->wait_chan = chan;
    t->wait_reason = reason;
    t->wait_seq = ++wait_seq_counter;
    t->state = TASK_BLOCKED;
    schedule();
    t->wait_reason = NULL;
    irq_restore(f);
}

/*
 * Like sched_wait_on(), but the task is also made runnable again once
 * `ticks` PIT ticks have passed, whether or not `chan` was ever woken.
 * The caller re-checks its own condition (and the clock) after returning.
 */
void sched_wait_on_timeout(const void *chan, const char *reason, uint32_t ticks) {
    struct task *t = current;
    uint32_t f = irq_save();

    if (t == idle_task) {
        irq_restore(f);
        return;
    }
    t->wait_chan = chan;
    t->wait_reason = reason;
    t->wake_tick = pit_ticks() + (ticks ? ticks : 1);
    t->wait_timed = 1;
    t->wait_seq = ++wait_seq_counter;
    t->state = TASK_BLOCKED;
    schedule();
    t->wait_reason = NULL;
    irq_restore(f);
}

/*
 * Mark the current task as non-killable until the matching _leave().
 * task_signal() still records the signal in t->sig_pending, but neither wakes the
 * task out of a wait nor lets schedule() terminate it, so kernel code that
 * holds a hardware resource always gets to unwind and release it.
 */
void task_uninterruptible_enter(void) {
    if (current) {
        uint32_t f = irq_save();
        current->nokill++;
        irq_restore(f);
    }
}

void task_uninterruptible_leave(void) {
    if (current) {
        uint32_t f = irq_save();
        if (current->nokill > 0) {
            current->nokill--;
        }
        irq_restore(f);
    }
}

void sched_wake(const void *chan) {
    uint32_t f = irq_save();

    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_BLOCKED && t->wait_chan == chan) {
            t->wait_chan = NULL;
            runq_push(t);
        }
    }
    irq_restore(f);
}

struct task *sched_wake_one(const void *chan) {
    uint32_t f = irq_save();
    struct task *best = NULL;

    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_BLOCKED && t->wait_chan == chan &&
            (!best || (int32_t)(t->wait_seq - best->wait_seq) < 0)) {
            best = t;
        }
    }
    if (best) {
        best->wait_chan = NULL;
        runq_push(best);
    }
    irq_restore(f);
    return best;
}

void sched_tick(void) {
    if (!started) {
        return;
    }
    uint32_t now = pit_ticks();
    struct task *c = current;

    total_ticks++;
    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_tick) >= 0) {
            runq_push(t);
        } else if (t->state == TASK_BLOCKED && t->wait_timed &&
                   (int32_t)(now - t->wake_tick) >= 0) {
            t->wait_chan = NULL;
            runq_push(t);
        }
    }

    c->cpu_ticks++;
    if (c == idle_task) {
        idle_ticks++;
        if (runq_head) {
            need_resched = 1;
        }
        return;
    }
    if (c->slice_left) {
        c->slice_left--;
    }
    if (!c->slice_left) {
        if (runq_head) {
            need_resched = 1;
        } else {
            c->slice_left = slice_for(c);
        }
    }
}

void sched_irq_return(struct regs *r) {
    if (!started) {
        return;
    }
    if ((r->cs & 3) != 3) {
        /* Interrupted kernel code. It may be preempted only if it holds no
           spinlock, was running with interrupts enabled (so it was not inside
           an irq_save() section) and is not the idle loop. A pending kill is
           NOT acted on here: the code could be half way through an update. */
        struct task *c = current;

        if (need_resched && kpreempt_enabled && c != idle_task && c->preempt == 0 &&
            (r->rflags & 0x200)) {
            total_kpreempt++;
            c->kpreempted++;
            do_schedule();
        }
        return;
    }
    if (need_resched) {
        total_preemptions++;
        schedule();
    }
    check_killed();
    sched_try_deliver(r);
}

void sched_syscall_return(struct regs *r) {
    if (!started) {
        return;
    }
    if (need_resched) {
        schedule();
    }
    check_killed();
    sched_try_deliver(r);
}

static void (*exit_hook)(struct task *);

void sched_set_exit_hook(void (*fn)(struct task *)) {
    exit_hook = fn;
}

void task_exit(int code) {
    cli();
    struct task *t = current;

    t->exiting = 1;
    if (t == idle_task) {
        for (;;) {
            hlt();
        }
    }
    if (exit_hook && t->user) {
        exit_hook(t);
    }
    t->exit_code = code;
    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        struct task *c = &tasks[i];
        if (c != t && c->state != TASK_UNUSED && c->ppid == t->pid) {
            c->ppid = 0;
        }
    }
    if (foreground == t->pid) {
        foreground = shell_task ? shell_task->pid : 0;
    }
    t->state = TASK_ZOMBIE;

    struct task *parent = task_find(t->ppid);
    if (parent) {
        sched_wake(parent);
    }
    schedule();
    for (;;) {
        hlt();
    }
}

int task_signal(int pid, int sig) {
    uint32_t f = irq_save();
    struct task *t = task_find(pid);
    int result = -1;

    if (t && t->user && t->state != TASK_ZOMBIE && t->state != TASK_DYING) {
        if (sig > 0 && sig < NSIG) {
            uint32_t bit = 1u << (sig - 1);
            int ignored = sig != SIGKILL && t->sig_handler[sig - 1] == SIG_IGN_VAL;

            if (!ignored) {
                t->sig_pending |= bit;
            }
            int deliverable = sig == SIGKILL || !(t->sig_blocked & bit);
            if (!ignored && deliverable && !t->nokill &&
                (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)) {
                t->wait_chan = NULL;
                runq_push(t);
            }
        }
        result = 0;
    }
    irq_restore(f);
    return result;
}

int task_kill(int pid, int sig) {
    return task_signal(pid, sig);
}

int task_wait(int pid, int *code) {
    struct task *me = current;

    for (;;) {
        uint32_t f = irq_save();
        struct task *zombie = NULL;
        int children = 0;

        for (int i = 1; i < SCHED_MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            if (t == me || t->state == TASK_UNUSED || t->state == TASK_DYING || t->ppid != me->pid) {
                continue;
            }
            if (pid > 0 && t->pid != pid) {
                continue;
            }
            children++;
            if (t->state == TASK_ZOMBIE) {
                zombie = t;
                break;
            }
        }
        if (zombie) {
            int reaped = zombie->pid;
            if (code) {
                *code = zombie->exit_code;
            }
            zombie->state = TASK_DYING;
            irq_restore(f);
            task_release(zombie);
            return reaped;
        }
        if (!children) {
            irq_restore(f);
            return -1;
        }
        sched_wait_on(me, "child");
        irq_restore(f);
    }
}

int task_collect(int ppid, int *pid, int *code, char *name, uint32_t size) {
    reap_orphans();
    uint32_t f = irq_save();
    struct task *zombie = NULL;

    for (int i = 1; i < SCHED_MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_ZOMBIE && t->ppid == ppid) {
            zombie = t;
            break;
        }
    }
    if (!zombie) {
        irq_restore(f);
        return 0;
    }
    *pid = zombie->pid;
    *code = zombie->exit_code;
    strlcpy(name, zombie->name, size);
    zombie->state = TASK_DYING;
    irq_restore(f);
    task_release(zombie);
    return 1;
}

void sched_run(void (*shell_entry)(void *)) {
    shell_task = task_new("vsh", shell_entry, NULL, SHELL_STACK_BYTES, vmm_kernel_space(), 0);
    if (shell_task) {
        foreground = shell_task->pid;
        task_start(shell_task);
    }
    sti();
    for (;;) {
        reap_orphans();
        cli();
        if (runq_head) {
            sti();
            schedule();
        } else {
            __asm__ volatile ("sti; hlt");
        }
    }
}

const void *sched_input_channel(void) {
    return &input_anchor;
}

void sched_wake_input(void) {
    if (started) {
        sched_wake(&input_anchor);
    }
}

void sched_set_foreground(int pid) {
    foreground = pid;
}

int sched_foreground(void) {
    return foreground;
}

int sched_is_foreground(const struct task *t) {
    return t->pid == foreground;
}

int sched_shell_pid(void) {
    return shell_task ? shell_task->pid : 0;
}

int sched_break(void) {
    if (!started || !shell_task || foreground == shell_task->pid) {
        return 0;
    }
    return task_signal(foreground, SCHED_SIGINT) == 0;
}

int task_set_nice(int pid, int nice) {
    uint32_t f = irq_save();
    struct task *t = task_find(pid);

    if (nice < SCHED_MIN_NICE) {
        nice = SCHED_MIN_NICE;
    }
    if (nice > SCHED_MAX_NICE) {
        nice = SCHED_MAX_NICE;
    }
    if (t && t != idle_task && t->state != TASK_ZOMBIE && t->state != TASK_DYING) {
        t->nice = nice;
        irq_restore(f);
        return 0;
    }
    irq_restore(f);
    return -1;
}

uint32_t sched_quantum(void) {
    return default_quantum;
}

void sched_set_quantum(uint32_t ticks) {
    if (ticks < 1) {
        ticks = 1;
    }
    if (ticks > SCHED_MAX_QUANTUM) {
        ticks = SCHED_MAX_QUANTUM;
    }
    default_quantum = ticks;
}

int sched_snapshot(struct task_info *out, int max) {
    uint32_t f = irq_save();
    uint32_t now = pit_ticks();
    int n = 0;

    for (int i = 0; i < SCHED_MAX_TASKS && n < max; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_UNUSED || t->state == TASK_NEW || t->state == TASK_DYING) {
            continue;
        }
        out[n].pid = t->pid;
        out[n].ppid = t->ppid;
        out[n].state = t->state;
        out[n].user = t->user;
        out[n].nice = t->nice;
        out[n].wait_reason = t->wait_reason;
        out[n].cpu_ticks = t->cpu_ticks;
        out[n].lifetime_ticks = now - t->start_tick;
        out[n].switches = t->switches;
        strlcpy(out[n].name, t->name, SCHED_NAME_LEN);
        n++;
    }
    irq_restore(f);
    return n;
}

void sched_get_stats(struct sched_stats *out) {
    uint32_t f = irq_save();
    uint32_t ready = 0;
    uint32_t count = 0;

    for (struct task *t = runq_head; t; t = t->next) {
        ready++;
    }
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DYING) {
            count++;
        }
    }
    out->switches = total_switches;
    out->preemptions = total_preemptions;
    out->kernel_preemptions = total_kpreempt;
    out->atomic_sleep_bugs = atomic_bugs;
    out->kpreempt = kpreempt_enabled;
    out->total_ticks = total_ticks;
    out->idle_ticks = idle_ticks;
    out->quantum = default_quantum;
    out->ready = ready;
    out->tasks = count;
    irq_restore(f);
}

const char *sched_state_name(int state) {
    switch (state) {
    case TASK_NEW:      return "new";
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
    case TASK_SLEEPING: return "sleeping";
    case TASK_BLOCKED:  return "waiting";
    case TASK_ZOMBIE:   return "zombie";
    default:            return "?";
    }
}

void preempt_disable(void) {
    if (current) {
        current->preempt++;
    } else {
        early_preempt++;
    }
}

void preempt_enable(void) {
    if (!current) {
        early_preempt--;
        return;
    }
    if (--current->preempt == 0 && need_resched && started && kpreempt_enabled &&
        current != idle_task) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
        if (flags & 0x200) {
            do_schedule();
        }
    }
}

int preempt_count(void) {
    return current ? current->preempt : early_preempt;
}

int sched_kpreempt_enabled(void) {
    return kpreempt_enabled;
}

void sched_set_kpreempt(int on) {
    kpreempt_enabled = on != 0;
}

const char *sched_atomic_bug_task(void) {
    return atomic_bug_task;
}

/* A task that temporarily works inside another address space (loading a new
   program image) must record it as its own, or the scheduler would switch
   back to the old one the next time this task is scheduled in. */
void sched_set_space(struct vm_space *space) {
    uint32_t f = irq_save();

    current->space = space;
    vmm_space_switch(space);
    irq_restore(f);
}

struct task *kthread_create(const char *name, void (*entry)(void *), void *arg) {
    struct task *t = task_new(name, entry, arg, 16384u, vmm_kernel_space(), 0);

    if (t) {
        t->ppid = 0;      /* no parent to collect it: reaped as an orphan */
        task_start(t);
    }
    return t;
}

static volatile int irq_depth;

void irq_enter(void) {
    irq_depth++;
}

void irq_exit(void) {
    irq_depth--;
}

int in_irq(void) {
    return irq_depth > 0;
}
