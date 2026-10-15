#ifndef PL011_H
#define PL011_H

#include <stdint.h>

struct pl011_id {
    uint32_t part;          /* 0x011 for PL011 */
    uint32_t designer;      /* 0x41 ('A') for Arm */
    uint32_t revision;
    int      primecell_ok;  /* component ID reads back 0xB105F00D */
};

/* Reads the PrimeCell peripheral/component ID registers at base + 0xFE0. */
void pl011_read_id(uintptr_t base, struct pl011_id *id);

/* Programs baud rate from the reference clock, 8N1, FIFOs on. */
void pl011_init(uintptr_t base, uint32_t clk_hz, uint32_t baud);

uintptr_t pl011_base(void);
void pl011_putc(char c);

/* Interrupt-driven receive into a ring buffer. */
void pl011_enable_rx_irq(unsigned intid);
int  pl011_getc_nonblock(void);     /* -1 if no data */
char pl011_getc(void);              /* sleeps in WFI until a byte arrives */
uint32_t pl011_rx_overruns(void);

#endif
