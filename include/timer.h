#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* EL1 virtual timer (CNTV_*), periodic tick on the given PPI. */
void     timer_init(unsigned intid, unsigned hz);
uint64_t timer_ticks(void);
unsigned timer_hz(void);

uint64_t uptime_us(void);
void     delay_us(uint64_t us);

#endif
