#ifndef MMU_H
#define MMU_H

#include <stdint.h>

enum mem_type {
    MEM_DEVICE,     /* Device-nGnRE, execute-never: MMIO */
    MEM_NORMAL,     /* Normal write-back cacheable, inner shareable: RAM */
};

/* Identity-map [base, base + size) at 2 MiB granularity (1 GiB blocks where
 * possible). Later mappings override earlier ones. */
int  mmu_map(uint64_t base, uint64_t size, enum mem_type type);

/* Program MAIR/TCR/TTBR0 and turn on MMU + caches on the calling CPU.
 * Secondary CPUs call this too, sharing the boot CPU's tables. */
void mmu_enable(void);
int  mmu_enabled(void);

/* Hardware translation of va via AT S1E1R. Returns 0 and fills pa/attr,
 * or -1 with *fsc set to the fault status code. */
int  mmu_translate(uint64_t va, uint64_t *pa, uint8_t *attr, unsigned *fsc);

unsigned mmu_va_bits(void);
unsigned mmu_tables_used(void);

#endif
