#ifndef GIC_H
#define GIC_H

#include <stdint.h>

#define GIC_SPURIOUS    1020    /* IDs >= 1020 are special/spurious */
#define GIC_MAX_IRQ     1020

/* Interrupt ID ranges: SGI 0-15, PPI 16-31, SPI 32-1019 */
#define GIC_PPI(n)      (16 + (n))
#define GIC_SPI(n)      (32 + (n))

void     gic_init(uintptr_t gicd, uintptr_t gicc);  /* distributor + boot CPU */
void     gic_cpu_init(void);                        /* each CPU's banked state */

void     gic_enable(unsigned intid, uint8_t priority);
void     gic_disable(unsigned intid);
void     gic_set_trigger(unsigned intid, int edge);
void     gic_route_spi(unsigned intid, unsigned cpu);
void     gic_send_sgi(unsigned sgi, uint8_t cpu_mask);

uint32_t gic_ack(void);                 /* read GICC_IAR */
void     gic_eoi(uint32_t iar);         /* write GICC_EOIR */

unsigned gic_num_lines(void);
uint32_t gic_dist_iidr(void);
uint32_t gic_cpu_iidr(void);
uint8_t  gic_cpu_mask(unsigned cpu);    /* CPU interface bit for logical CPU */

#endif
