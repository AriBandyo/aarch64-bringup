/*
 * Arm Generic Interrupt Controller v2 (GIC-400 / Cortex-A15 GIC) driver.
 *
 * Register map from the GICv2 Architecture Specification (Arm IHI 0048B).
 * The distributor (GICD) is shared; SGI/PPI state (IDs 0-31) and the CPU
 * interface (GICC) are banked per CPU, so gic_cpu_init() runs on each core.
 */
#include "gic.h"
#include "board.h"
#include "sysreg.h"

#define GICD_CTLR           0x000
#define GICD_TYPER          0x004
#define GICD_IIDR           0x008
#define GICD_ISENABLER      0x100
#define GICD_ICENABLER      0x180
#define GICD_ICPENDR        0x280
#define GICD_IPRIORITYR     0x400
#define GICD_ITARGETSR      0x800
#define GICD_ICFGR          0xc00
#define GICD_SGIR           0xf00

#define GICC_CTLR           0x000
#define GICC_PMR            0x004
#define GICC_BPR            0x008
#define GICC_IAR            0x00c
#define GICC_EOIR           0x010
#define GICC_IIDR           0x0fc

#define DEFAULT_PRIORITY    0xa0
#define PRIORITY_MASK       0xf0    /* allow priorities numerically < 0xf0 */

static uintptr_t gicd, gicc;
static unsigned  num_lines;
static uint8_t   cpu_mask[MAX_CPUS];

void gic_init(uintptr_t dist_base, uintptr_t cpu_base)
{
    gicd = dist_base;
    gicc = cpu_base;

    mmio_write32(gicd + GICD_CTLR, 0);

    num_lines = 32 * ((mmio_read32(gicd + GICD_TYPER) & 0x1f) + 1);
    if (num_lines > GIC_MAX_IRQ)
        num_lines = GIC_MAX_IRQ;

    /* SPIs: disabled, not pending, default priority, level, no target. */
    for (unsigned i = 32; i < num_lines; i += 32) {
        mmio_write32(gicd + GICD_ICENABLER + i / 8, 0xffffffff);
        mmio_write32(gicd + GICD_ICPENDR + i / 8, 0xffffffff);
    }
    for (unsigned i = 32; i < num_lines; i += 4) {
        mmio_write32(gicd + GICD_IPRIORITYR + i, 0xa0a0a0a0);
        mmio_write32(gicd + GICD_ITARGETSR + i, 0);
    }
    for (unsigned i = 32; i < num_lines; i += 16)
        mmio_write32(gicd + GICD_ICFGR + i / 4, 0);

    mmio_write32(gicd + GICD_CTLR, 1);
    gic_cpu_init();
}

void gic_cpu_init(void)
{
    unsigned cpu = cpu_id();

    /* Banked SGI/PPI state: PPIs off, SGIs on, default priority. */
    mmio_write32(gicd + GICD_ICENABLER, 0xffff0000);
    mmio_write32(gicd + GICD_ISENABLER, 0x0000ffff);
    for (unsigned i = 0; i < 32; i += 4)
        mmio_write32(gicd + GICD_IPRIORITYR + i, 0xa0a0a0a0);

    /*
     * ITARGETSR0-7 are read-only and return the reading CPU's own interface
     * bit. Cache it so SPIs and SGIs can be aimed at a logical CPU without
     * assuming interface number == CPU number.
     */
    if (cpu < MAX_CPUS)
        cpu_mask[cpu] = mmio_read8(gicd + GICD_ITARGETSR);

    mmio_write32(gicc + GICC_PMR, PRIORITY_MASK);
    mmio_write32(gicc + GICC_BPR, 0);
    mmio_write32(gicc + GICC_CTLR, 1);
}

void gic_enable(unsigned intid, uint8_t priority)
{
    mmio_write8(gicd + GICD_IPRIORITYR + intid, priority);
    mmio_write32(gicd + GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

void gic_disable(unsigned intid)
{
    mmio_write32(gicd + GICD_ICENABLER + (intid / 32) * 4, 1u << (intid % 32));
}

void gic_set_trigger(unsigned intid, int edge)
{
    if (intid < 16)
        return;                             /* SGIs are always edge */
    uintptr_t reg = gicd + GICD_ICFGR + (intid / 16) * 4;
    unsigned shift = (intid % 16) * 2 + 1;
    uint32_t v = mmio_read32(reg);
    v = edge ? (v | (1u << shift)) : (v & ~(1u << shift));
    mmio_write32(reg, v);
}

void gic_route_spi(unsigned intid, unsigned cpu)
{
    if (intid < 32 || cpu >= MAX_CPUS)
        return;
    mmio_write8(gicd + GICD_ITARGETSR + intid, cpu_mask[cpu]);
}

void gic_send_sgi(unsigned sgi, uint8_t mask)
{
    dsb(ish);                               /* publish data before the IPI */
    mmio_write32(gicd + GICD_SGIR, ((uint32_t)mask << 16) | (sgi & 0xf));
}

uint32_t gic_ack(void)
{
    return mmio_read32(gicc + GICC_IAR);
}

void gic_eoi(uint32_t iar)
{
    mmio_write32(gicc + GICC_EOIR, iar);
}

unsigned gic_num_lines(void)  { return num_lines; }
uint32_t gic_dist_iidr(void)  { return mmio_read32(gicd + GICD_IIDR); }
uint32_t gic_cpu_iidr(void)   { return mmio_read32(gicc + GICC_IIDR); }

uint8_t gic_cpu_mask(unsigned cpu)
{
    return cpu < MAX_CPUS ? cpu_mask[cpu] : 0;
}
