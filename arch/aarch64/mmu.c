/*
 * Stage-1 EL1 translation: identity map, 4 KiB granule, 39-bit VA.
 *
 * With TCR_EL1.T0SZ = 25 and a 4 KiB granule the walk starts at level 1:
 *   L1 entry -> 1 GiB block, or pointer to an L2 table
 *   L2 entry -> 2 MiB block
 *
 * Memory attributes come from MAIR_EL1 (index in the descriptor's AttrIndx):
 *   0: Device-nGnRnE   1: Device-nGnRE   2: Normal, write-back RW-allocate
 */
#include "mmu.h"
#include "kstring.h"
#include "sysreg.h"

#define VA_BITS         39
#define L1_SHIFT        30
#define L2_SHIFT        21
#define ENTRIES         512
#define MAX_L2_TABLES   8

#define DESC_INVALID    0x0ull
#define DESC_BLOCK      0x1ull
#define DESC_TABLE      0x3ull
#define DESC_TYPE_MASK  0x3ull

#define ATTR_IDX(n)     ((uint64_t)(n) << 2)
#define AP_EL1_RW       (0ull << 6)
#define SH_INNER        (3ull << 8)
#define AF              (1ull << 10)
#define PXN             (1ull << 53)
#define UXN             (1ull << 54)
#define OA_MASK         0x0000fffffffff000ull

#define MAIR_DEVICE_nGnRnE  0x00
#define MAIR_DEVICE_nGnRE   0x04
#define MAIR_NORMAL_WB      0xff
#define MAIR_VALUE      ((MAIR_DEVICE_nGnRnE << 0) | (MAIR_DEVICE_nGnRE << 8) | \
                         ((uint64_t)MAIR_NORMAL_WB << 16))

#define TCR_T0SZ        (64 - VA_BITS)
#define TCR_IRGN0_WB    (1ull << 8)
#define TCR_ORGN0_WB    (1ull << 10)
#define TCR_SH0_INNER   (3ull << 12)
#define TCR_TG0_4K      (0ull << 14)
#define TCR_EPD1        (1ull << 23)    /* no TTBR1 (upper-half) walks */
#define TCR_IPS_SHIFT   32

#define SCTLR_M         (1ull << 0)
#define SCTLR_C         (1ull << 2)
#define SCTLR_I         (1ull << 12)

static uint64_t l1_table[ENTRIES] __attribute__((aligned(4096)));
static uint64_t l2_tables[MAX_L2_TABLES][ENTRIES] __attribute__((aligned(4096)));
static unsigned l2_used;

static uint64_t block_attrs(enum mem_type type)
{
    if (type == MEM_DEVICE)
        return ATTR_IDX(1) | AP_EL1_RW | AF | PXN | UXN;
    return ATTR_IDX(2) | AP_EL1_RW | SH_INNER | AF;
}

/* Return the L2 table behind L1 slot i, creating (or splitting a 1 GiB
 * block into 512 x 2 MiB blocks) as needed. */
static uint64_t *l2_for(unsigned i)
{
    uint64_t e = l1_table[i];

    if ((e & DESC_TYPE_MASK) == DESC_TABLE)
        return (uint64_t *)(uintptr_t)(e & OA_MASK);
    if (l2_used >= MAX_L2_TABLES)
        return 0;

    uint64_t *t = l2_tables[l2_used++];
    if ((e & DESC_TYPE_MASK) == DESC_BLOCK) {
        uint64_t attrs = e & ~(OA_MASK | DESC_TYPE_MASK);
        uint64_t base = (uint64_t)i << L1_SHIFT;
        for (unsigned j = 0; j < ENTRIES; j++)
            t[j] = (base + ((uint64_t)j << L2_SHIFT)) | attrs | DESC_BLOCK;
    } else {
        memset(t, 0, sizeof(l2_tables[0]));
    }
    l1_table[i] = (uint64_t)(uintptr_t)t | DESC_TABLE;
    return t;
}

int mmu_map(uint64_t base, uint64_t size, enum mem_type type)
{
    const uint64_t gib = 1ull << L1_SHIFT, mib2 = 1ull << L2_SHIFT;
    uint64_t addr = base & ~(mib2 - 1);
    uint64_t end = (base + size + mib2 - 1) & ~(mib2 - 1);
    uint64_t attrs = block_attrs(type);

    if (mmu_enabled() || end > (1ull << VA_BITS) || end <= addr)
        return -1;

    while (addr < end) {
        unsigned i = (unsigned)(addr >> L1_SHIFT);
        if ((addr & (gib - 1)) == 0 && end - addr >= gib &&
            (l1_table[i] & DESC_TYPE_MASK) != DESC_TABLE) {
            l1_table[i] = addr | attrs | DESC_BLOCK;
            addr += gib;
            continue;
        }
        uint64_t *l2 = l2_for(i);
        if (!l2)
            return -1;
        l2[(addr >> L2_SHIFT) & (ENTRIES - 1)] = addr | attrs | DESC_BLOCK;
        addr += mib2;
    }
    return 0;
}

void mmu_enable(void)
{
    /* Physical address size the CPU supports -> TCR_EL1.IPS */
    uint64_t parange = read_sysreg(id_aa64mmfr0_el1) & 0xf;
    if (parange > 5)
        parange = 5;

    uint64_t tcr = TCR_T0SZ | TCR_IRGN0_WB | TCR_ORGN0_WB | TCR_SH0_INNER |
                   TCR_TG0_4K | TCR_EPD1 | (parange << TCR_IPS_SHIFT);

    dsb(ish);                       /* table writes visible to the walker */
    write_sysreg(mair_el1, MAIR_VALUE);
    write_sysreg(tcr_el1, tcr);
    write_sysreg(ttbr0_el1, (uint64_t)(uintptr_t)l1_table);
    isb();

    __asm__ volatile("tlbi vmalle1\n\tic iallu" ::: "memory");
    dsb(nsh);
    isb();

    uint64_t sctlr = read_sysreg(sctlr_el1);
    sctlr |= SCTLR_M | SCTLR_C | SCTLR_I;
    write_sysreg(sctlr_el1, sctlr);
    isb();
}

int mmu_enabled(void)
{
    return (read_sysreg(sctlr_el1) & SCTLR_M) != 0;
}

int mmu_translate(uint64_t va, uint64_t *pa, uint8_t *attr, unsigned *fsc)
{
    uint64_t flags = irq_save();        /* PAR_EL1 is not context-switched */
    __asm__ volatile("at s1e1r, %0" :: "r"(va));
    isb();
    uint64_t par = read_sysreg(par_el1);
    irq_restore(flags);

    if (par & 1) {
        if (fsc)
            *fsc = (unsigned)((par >> 1) & 0x3f);
        return -1;
    }
    if (pa)
        *pa = (par & OA_MASK) | (va & 0xfff);
    if (attr)
        *attr = (uint8_t)(par >> 56);
    return 0;
}

unsigned mmu_va_bits(void)     { return VA_BITS; }
unsigned mmu_tables_used(void) { return 1 + l2_used; }
