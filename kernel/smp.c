/*
 * Multi-core bring-up.
 *
 * The boot CPU starts each secondary with PSCI CPU_ON, passing its logical
 * index as the context ID (it arrives in x0 at secondary_entry). Each
 * secondary turns on its MMU with the shared page tables, initialises its
 * banked GIC CPU interface, reports in, then sleeps in WFI.
 *
 * Work is handed out by bumping a generation counter and sending SGI 0
 * (a software-generated interrupt, i.e. an IPI) to wake the sleepers.
 */
#include "smp.h"
#include "gic.h"
#include "exceptions.h"
#include "kprintf.h"
#include "mmu.h"
#include "platform.h"
#include "psci.h"
#include "spinlock.h"
#include "sysreg.h"
#include "timer.h"

extern char secondary_entry[];

static volatile unsigned online_mask = 1;       /* bit per logical CPU */
static volatile unsigned n_online = 1;
static unsigned          n_possible = 1;
static uint64_t          logical_mpidr[MAX_CPUS];

static volatile uint32_t work_gen;
static volatile int      work_locked;
static volatile uint64_t work_iters;
static volatile unsigned work_done;

static spinlock_t        test_lock = SPINLOCK_INIT;
static volatile uint64_t shared_counter;

static volatile uint64_t pings[MAX_CPUS];

#define AFF_MASK    0xff00ffffffUL         /* MPIDR Aff3..Aff0 */

static void sgi_handler(unsigned intid, void *ctx)
{
    (void)ctx;
    if (intid == SGI_PING)
        pings[cpu_id()]++;
    /* SGI_WORK needs no action: waking from WFI is the point. */
}

static void run_work(int locked, uint64_t iters)
{
    if (locked) {
        for (uint64_t i = 0; i < iters; i++) {
            spin_lock(&test_lock);
            shared_counter++;
            spin_unlock(&test_lock);
        }
    } else {
        for (uint64_t i = 0; i < iters; i++)
            shared_counter++;           /* load, add, store: not atomic */
    }
}

void secondary_main(unsigned idx)
{
    mmu_enable();
    gic_cpu_init();

    klog("CPU%u online: MPIDR=0x%lx, EL%u\n", idx,
         read_sysreg(mpidr_el1) & AFF_MASK, current_el());
    /* Report in last, so the boot CPU's log continues after ours. */
    __atomic_add_fetch(&n_online, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&online_mask, 1u << idx, __ATOMIC_SEQ_CST);

    uint32_t seen = work_gen;
    irq_enable();

    for (;;) {
        irq_disable();
        if (work_gen == seen)
            wfi();                      /* woken by SGI_WORK */
        irq_enable();

        if (work_gen != seen) {
            seen = work_gen;
            dmb(ish);                   /* read parameters after the generation */
            run_work(work_locked, work_iters);
            __atomic_add_fetch(&work_done, 1, __ATOMIC_SEQ_CST);
        }
    }
}

static int wait_until(volatile unsigned *val, unsigned target, uint64_t timeout_us)
{
    uint64_t end = uptime_us() + timeout_us;
    while (*val != target)
        if (uptime_us() > end)
            return -1;
    return 0;
}

void smp_init(void)
{
    irq_register(SGI_WORK, sgi_handler, 0);
    irq_register(SGI_PING, sgi_handler, 0);
    logical_mpidr[0] = read_sysreg(mpidr_el1) & AFF_MASK;

    if (plat.ncpus <= 1)
        return;
    if (!psci_available()) {
        klog("SMP: %d CPUs described but no PSCI conduit: staying single-core\n",
             plat.ncpus);
        return;
    }

    unsigned next = 1;
    for (int i = 0; i < plat.ncpus && next < MAX_CPUS; i++) {
        uint64_t mpidr = plat.mpidr[i] & AFF_MASK;
        if (mpidr == logical_mpidr[0])
            continue;                   /* that's us */

        unsigned idx = next++;
        logical_mpidr[idx] = mpidr;
        n_possible = next;

        int64_t r = psci_cpu_on(mpidr, (uint64_t)(uintptr_t)secondary_entry, idx);
        if (r != PSCI_SUCCESS) {
            klog("CPU%u: PSCI CPU_ON(MPIDR=0x%lx) failed: %s\n",
                 idx, mpidr, psci_strerror(r));
            continue;
        }
        /* One core at a time keeps the boot log readable. */
        unsigned want = online_mask | (1u << idx);
        if (wait_until(&online_mask, want, 500000) != 0)
            klog("CPU%u: did not come online within 500 ms\n", idx);
    }
}

unsigned smp_online(void)   { return n_online; }
unsigned smp_possible(void) { return n_possible; }

static uint8_t secondary_gic_mask(void)
{
    uint8_t mask = 0;
    for (unsigned i = 1; i < MAX_CPUS; i++)
        if (online_mask & (1u << i))
            mask |= gic_cpu_mask(i);
    return mask;
}

uint64_t smp_counter_test(int locked, uint64_t iters, uint64_t *elapsed_us)
{
    unsigned others = n_online - 1;

    shared_counter = 0;
    work_done = 0;
    work_locked = locked;
    work_iters = iters;
    dmb(ish);                           /* parameters before the generation */
    work_gen++;

    uint64_t t0 = uptime_us();
    if (others)
        gic_send_sgi(SGI_WORK, secondary_gic_mask());
    run_work(locked, iters);
    if (wait_until(&work_done, others, 10000000) != 0)
        klog("counter test: only %u/%u secondaries finished\n", work_done, others);
    if (elapsed_us)
        *elapsed_us = uptime_us() - t0;
    return shared_counter;
}

unsigned smp_ipi_test(void)
{
    unsigned answered = 0;

    for (unsigned i = 1; i < MAX_CPUS; i++) {
        if (!(online_mask & (1u << i)))
            continue;
        uint64_t before = pings[i];
        gic_send_sgi(SGI_PING, gic_cpu_mask(i));
        uint64_t end = uptime_us() + 100000;
        while (pings[i] == before && uptime_us() < end)
            ;
        if (pings[i] != before)
            answered++;
    }
    return answered;
}
