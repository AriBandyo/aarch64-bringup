/*
 * Ticket-free test-and-set spinlock built on the ARMv8.0 exclusive monitor.
 *
 * lock:   load-acquire exclusive / store exclusive loop. Waiters sleep in WFE;
 *         the unlocking store-release clears their exclusive reservation,
 *         which generates the wake-up event (same scheme as Linux arm64).
 * unlock: store-release of zero.
 *
 * Exclusives only behave coherently across cores on Normal, shareable memory,
 * so every core must have its MMU and caches on before taking a shared lock.
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t v;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *lock)
{
    uint32_t tmp;
    __asm__ volatile(
        "   sevl\n"
        "1: wfe\n"
        "2: ldaxr   %w0, [%1]\n"
        "   cbnz    %w0, 1b\n"
        "   stxr    %w0, %w2, [%1]\n"
        "   cbnz    %w0, 2b\n"
        : "=&r"(tmp)
        : "r"(&lock->v), "r"(1)
        : "memory");
}

static inline int spin_trylock(spinlock_t *lock)
{
    uint32_t tmp, status;
    __asm__ volatile(
        "   ldaxr   %w0, [%2]\n"
        "   cbnz    %w0, 1f\n"
        "   stxr    %w1, %w3, [%2]\n"
        "   b       2f\n"
        "1: clrex\n"
        "   mov     %w1, #1\n"
        "2:\n"
        : "=&r"(tmp), "=&r"(status)
        : "r"(&lock->v), "r"(1)
        : "memory");
    return status == 0;
}

static inline void spin_unlock(spinlock_t *lock)
{
    __asm__ volatile("stlr wzr, [%0]" :: "r"(&lock->v) : "memory");
}

#endif
