/*
 * Exception handling: synchronous exception decode, IRQ dispatch, panic.
 *
 * ESR_EL1 layout (Arm ARM D17.2.37):
 *   [31:26] EC   exception class
 *   [25]    IL   instruction length (1 = 32-bit)
 *   [24:0]  ISS  class-specific syndrome
 */
#include <stdarg.h>
#include "exceptions.h"
#include "board.h"
#include "gic.h"
#include "kprintf.h"
#include "sysreg.h"

#define EC_UNKNOWN      0x00
#define EC_WFX          0x01
#define EC_FP           0x07
#define EC_ILLEGAL      0x0e
#define EC_SVC64        0x15
#define EC_HVC64        0x16
#define EC_SMC64        0x17
#define EC_SYSREG       0x18
#define EC_IABT_LOW     0x20
#define EC_IABT_CUR     0x21
#define EC_PC_ALIGN     0x22
#define EC_DABT_LOW     0x24
#define EC_DABT_CUR     0x25
#define EC_SP_ALIGN     0x26
#define EC_SERROR       0x2f
#define EC_BRK64        0x3c

static irq_handler_fn irq_handlers[GIC_MAX_IRQ];
static void          *irq_ctx[GIC_MAX_IRQ];
static uint64_t       irq_counts[GIC_MAX_IRQ];

/* Per-CPU "an abort here is expected" flags for safe_read32() and friends. */
static volatile int fault_expected[MAX_CPUS];
static volatile int fault_hit[MAX_CPUS];
static int verbose = 1;

const char *esr_class_name(unsigned ec)
{
    switch (ec) {
    case EC_UNKNOWN:    return "Undefined instruction";
    case EC_WFX:        return "Trapped WFI/WFE";
    case EC_FP:         return "FP/SIMD access trap";
    case EC_ILLEGAL:    return "Illegal execution state";
    case EC_SVC64:      return "SVC (AArch64)";
    case EC_HVC64:      return "HVC (AArch64)";
    case EC_SMC64:      return "SMC (AArch64)";
    case EC_SYSREG:     return "Trapped MSR/MRS";
    case EC_IABT_LOW:   return "Instruction abort (lower EL)";
    case EC_IABT_CUR:   return "Instruction abort (current EL)";
    case EC_PC_ALIGN:   return "PC alignment fault";
    case EC_DABT_LOW:   return "Data abort (lower EL)";
    case EC_DABT_CUR:   return "Data abort (current EL)";
    case EC_SP_ALIGN:   return "SP alignment fault";
    case EC_SERROR:     return "SError";
    case EC_BRK64:      return "BRK (AArch64)";
    default:            return "Unrecognized exception class";
    }
}

/* Fault status code (DFSC/IFSC, ISS[5:0]) for instruction and data aborts. */
static void describe_fsc(unsigned fsc, char *buf, unsigned size)
{
    unsigned level = fsc & 3;
    switch (fsc >> 2) {
    case 0x0: ksnprintf(buf, size, "address size fault, level %u", level); return;
    case 0x1: ksnprintf(buf, size, "translation fault, level %u", level); return;
    case 0x2: ksnprintf(buf, size, "access flag fault, level %u", level); return;
    case 0x3: ksnprintf(buf, size, "permission fault, level %u", level); return;
    case 0x5: ksnprintf(buf, size, "external abort on table walk, level %u", level); return;
    }
    switch (fsc) {
    case 0x10: ksnprintf(buf, size, "synchronous external abort"); return;
    case 0x21: ksnprintf(buf, size, "alignment fault"); return;
    case 0x30: ksnprintf(buf, size, "TLB conflict abort"); return;
    default:   ksnprintf(buf, size, "fault status 0x%02x", fsc); return;
    }
}

static void describe_exception(uint64_t esr, uint64_t far, uint64_t elr)
{
    unsigned ec = (unsigned)(esr >> 26);
    unsigned iss = (unsigned)(esr & 0x1ffffff);

    kprintf_unlocked("  %s (EC=0x%02x, ESR=0x%08lx)\n", esr_class_name(ec), ec, esr);

    if (ec == EC_DABT_CUR || ec == EC_DABT_LOW ||
        ec == EC_IABT_CUR || ec == EC_IABT_LOW) {
        char fsc[64];
        describe_fsc(iss & 0x3f, fsc, sizeof(fsc));
        kprintf_unlocked("  cause: %s\n", fsc);
        if (ec == EC_DABT_CUR || ec == EC_DABT_LOW)
            kprintf_unlocked("  access: %s\n", (iss & (1u << 6)) ? "write" : "read");
        if (iss & (1u << 10))
            kprintf_unlocked("  FAR: not valid (FnV set)\n");
        else
            kprintf_unlocked("  FAR: 0x%016lx\n", far);
    }
    kprintf_unlocked("  ELR: 0x%016lx\n", elr);
}

static void dump_frame(const struct trap_frame *f)
{
    for (int i = 0; i < 31; i++)
        kprintf_unlocked("  x%-2d %016lx%s", i, f->x[i], (i % 4 == 3) ? "\n" : "");
    kprintf_unlocked("\n  ELR %016lx  SPSR %016lx  SP %016lx\n",
                     f->elr, f->spsr, (uint64_t)(uintptr_t)(f + 1));
}

static const char *vector_name(unsigned kind)
{
    static const char *const types[] = { "Sync", "IRQ", "FIQ", "SError" };
    static const char *const origin[] = {
        "current EL, SP_ELx", "current EL, SP_EL0", "lower EL, AArch64", "lower EL, AArch32",
    };
    static char buf[48];
    ksnprintf(buf, sizeof(buf), "%s from %s", types[kind & 3], origin[(kind >> 4) & 3]);
    return buf;
}

static void __attribute__((noreturn)) halt(void)
{
    irq_disable();
    for (;;)
        wfe();
}

void handle_sync(struct trap_frame *f, unsigned kind)
{
    uint64_t esr = read_sysreg(esr_el1);
    uint64_t far = read_sysreg(far_el1);
    unsigned ec = (unsigned)(esr >> 26);
    unsigned cpu = cpu_id();

    if (ec == EC_SVC64) {
        /* ELR already points past the SVC: just return. */
        kprintf_unlocked("  SVC #0x%lx taken at EL1 (return to 0x%lx)\n",
                         esr & 0xffff, f->elr);
        return;
    }

    if (cpu < MAX_CPUS && fault_expected[cpu]) {
        fault_hit[cpu] = 1;
        if (verbose) {
            kprintf_unlocked("Synchronous exception on CPU%u:\n", cpu);
            describe_exception(esr, far, f->elr);
            kprintf_unlocked("  recovered: skipping faulting instruction\n");
        }
        f->elr += 4;            /* all A64 instructions are 4 bytes */
        return;
    }

    kprintf_unlocked("\n*** Unhandled exception on CPU%u: %s ***\n", cpu, vector_name(kind));
    describe_exception(esr, far, f->elr);
    dump_frame(f);
    kprintf_unlocked("*** SYSTEM HALTED ***\n");
    halt();
}

void handle_irq(struct trap_frame *f, unsigned kind)
{
    (void)f;
    (void)kind;

    uint32_t iar = gic_ack();
    unsigned intid = iar & 0x3ff;

    if (intid >= GIC_SPURIOUS)
        return;                 /* spurious: no EOI */

    __atomic_fetch_add(&irq_counts[intid], 1, __ATOMIC_RELAXED);
    if (irq_handlers[intid])
        irq_handlers[intid](intid, irq_ctx[intid]);
    else
        kprintf_unlocked("unhandled IRQ %u on CPU%u\n", intid, cpu_id());
    gic_eoi(iar);
}

void handle_unexpected(struct trap_frame *f, unsigned kind)
{
    kprintf_unlocked("\n*** Unexpected exception on CPU%u: %s ***\n", cpu_id(), vector_name(kind));
    describe_exception(read_sysreg(esr_el1), read_sysreg(far_el1), f->elr);
    dump_frame(f);
    kprintf_unlocked("*** SYSTEM HALTED ***\n");
    halt();
}

void irq_register(unsigned intid, irq_handler_fn fn, void *ctx)
{
    if (intid < GIC_MAX_IRQ) {
        irq_ctx[intid] = ctx;
        irq_handlers[intid] = fn;
    }
}

uint64_t irq_count(unsigned intid)
{
    return intid < GIC_MAX_IRQ ? irq_counts[intid] : 0;
}

void exceptions_set_verbose(int v)
{
    verbose = v;
}

/*
 * The access is a single A64 instruction inside the asm block, so when it
 * aborts the handler can resume at ELR + 4 and land right after it.
 */
int safe_read32(uintptr_t addr, uint32_t *out)
{
    unsigned cpu = cpu_id();
    uint32_t v = 0;

    fault_hit[cpu] = 0;
    fault_expected[cpu] = 1;
    __asm__ volatile("ldr %w0, [%1]" : "+r"(v) : "r"(addr) : "memory");
    fault_expected[cpu] = 0;

    if (fault_hit[cpu])
        return -1;
    *out = v;
    return 0;
}

int safe_write32(uintptr_t addr, uint32_t val)
{
    unsigned cpu = cpu_id();

    fault_hit[cpu] = 0;
    fault_expected[cpu] = 1;
    __asm__ volatile("str %w0, [%1]" :: "r"(val), "r"(addr) : "memory");
    fault_expected[cpu] = 0;

    return fault_hit[cpu] ? -1 : 0;
}

void trigger_svc(void)
{
    __asm__ volatile("svc #0x42" ::: "memory");
}

void trigger_undef(void)
{
    unsigned cpu = cpu_id();
    fault_hit[cpu] = 0;
    fault_expected[cpu] = 1;
    __asm__ volatile("udf #0" ::: "memory");
    fault_expected[cpu] = 0;
}

void panic(const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    kprintf_unlocked("\n*** PANIC on CPU%u: %s ***\n", cpu_id(), msg);
    halt();
}
