/*
 * Hardware description discovered at boot from the device tree, with the
 * board.h defaults filling in anything the device tree does not provide.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include "board.h"

#define MAX_RAM_REGIONS 4

enum psci_conduit { PSCI_NONE = 0, PSCI_HVC, PSCI_SMC };

struct mem_region {
    uint64_t base;
    uint64_t size;
};

struct platform {
    unsigned  boot_el;
    int       dt_found;
    uintptr_t dtb;
    char      model[64];

    uintptr_t uart_base;
    uint32_t  uart_clk;
    unsigned  uart_irq;

    int       gic_found;
    int       gic_unsupported;      /* e.g. GICv3: present but no driver */
    uintptr_t gicd, gicc;
    unsigned  timer_irq;            /* EL1 virtual timer PPI */

    struct mem_region ram[MAX_RAM_REGIONS];
    int       nram;

    enum psci_conduit psci;

    int       ncpus;
    uint64_t  mpidr[MAX_CPUS];
};

extern struct platform plat;

/* Parse the DTB (or fall back to board defaults) into `plat`. */
void platform_discover(uintptr_t dtb, unsigned boot_el);

#endif
