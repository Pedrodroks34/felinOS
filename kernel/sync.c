#include "sync.h"
#include "io.h"
#include "console.h"
#include "drivers/pit.h"
#include "drivers/vga.h"
#include "lib/string.h"

#define SYNC_MAX_LOCKS   96
#define SYNC_WARN_TICKS  (30u * PIT_FREQUENCY)

static char boot_ctx;
static struct { void *p; int kind; } registry[SYNC_MAX_LOCKS];
static int reg_count;
static uint32_t slow_warnings;

static const void *ctx(void) {
    const void *t = sched_current();

    return t ? t : (const void *)&boot_ctx;
}

static void reg(void *p, int kind) {
    uint32_t f = irq_save();

    if (reg_count < SYNC_MAX_LOCKS) {
        registry[reg_count].p = p;
        registry[reg_count].kind = kind;
        reg_count++;
    }
    irq_restore(f);
}

static void sync_bug(const char *what, const char *name) __attribute__((noreturn));
static void sync_bug(const char *what, const char *name) {
    cli();
    console_set_panic();
    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n\n KERNEL PANIC -- Gato \n");
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("\nlock bug: %s (%s)\n", what, name ? name : "?");
    struct task *t = sched_current();
    if (t) {
        kprintf("task %d (%s), preempt count %d\n", t->pid, t->name, t->preempt);
    }
    kprintf("\nSystem halted.\n");
    for (;;) {
        hlt();
    }
}

/* ---------------------------------------------------------------- spinlock */

void spin_init(spinlock_t *l, const char *name) {
    memset(l, 0, sizeof(*l));
    l->name = name;
}

void spin_lock(spinlock_t *l) {
    const void *me = ctx();

    preempt_disable();
    if (l->locked) {
        uint32_t spins = 0;

        if (l->owner == me) {
            sync_bug("spinlock recursion (or IRQ took a lock held by the code it interrupted)", l->name);
        }
        l->contended++;
        while (l->locked) {
            __asm__ volatile ("pause" : : : "memory");
            if (++spins > 200000000u) {
                sync_bug("spinlock deadlock", l->name);
            }
        }
    }
    l->locked = 1;
    l->owner = me;
    l->acquisitions++;
    if (!l->registered) {
        l->registered = 1;
        reg(l, 0);
    }
}

int spin_trylock(spinlock_t *l) {
    preempt_disable();
    if (l->locked) {
        preempt_enable();
        return 0;
    }
    l->locked = 1;
    l->owner = ctx();
    l->acquisitions++;
    return 1;
}

void spin_unlock(spinlock_t *l) {
    if (!l->locked) {
        sync_bug("spin_unlock of a free lock", l->name);
    }
    l->owner = NULL;
    l->locked = 0;
    preempt_enable();
}

uint32_t spin_lock_irqsave(spinlock_t *l) {
    uint32_t f = irq_save();

    spin_lock(l);
    return f;
}

void spin_unlock_irqrestore(spinlock_t *l, uint32_t flags) {
    if (!l->locked) {
        sync_bug("spin_unlock of a free lock", l->name);
    }
    l->owner = NULL;
    l->locked = 0;
    /* Interrupts are still masked here, so releasing the preempt count cannot
       reschedule; a reschedule requested meanwhile is picked up at the next
       timer interrupt return or preempt point. */
    struct task *t = sched_current();
    if (t) {
        t->preempt--;
    } else {
        preempt_enable();
    }
    irq_restore(flags);
}

/* ------------------------------------------------------------------- mutex */

void mutex_init(mutex_t *m, const char *name, int recursive) {
    memset(m, 0, sizeof(*m));
    m->name = name;
    m->recursive = recursive;
}

static void mutex_take(mutex_t *m, const void *me) {
    m->owner = me;
    m->depth = 1;
    task_uninterruptible_enter();
}

static void report_slow(mutex_t *m) {
    const struct task *o = (const struct task *)m->owner;
    struct task *t = sched_current();

    slow_warnings++;
    kprintf("\n[lock] pid %d (%s) has waited over 30 s for mutex '%s' held by pid %d\n",
            t ? t->pid : -1, t ? t->name : "?", m->name ? m->name : "?",
            (o && (const void *)o != (const void *)&boot_ctx) ? o->pid : -1);
}

void mutex_lock(mutex_t *m) {
    const void *me = ctx();
    uint32_t f = irq_save();
    int warned = 0;
    uint32_t start = 0;

    if (!m->registered) {
        m->registered = 1;
        reg(m, 1);
    }
    if (m->owner == me) {
        if (!m->recursive) {
            sync_bug("mutex relocked by its owner (deadlock)", m->name);
        }
        m->depth++;
        irq_restore(f);
        return;
    }
    m->acquisitions++;
    if (!m->owner) {
        mutex_take(m, me);
        irq_restore(f);
        return;
    }

    m->contended++;
    m->waiters++;
    start = pit_ticks();
    while (m->owner != me) {
        if (!m->owner) {
            mutex_take(m, me);
            break;
        }
        if (!sched_can_sleep()) {
            if (!sched_active()) {
                sync_bug("mutex contended before the scheduler is running", m->name);
            }
            /* the idle loop cannot sleep: let the holder run instead */
            irq_restore(f);
            sched_yield();
            f = irq_save();
            continue;
        }
        sched_wait_on_timeout(m, m->name, SYNC_WARN_TICKS);
        if (m->owner != me && !warned && (int32_t)(pit_ticks() - start) >= (int32_t)SYNC_WARN_TICKS) {
            warned = 1;
            irq_restore(f);
            report_slow(m);
            f = irq_save();
        }
    }
    if (m->owner == me) {
        m->waiters--;
    }
    irq_restore(f);
}

int mutex_trylock(mutex_t *m) {
    const void *me = ctx();
    uint32_t f = irq_save();
    int ok = 0;

    if (!m->registered) {
        m->registered = 1;
        reg(m, 1);
    }
    if (!m->owner) {
        m->acquisitions++;
        mutex_take(m, me);
        ok = 1;
    } else if (m->owner == me && m->recursive) {
        m->depth++;
        ok = 1;
    }
    irq_restore(f);
    return ok;
}

void mutex_unlock(mutex_t *m) {
    uint32_t f = irq_save();

    if (m->owner != ctx()) {
        sync_bug("mutex unlocked by a task that does not own it", m->name);
    }
    if (m->depth > 1) {
        m->depth--;
        irq_restore(f);
        return;
    }
    m->depth = 0;
    m->owner = NULL;
    task_uninterruptible_leave();
    if (m->waiters > 0) {
        /* hand the lock straight to the longest waiter (FIFO, no barging);
           it is made unkillable on its behalf so the lock cannot leak */
        struct task *w = sched_wake_one(m);

        if (w) {
            m->owner = w;
            m->depth = 1;
            w->nokill++;
        }
    }
    irq_restore(f);
}

int mutex_locked(const mutex_t *m) {
    return m->owner != NULL;
}

int mutex_owned(const mutex_t *m) {
    return m->owner == ctx();
}

/* --------------------------------------------------------------- semaphore */

void sem_init(semaphore_t *s, int value, const char *name) {
    memset(s, 0, sizeof(*s));
    s->count = value;
    s->name = name;
}

static int sem_down(semaphore_t *s, int timed, uint32_t ticks) {
    uint32_t f = irq_save();
    struct task *me = sched_current();
    int rc = 0;

    if (!s->registered) {
        s->registered = 1;
        reg(s, 2);
    }
    s->waits++;
    if (s->count > 0) {
        s->count--;
        irq_restore(f);
        return 0;
    }
    if (!sched_can_sleep()) {
        if (timed) {
            irq_restore(f);
            return -1;
        }
        sync_bug("sem_wait would block where sleeping is impossible", s->name);
    }
    s->blocked++;
    s->waiters++;
    me->sync_grant = 0;
    uint32_t deadline = pit_ticks() + ticks;
    for (;;) {
        if (s->count > 0) {
            s->count--;
            break;
        }
        if (timed) {
            int32_t left = (int32_t)(deadline - pit_ticks());
            if (left <= 0) {
                rc = -1;
                break;
            }
            sched_wait_on_timeout(s, s->name, (uint32_t)left);
        } else {
            sched_wait_on(s, s->name);
        }
        if (me->sync_grant) {
            break;
        }
    }
    s->waiters--;
    if (me->sync_grant) {
        me->sync_grant = 0;
        if (me->nokill > 0) {
            me->nokill--;
        }
        rc = 0;
    }
    irq_restore(f);
    return rc;
}

void sem_wait(semaphore_t *s) {
    sem_down(s, 0, 0);
}

int sem_timedwait(semaphore_t *s, uint32_t ticks) {
    return sem_down(s, 1, ticks);
}

int sem_trywait(semaphore_t *s) {
    uint32_t f = irq_save();
    int ok = 0;

    if (s->count > 0) {
        s->count--;
        ok = 1;
    }
    irq_restore(f);
    return ok ? 0 : -1;
}

void sem_post(semaphore_t *s) {
    uint32_t f = irq_save();
    struct task *w = s->waiters > 0 ? sched_wake_one(s) : NULL;

    if (w) {
        w->sync_grant = 1;
        w->nokill++;     /* the token is now its own; do not let a kill lose it */
    } else {
        s->count++;
    }
    irq_restore(f);
}

int sem_value(const semaphore_t *s) {
    return s->count;
}

/* -------------------------------------------------------------- statistics */

int sync_snapshot(struct sync_info *out, int max) {
    uint32_t f = irq_save();
    int n = 0;

    for (int i = 0; i < reg_count && n < max; i++) {
        struct sync_info *o = &out[n++];

        memset(o, 0, sizeof(*o));
        o->kind = registry[i].kind;
        o->owner_pid = -1;
        if (o->kind == 0) {
            const spinlock_t *l = registry[i].p;
            o->name = l->name;
            o->acquisitions = l->acquisitions;
            o->contended = l->contended;
            o->held = l->locked;
        } else if (o->kind == 1) {
            const mutex_t *m = registry[i].p;
            o->name = m->name;
            o->acquisitions = m->acquisitions;
            o->contended = m->contended;
            o->held = m->owner != NULL;
            if (m->owner && m->owner != (const void *)&boot_ctx) {
                o->owner_pid = ((const struct task *)m->owner)->pid;
            }
        } else {
            const semaphore_t *s = registry[i].p;
            o->name = s->name;
            o->acquisitions = s->waits;
            o->contended = s->blocked;
            o->held = s->count;
        }
    }
    irq_restore(f);
    return n;
}

void sync_get_stats(struct sync_stats *out) {
    struct sched_stats ss;

    sched_get_stats(&ss);
    out->locks = (uint32_t)reg_count;
    out->slow_warnings = slow_warnings;
    out->atomic_sleep_bugs = ss.atomic_sleep_bugs;
    out->kernel_preemptions = ss.kernel_preemptions;
    out->preempt_enabled = ss.kpreempt;
    strlcpy(out->atomic_task, sched_atomic_bug_task(), sizeof(out->atomic_task));
}
