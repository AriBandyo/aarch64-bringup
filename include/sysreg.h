/* AArch64 system register, barrier and MMIO helpers. */
#ifndef SYSREG_H
#define SYSREG_H

#include <stdint.h>

#define read_sysreg(reg) ({                                   \
        uint64_t __val;                                       \
        __asm__ volatile("mrs %0, " #reg : "=r"(__val));      \
        __val;                                                \
})

#define write_sysreg(reg, val) do {                           \
        uint64_t __val = (uint64_t)(val);                     \
        __asm__ volatile("msr " #reg ", %0" :: "r"(__val));   \
} while (0)

#define dsb(opt)    __asm__ volatile("dsb " #opt ::: "memory")
#define dmb(opt)    __asm__ volatile("dmb " #opt ::: "memory")
#define isb()       __asm__ volatile("isb" ::: "memory")
#define wfi()       __asm__ volatile("wfi" ::: "memory")
#define wfe()       __asm__ volatile("wfe" ::: "memory")
#define sev()       __asm__ volatile("sev" ::: "memory")

/* PSTATE.I (IRQ mask) control */
static inline void irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }

static inline uint64_t irq_save(void)
{
    uint64_t flags = read_sysreg(daif);
    irq_disable();
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    write_sysreg(daif, flags);
}

static inline unsigned current_el(void)
{
    return (unsigned)((read_sysreg(CurrentEL) >> 2) & 3);
}

/*
 * Logical CPU index (0 = boot CPU). start.S stores it in TPIDR_EL1, the
 * per-CPU software thread ID register, so it does not depend on how a given
 * SoC encodes MPIDR affinity fields.
 */
static inline unsigned cpu_id(void)
{
    return (unsigned)read_sysreg(tpidr_el1);
}

/* MMIO accessors. Device registers must be accessed with exactly-sized,
 * volatile loads/stores so the compiler never merges or splits them. */
static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint8_t mmio_read8(uintptr_t addr)
{
    return *(volatile uint8_t *)addr;
}

static inline void mmio_write8(uintptr_t addr, uint8_t val)
{
    *(volatile uint8_t *)addr = val;
}

/* Generic timer: virtual counter */
static inline uint64_t counter_read(void)
{
    isb();
    return read_sysreg(cntvct_el0);
}

static inline uint64_t counter_freq(void)
{
    return read_sysreg(cntfrq_el0);
}

#endif
