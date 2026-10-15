/*
 * Arm PrimeCell PL011 UART driver.
 *
 * Register offsets and bit fields from the PL011 Technical Reference Manual
 * (Arm DDI 0183). Transmit is polled; receive is interrupt-driven into a
 * single-producer (IRQ) / single-consumer (shell) ring buffer.
 */
#include "pl011.h"
#include "exceptions.h"
#include "gic.h"
#include "sysreg.h"

#define UARTDR      0x000
#define UARTFR      0x018
#define UARTIBRD    0x024
#define UARTFBRD    0x028
#define UARTLCR_H   0x02c
#define UARTCR      0x030
#define UARTIFLS    0x034
#define UARTIMSC    0x038
#define UARTMIS     0x040
#define UARTICR     0x044
#define UARTPID0    0xfe0
#define UARTCID0    0xff0

#define FR_BUSY     (1u << 3)
#define FR_RXFE     (1u << 4)
#define FR_TXFF     (1u << 5)

#define LCR_FEN     (1u << 4)
#define LCR_WLEN8   (3u << 5)

#define CR_UARTEN   (1u << 0)
#define CR_TXE      (1u << 8)
#define CR_RXE      (1u << 9)

#define INT_RX      (1u << 4)       /* receive */
#define INT_RT      (1u << 6)       /* receive timeout (FIFO non-empty, idle) */
#define INT_OE      (1u << 10)      /* overrun */
#define INT_ALL     0x7ff

#define DR_OE       (1u << 11)

#define RX_BUF_SIZE 256             /* power of two */

static uintptr_t uart_base;
static int       rx_irq_enabled;

static volatile char     rx_buf[RX_BUF_SIZE];
static volatile uint32_t rx_head;   /* written by IRQ handler */
static volatile uint32_t rx_tail;   /* written by consumer */
static volatile uint32_t rx_overruns;

static inline uint32_t rd(uint32_t off)          { return mmio_read32(uart_base + off); }
static inline void     wr(uint32_t off, uint32_t v) { mmio_write32(uart_base + off, v); }

void pl011_read_id(uintptr_t base, struct pl011_id *id)
{
    uint32_t pid[4], cid = 0;

    for (int i = 0; i < 4; i++) {
        pid[i] = mmio_read32(base + UARTPID0 + 4u * (unsigned)i) & 0xff;
        cid |= (mmio_read32(base + UARTCID0 + 4u * (unsigned)i) & 0xff) << (8 * i);
    }
    id->part = pid[0] | ((pid[1] & 0xf) << 8);
    id->designer = (pid[1] >> 4) | ((pid[2] & 0xf) << 4);
    id->revision = pid[2] >> 4;
    id->primecell_ok = (cid == 0xb105f00d);
}

void pl011_init(uintptr_t base, uint32_t clk_hz, uint32_t baud)
{
    uart_base = base;

    wr(UARTCR, 0);                          /* disable while reprogramming */
    while (rd(UARTFR) & FR_BUSY)
        ;
    wr(UARTLCR_H, 0);                       /* flush FIFOs */

    /*
     * Baud divisor = clk / (16 * baud), as a 16.6 fixed-point value:
     * IBRD = integer part, FBRD = round(fraction * 64).
     * Computing 4 * clk / baud gives the divisor already scaled by 64.
     */
    uint32_t div64 = (uint32_t)((4ull * clk_hz + baud / 2) / baud);
    wr(UARTIBRD, div64 >> 6);
    wr(UARTFBRD, div64 & 0x3f);
    wr(UARTLCR_H, LCR_WLEN8 | LCR_FEN);     /* 8N1; must follow the divisors */

    wr(UARTIFLS, 0);                        /* RX interrupt at 1/8 full */
    wr(UARTIMSC, 0);
    wr(UARTICR, INT_ALL);
    wr(UARTCR, CR_UARTEN | CR_TXE | CR_RXE);
}

uintptr_t pl011_base(void)
{
    return uart_base;
}

void pl011_putc(char c)
{
    while (rd(UARTFR) & FR_TXFF)
        ;
    wr(UARTDR, (uint32_t)(unsigned char)c);
}

static void pl011_irq(unsigned intid, void *ctx)
{
    (void)intid;
    (void)ctx;

    /*
     * Acknowledge BEFORE draining. If the clear came after the loop, a byte
     * landing between the last FIFO-empty check and the ICR write would have
     * its interrupt wiped while it sits in the FIFO; QEMU's PL011 raises RX
     * only on the empty -> non-empty transition, so input would stall.
     * Clearing first means any byte that arrives after the drain re-raises it.
     */
    wr(UARTICR, INT_RX | INT_RT | INT_OE);

    while (!(rd(UARTFR) & FR_RXFE)) {
        uint32_t dr = rd(UARTDR);
        uint32_t head = rx_head;
        if (dr & DR_OE)
            rx_overruns++;
        if (head - rx_tail < RX_BUF_SIZE) {
            rx_buf[head & (RX_BUF_SIZE - 1)] = (char)(dr & 0xff);
            dmb(ish);                       /* data before index */
            rx_head = head + 1;
        } else {
            rx_overruns++;                  /* software buffer full */
        }
    }
}

void pl011_enable_rx_irq(unsigned intid)
{
    irq_register(intid, pl011_irq, 0);
    gic_set_trigger(intid, 0);              /* level-sensitive */
    gic_route_spi(intid, 0);                /* to the boot CPU */
    gic_enable(intid, 0xa0);
    wr(UARTIMSC, INT_RX | INT_RT | INT_OE);
    rx_irq_enabled = 1;
}

int pl011_getc_nonblock(void)
{
    if (!rx_irq_enabled) {                  /* polled fallback (no GIC) */
        if (rd(UARTFR) & FR_RXFE)
            return -1;
        return (int)(rd(UARTDR) & 0xff);
    }

    uint32_t tail = rx_tail;
    if (tail == rx_head)
        return -1;
    dmb(ish);                               /* index before data */
    char c = rx_buf[tail & (RX_BUF_SIZE - 1)];
    rx_tail = tail + 1;
    return (unsigned char)c;
}

char pl011_getc(void)
{
    if (!rx_irq_enabled) {
        int c;
        while ((c = pl011_getc_nonblock()) < 0)
            ;
        return (char)c;
    }

    for (;;) {
        /*
         * Mask IRQs, check, then WFI: a pending interrupt still wakes WFI
         * while masked, so no byte can slip in between the check and the
         * sleep. Unmasking afterwards lets the handler run.
         */
        irq_disable();
        int c = pl011_getc_nonblock();
        if (c < 0)
            wfi();
        irq_enable();
        if (c >= 0)
            return (char)c;
    }
}

uint32_t pl011_rx_overruns(void)
{
    return rx_overruns;
}
