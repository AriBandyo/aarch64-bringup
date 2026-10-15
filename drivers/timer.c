/*
 * Arm Generic Timer: EL1 virtual timer as a periodic tick.
 *
 * The compare value (CVAL) is advanced by a fixed interval on every tick
 * rather than reloading TVAL, so interrupt latency does not accumulate as
 * drift.
 */
#include "timer.h"
#include "exceptions.h"
#include "gic.h"
#include "sysreg.h"

#define CNTV_CTL_ENABLE     (1u << 0)
#define CNTV_CTL_IMASK      (1u << 1)

static uint64_t interval;
static uint64_t next_cval;
static unsigned tick_hz;
static volatile uint64_t ticks;

static void timer_irq(unsigned intid, void *ctx)
{
    (void)intid;
    (void)ctx;
    next_cval += interval;
    write_sysreg(cntv_cval_el0, next_cval);
    ticks++;
}

void timer_init(unsigned intid, unsigned hz)
{
    tick_hz = hz;
    interval = counter_freq() / hz;
    next_cval = counter_read() + interval;

    irq_register(intid, timer_irq, 0);
    write_sysreg(cntv_cval_el0, next_cval);
    write_sysreg(cntv_ctl_el0, CNTV_CTL_ENABLE);
    isb();
    gic_enable(intid, 0x80);        /* PPI: banked, enabled on this CPU only */
}

uint64_t timer_ticks(void) { return ticks; }
unsigned timer_hz(void)    { return tick_hz; }

uint64_t uptime_us(void)
{
    uint64_t f = counter_freq();
    uint64_t c = counter_read();
    if (!f)
        return 0;
    /* split to avoid overflowing c * 1e6 */
    return (c / f) * 1000000ull + ((c % f) * 1000000ull) / f;
}

void delay_us(uint64_t us)
{
    uint64_t f = counter_freq();
    uint64_t end = counter_read() + (us * f) / 1000000ull;
    while (counter_read() < end)
        ;
}
