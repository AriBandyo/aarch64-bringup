#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdint.h>

/* Layout must match save_frame in vectors.S */
struct trap_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t pad;
};

_Static_assert(sizeof(struct trap_frame) == 272, "trap_frame layout");

enum vector_kind {
    VEC_SYNC    = 0,
    VEC_IRQ     = 1,
    VEC_FIQ     = 2,
    VEC_SERROR  = 3,
    VEC_SP0     = 0x10,
    VEC_LOWER64 = 0x20,
    VEC_LOWER32 = 0x30,
};

typedef void (*irq_handler_fn)(unsigned intid, void *ctx);

void irq_register(unsigned intid, irq_handler_fn fn, void *ctx);
uint64_t irq_count(unsigned intid);

/* Probe an address: returns 0 and the value, or -1 if the access aborted.
 * The abort is decoded and logged, then execution resumes after the load. */
int  safe_read32(uintptr_t addr, uint32_t *out);
int  safe_write32(uintptr_t addr, uint32_t val);

/* Deliberately raise exceptions and recover (self-test / demo). */
void trigger_svc(void);
void trigger_undef(void);

/* Quiet mode: decode recovered faults without printing (used by md). */
void exceptions_set_verbose(int verbose);

const char *esr_class_name(unsigned ec);

void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#endif
