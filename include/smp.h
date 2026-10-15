#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#define SGI_WORK    0       /* "new work posted": wakes idle cores */
#define SGI_PING    1       /* IPI round-trip test */

/* Start every CPU listed in the device tree through PSCI CPU_ON. */
void     smp_init(void);
unsigned smp_online(void);
unsigned smp_possible(void);

/*
 * Every online CPU increments one shared counter `iters` times.
 * locked = 1: inside a spinlock (expect exactly online * iters).
 * locked = 0: plain read-modify-write (shows lost updates).
 * Returns the final counter value; *elapsed_us gets the wall time.
 */
uint64_t smp_counter_test(int locked, uint64_t iters, uint64_t *elapsed_us);

/* Sends an SGI to each secondary and counts how many answered. */
unsigned smp_ipi_test(void);

void secondary_main(unsigned idx);

#endif
