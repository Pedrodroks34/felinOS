#ifndef FELINOS_ATOMIC_H
#define FELINOS_ATOMIC_H

#include <stdint.h>

/*
 * These use real LOCK-prefixed x86-64 instructions, not irq_save()/
 * irq_restore(): disabling local interrupts only keeps this CPU's own IRQ
 * handlers from interleaving with the read-modify-write -- it does nothing
 * to stop a SECOND CPU from touching the same atomic_t at the same time.
 * On SMP (the whole point of this header, see ROADMAP.md Fase 2) that is
 * a straight data race. The x86-64 LOCK prefix locks the memory bus/cache
 * line for the duration of the instruction, which is what actually makes
 * these safe across cores, and is cheaper than an IRQ round-trip besides.
 */

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
    __asm__ volatile("lock addl %1, %0"
                      : "+m"(v->counter)
                      : "ir"(i)
                      : "memory");
}

/* Atomic subtract */
static inline void atomic_sub(int i, atomic_t *v) {
    __asm__ volatile("lock subl %1, %0"
                      : "+m"(v->counter)
                      : "ir"(i)
                      : "memory");
}

/* Atomic increment */
static inline void atomic_inc(atomic_t *v) {
    __asm__ volatile("lock incl %0" : "+m"(v->counter) :: "memory");
}

/* Atomic decrement */
static inline void atomic_dec(atomic_t *v) {
    __asm__ volatile("lock decl %0" : "+m"(v->counter) :: "memory");
}

/* Atomic decrement and test if zero */
static inline int atomic_dec_and_test(atomic_t *v) {
    uint8_t zero;
    __asm__ volatile("lock decl %0; sete %1"
                      : "+m"(v->counter), "=qm"(zero)
                      :: "memory");
    return zero;
}

/* Atomic increment and test if zero */
static inline int atomic_inc_and_test(atomic_t *v) {
    uint8_t zero;
    __asm__ volatile("lock incl %0; sete %1"
                      : "+m"(v->counter), "=qm"(zero)
                      :: "memory");
    return zero;
}

/* Atomic add, then test if the result is negative */
static inline int atomic_add_negative(int i, atomic_t *v) {
    uint8_t neg;
    __asm__ volatile("lock addl %2, %0; sets %1"
                      : "+m"(v->counter), "=qm"(neg)
                      : "ir"(i)
                      : "memory");
    return neg;
}

/* Atomic compare and swap: on mismatch, *v is left unchanged and this
 * returns 0 -- same success/fail convention as the original irq_save()
 * version, but note that unlike Linux's atomic_cmpxchg() this does NOT
 * return the value that was actually in *v. Callers that need that value
 * (e.g. to retry with it) should use atomic_cmpxchg_val() instead. */
static inline int atomic_cmpxchg(atomic_t *v, int old, int new) {
    uint8_t success;
    __asm__ volatile("lock cmpxchgl %3, %0; sete %1"
                      : "+m"(v->counter), "=q"(success), "+a"(old)
                      : "r"(new)
                      : "memory");
    return success;
}

/* Same compare-and-swap, but returns the value observed in *v (Linux's
 * usual atomic_cmpxchg() semantics): equal to `old` on success. */
static inline int atomic_cmpxchg_val(atomic_t *v, int old, int new) {
    __asm__ volatile("lock cmpxchgl %2, %0"
                      : "+m"(v->counter), "+a"(old)
                      : "r"(new)
                      : "memory");
    return old;
}

/* Atomic exchange */
static inline int atomic_xchg(atomic_t *v, int new) {
    /* XCHG against memory is implicitly LOCKed on x86 -- no "lock" prefix
     * needed (and the assembler would reject one). */
    __asm__ volatile("xchgl %0, %1"
                      : "+m"(v->counter), "+r"(new)
                      :: "memory");
    return new;
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
    __asm__ volatile("lock addq %1, %0"
                      : "+m"(v->counter)
                      : "ir"(i)
                      : "memory");
}

static inline void atomic64_inc(atomic64_t *v) {
    __asm__ volatile("lock incq %0" : "+m"(v->counter) :: "memory");
}

static inline void atomic64_dec(atomic64_t *v) {
    __asm__ volatile("lock decq %0" : "+m"(v->counter) :: "memory");
}

static inline int atomic64_dec_and_test(atomic64_t *v) {
    uint8_t zero;
    __asm__ volatile("lock decq %0; sete %1"
                      : "+m"(v->counter), "=qm"(zero)
                      :: "memory");
    return zero;
}

static inline int64_t atomic64_xchg(atomic64_t *v, int64_t new) {
    __asm__ volatile("xchgq %0, %1"
                      : "+m"(v->counter), "+r"(new)
                      :: "memory");
    return new;
}

/* Returns the value observed in *v (see atomic_cmpxchg_val() above for why
 * this differs from atomic_cmpxchg()'s 0/1 success flag). */
static inline int64_t atomic64_cmpxchg(atomic64_t *v, int64_t old, int64_t new) {
    __asm__ volatile("lock cmpxchgq %2, %0"
                      : "+m"(v->counter), "+a"(old)
                      : "r"(new)
                      : "memory");
    return old;
}

#endif /* FELINOS_ATOMIC_H */
