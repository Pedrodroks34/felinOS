#ifndef FELINOS_SYNC_H
#define FELINOS_SYNC_H

#include <stdint.h>
#include "sched.h"

/*
 * Kernel synchronisation primitives.
 *
 *  spinlock_t    busy-wait lock. Taking one disables kernel preemption for as
 *                long as it is held. The holder must not sleep and must not
 *                touch memory that can fault in a blocking way (kernel heap,
 *                user memory): protect static data only. spin_lock_irqsave()
 *                also masks interrupts, for data shared with IRQ handlers.
 *  mutex_t       sleeping lock, FIFO hand-off, optionally recursive. The holder
 *                may block and be preempted; a task holding a mutex is not
 *                killable until it lets go, so the lock is never leaked.
 *  semaphore_t   counting semaphore with FIFO wake-up and optional timeout.
 *
 * The three block through the scheduler (sched_wait_on / sched_wake_one), so
 * an idle machine still sits in hlt.
 */

typedef struct spinlock {
    volatile uint32_t locked;
    const char *name;
    const void *owner;
    uint32_t acquisitions;
    uint32_t contended;
    int registered;
} spinlock_t;

typedef struct mutex {
    const void *owner;
    int depth;
    int recursive;
    int waiters;
    const char *name;
    uint32_t acquisitions;
    uint32_t contended;
    int registered;
} mutex_t;

typedef struct semaphore {
    int count;
    int waiters;
    const char *name;
    uint32_t waits;
    uint32_t blocked;
    int registered;
} semaphore_t;

#define SPINLOCK_INIT(n)        { 0, n, 0, 0, 0, 0 }
#define MUTEX_INIT(n)           { 0, 0, 0, 0, n, 0, 0, 0 }
#define MUTEX_INIT_RECURSIVE(n) { 0, 0, 1, 0, n, 0, 0, 0 }
#define SEMAPHORE_INIT(n, v)    { v, 0, n, 0, 0, 0 }

void spin_init(spinlock_t *l, const char *name);
void spin_lock(spinlock_t *l);
int spin_trylock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
uint32_t spin_lock_irqsave(spinlock_t *l);
void spin_unlock_irqrestore(spinlock_t *l, uint32_t flags);

void mutex_init(mutex_t *m, const char *name, int recursive);
void mutex_lock(mutex_t *m);
int mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);
int mutex_locked(const mutex_t *m);
int mutex_owned(const mutex_t *m);

void sem_init(semaphore_t *s, int value, const char *name);
void sem_wait(semaphore_t *s);
int sem_timedwait(semaphore_t *s, uint32_t ticks);   /* 0 = acquired, -1 = timeout */
int sem_trywait(semaphore_t *s);
void sem_post(semaphore_t *s);
int sem_value(const semaphore_t *s);

struct sync_info {
    const char *name;
    int kind;                 /* 0 spinlock, 1 mutex, 2 semaphore */
    uint32_t acquisitions;
    uint32_t contended;
    int held;
    int owner_pid;
};

struct sync_stats {
    uint32_t locks;
    uint32_t slow_warnings;
    uint32_t atomic_sleep_bugs;
    uint32_t kernel_preemptions;
    int preempt_enabled;
    char atomic_task[SCHED_NAME_LEN];
};

int sync_snapshot(struct sync_info *out, int max);
void sync_get_stats(struct sync_stats *out);
void sync_selftest_run(void (*log)(void *ctx, const char *line), void *ctx, int *failures);

#endif
