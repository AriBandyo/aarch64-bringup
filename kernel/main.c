/*
 * Boot sequence for the primary CPU, in the order real bring-up happens:
 *
 *   1. early console      -- print as soon as possible, at a known address
 *   2. hardware discovery -- device tree replaces hardcoded addresses
 *   3. MMU + caches       -- RAM becomes Normal memory, MMIO stays Device
 *   4. interrupts         -- GIC, timer tick, interrupt-driven UART
 *   5. firmware services  -- PSCI
 *   6. secondary CPUs     -- CPU_ON each core, verify they check in
 *   7. debug shell
 */
#include <stdint.h>
#include "board.h"
#include "exceptions.h"
#include "fdt.h"
#include "gic.h"
#include "kprintf.h"
#include "mmu.h"
#include "pl011.h"
#include "platform.h"
#include "psci.h"
#include "smp.h"
#include "sysreg.h"
#include "timer.h"

#define VERSION     "0.1.0"
#define TICK_HZ     100

void shell_run(void) __attribute__((noreturn));

/* Map a peripheral the device tree reported, unless the MMIO window has it. */
static void map_device(uint64_t base, uint64_t size)
{
    uint64_t off = base - (uint64_t)BOARD_MMIO_BASE;   /* wraps if below window */
    if (off < (uint64_t)BOARD_MMIO_SIZE && size <= (uint64_t)BOARD_MMIO_SIZE - off)
        return;
    if (mmu_map(base, size, MEM_DEVICE) != 0)
        panic("cannot map device at 0x%lx", base);
}

static void map_memory(void)
{
    for (int i = 0; i < plat.nram; i++) {
        if (mmu_map(plat.ram[i].base, plat.ram[i].size, MEM_NORMAL) != 0)
            panic("cannot map RAM region %d", i);
        klog("RAM: 0x%08lx - 0x%08lx (%lu MiB) -> Normal, write-back\n",
             plat.ram[i].base, plat.ram[i].base + plat.ram[i].size - 1,
             plat.ram[i].size >> 20);
    }

    /* Device mappings go last so they win over any overlapping RAM block. */
    if (mmu_map(BOARD_MMIO_BASE, BOARD_MMIO_SIZE, MEM_DEVICE) != 0)
        panic("cannot map MMIO window");
    map_device(plat.uart_base, 0x1000);
    map_device(plat.gicd, 0x10000);
    map_device(plat.gicc, 0x10000);
    klog("MMIO: 0x%08lx - 0x%08lx -> Device-nGnRE, execute-never\n",
         (uint64_t)BOARD_MMIO_BASE, (uint64_t)BOARD_MMIO_BASE + BOARD_MMIO_SIZE - 1);
}

void kmain(uint64_t dtb, uint64_t boot_el)
{
    /* 1. Early console at the board's default UART. */
    pl011_init(BOARD_UART_BASE, BOARD_UART_CLK_HZ, BOARD_UART_BAUD);
    console_set_output(pl011_putc);
    kprintf("\n\naarch64-bringup v%s (board: %s)\n", VERSION, BOARD_NAME);
    klog("Entered at EL%lu, running at EL%u\n", boot_el, current_el());

    /* 2. Hardware discovery. */
    platform_discover((uintptr_t)dtb, (unsigned)boot_el);
    if (plat.dt_found)
        klog("Device tree at 0x%08lx (%u bytes): %s\n",
             (uint64_t)plat.dtb, fdt_total_size(), plat.model);
    else
        klog("No device tree found: using board defaults\n");

    if (plat.uart_base != BOARD_UART_BASE || plat.uart_clk != BOARD_UART_CLK_HZ) {
        pl011_init(plat.uart_base, plat.uart_clk, BOARD_UART_BAUD);
        klog("Console moved to UART at 0x%08lx\n", (uint64_t)plat.uart_base);
    }

    struct pl011_id id;
    pl011_read_id(plat.uart_base, &id);
    klog("UART: PL011 at 0x%08lx, clock %u Hz, part 0x%03x rev %u, PrimeCell ID %s\n",
         (uint64_t)plat.uart_base, plat.uart_clk, id.part, id.revision,
         id.primecell_ok ? "OK" : "MISMATCH");
    klog("CPUs in device tree: %d, PSCI conduit: %s\n", plat.ncpus,
         plat.psci == PSCI_HVC ? "hvc" : plat.psci == PSCI_SMC ? "smc" : "none");

    /* 3. MMU and caches. */
    map_memory();
    mmu_enable();
    console_enable_locking();
    klog("MMU on: %u-bit VA, 4 KiB granule, %u translation table(s), caches enabled\n",
         mmu_va_bits(), mmu_tables_used());

    /* 4. Interrupts. */
    if (!plat.gic_found) {
        klog("No supported interrupt controller%s: interrupts stay off, UART polled\n",
             plat.gic_unsupported ? " (GICv3 found; this kernel has a GICv2 driver)" : "");
    } else {
        gic_init(plat.gicd, plat.gicc);
        uint32_t iidr = gic_dist_iidr();
        klog("GICv2: distributor 0x%08lx, CPU interface 0x%08lx, %u lines, IIDR 0x%08x\n",
             (uint64_t)plat.gicd, (uint64_t)plat.gicc, gic_num_lines(), iidr);

        timer_init(plat.timer_irq, TICK_HZ);
        pl011_enable_rx_irq(plat.uart_irq);
        irq_enable();
        klog("Interrupts on: timer IRQ %u at %u Hz (counter %lu Hz), UART RX IRQ %u\n",
             plat.timer_irq, TICK_HZ, counter_freq(), plat.uart_irq);
    }

    /* 5. Firmware services. At EL3 there is no firmware above us to call. */
    psci_init(plat.boot_el >= 3 ? PSCI_NONE : plat.psci);
    if (psci_available()) {
        uint32_t v = psci_version();
        klog("PSCI v%u.%u via %s\n", v >> 16, v & 0xffff, psci_conduit_name());
    } else {
        klog("PSCI not available%s\n",
             plat.boot_el >= 3 ? " (booted at EL3 with no resident firmware)" : "");
    }

    /* 6. Secondary CPUs (only if interrupts work: they are woken by SGIs). */
    if (plat.gic_found)
        smp_init();

    klog("Bring-up complete: %u/%d CPUs online\n", smp_online(), plat.ncpus);

    /* 7. Debug shell. */
    shell_run();
}
