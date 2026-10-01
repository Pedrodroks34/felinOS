#ifndef FELINOS_ATOMIC_H
#define FELINOS_ATOMIC_H

#include <stdint.h>
#include "sync.h"  /* for irq_save/irq_restore */

/* Atomic integer type */
typedef struct {
    volatile int counter;
} atomic_t;

#define ATOMIC_INIT(i) { (i) }

/* Initialize atomic variable */
static inline void atomic_set(atomic_t *v, int i) {
    v->counter = i;
}

/* Get current value */
static inline int atomic_read(const atomic_t *v) {
    return v->counter;
}

/* Atomic add */
static inline void atomic_add(int i, atomic_t *v) {
    uint32_t flags = irq_save();
    v->counter += i;
    irq_restore(flags);
}

/* Atomic subtract */
static inline void atomic_sub(int i, atomic_t *v) {
    uint32_t flags = irq_save();
    v->counter -= i;
    irq_restore(flags);
}

/* Atomic increment */
static inline void atomic_inc(atomic_t *v) {
    atomic_add(1, v);
}

/* Atomic decrement */
static inline void atomic_dec(atomic_t *v) {
    atomic_sub(1, v);
}

/* Atomic decrement and test if zero */
static inline int atomic_dec_and_test(atomic_t *v) {
    uint32_t flags = irq_save();
    v->counter--;
    int result = (v->counter == 0);
    irq_restore(flags);
    return result;
}

/* Atomic increment and test if zero */
static inline int atomic_inc_and_test(atomic_t *v) {
    uint32_t flags = irq_save();
    v->counter++;
    int result = (v->counter == 0);
    irq_restore(flags);
    return result;
}

/* Atomic add negative and test if negative */
static inline int atomic_add_negative(int i, atomic_t *v) {
    uint32_t flags = irq_save();
    v->counter += i;
    int result = (v->counter < 0);
    irq_restore(flags);
    return result;
}

/* Atomic compare and swap */
static inline int atomic_cmpxchg(atomic_t *v, int old, int new) {
    uint32_t flags = irq_save();
    int ret = (v->counter == old);
    if (ret) v->counter = new;
    irq_restore(flags);
    return ret;
}

/* Atomic exchange */
static inline int atomic_xchg(atomic_t *v, int new) {
    uint32_t flags = irq_save();
    int old = v->counter;
    v->counter = new;
    irq_restore(flags);
    return old;
}

/* 64-bit atomic operations */
typedef struct {
    volatile int64_t counter;
} atomic64_t;

#define ATOMIC64_INIT(i) { (i) }

static inline void atomic64_set(atomic64_t *v, int64_t i) {
    v->counter = i;
}

static inline int64_t atomic64_read(const atomic64_t *v) {
    return v->counter;
}

static inline void atomic64_add(int64_t i, atomic64_t *v) {
    uint32_t flags = irq_save();
    v->counter += i;
    irq_restore(flags);
}

static inline void atomic64_inc(atomic64_t *v) {
    atomic64_add(1, v);
}

static inline void atomic64_dec(atomic64_t *v) {
    uint32_t flags = irq_save();
    v->counter--;
    irq_restore(flags);
}

static inline int atomic64_dec_and_test(atomic64_t *v) {
    uint32_t flags = irq_save();
    v->counter--;
    int result = (v->counter == 0);
    irq_restore(flags);
    return result;
}

static inline int64_t atomic64_xchg(atomic64_t *v, int64_t new) {
    uint32_t flags = irq_save();
    int64_t old = v->counter;
    v->counter = new;
    irq_restore(flags);
    return old;
}

static inline int atomic64_cmpxchg(atomic64_t *v, int64_t old, int64_t new) {
    uint32_t flags = irq_save();
    int ret = (v->counter == old);
    if (ret) v->counter = new;
    irq_restore(flags);
    return ret;
}

#endif /* FELINOS_ATOMIC_H */