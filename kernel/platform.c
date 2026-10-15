/*
 * Hardware discovery: one pass over the device tree fills `plat`.
 *
 * Interrupt specifiers are decoded in the Arm GIC binding format
 * (3 cells: type, number, flags; type 0 = SPI, 1 = PPI).
 */
#include "platform.h"
#include "fdt.h"
#include "kstring.h"

struct platform plat;

static const char *const gicv2_compat[] = {
    "arm,cortex-a15-gic", "arm,gic-400", "arm,cortex-a9-gic", "arm,cortex-a7-gic",
};

static unsigned gic_intid(const uint8_t *spec)
{
    uint32_t type = fdt_be32(spec);
    uint32_t num = fdt_be32(spec + 4);
    return (type == 0 ? 32 : 16) + num;
}

static int is_status_okay(const struct fdt_node *n)
{
    const char *s = fdt_getprop_str(n, "status");
    return !s || strcmp(s, "okay") == 0 || strcmp(s, "ok") == 0;
}

struct scan {
    uint32_t uart_clk_phandle;
    int      uart_found;
};

static int scan_node(const struct fdt_node *n, void *p)
{
    struct scan *sc = p;
    const char *dtype = fdt_getprop_str(n, "device_type");
    int len;

    if (n->depth == 0) {
        const char *model = fdt_getprop_str(n, "model");
        if (!model)
            model = fdt_getprop_str(n, "compatible");
        if (model) {
            size_t l = strlen(model);
            if (l >= sizeof(plat.model))
                l = sizeof(plat.model) - 1;
            memcpy(plat.model, model, l);
            plat.model[l] = '\0';
        }
        return 0;
    }

    if (!is_status_okay(n))
        return 0;

    if (dtype && strcmp(dtype, "memory") == 0) {
        uint64_t base, size;
        for (int i = 0; plat.nram < MAX_RAM_REGIONS &&
                        fdt_get_reg(n, i, &base, &size) == 0; i++) {
            plat.ram[plat.nram].base = base;
            plat.ram[plat.nram].size = size;
            plat.nram++;
        }
        return 0;
    }

    if (dtype && strcmp(dtype, "cpu") == 0) {
        uint64_t mpidr;
        if (plat.ncpus < MAX_CPUS && fdt_get_reg(n, 0, &mpidr, 0) == 0)
            plat.mpidr[plat.ncpus++] = mpidr;
        return 0;
    }

    if (!sc->uart_found && fdt_is_compatible(n, "arm,pl011")) {
        uint64_t base;
        if (fdt_get_reg(n, 0, &base, 0) == 0) {
            sc->uart_found = 1;
            plat.uart_base = (uintptr_t)base;
            const uint8_t *irq = fdt_getprop(n, "interrupts", &len);
            if (irq && len >= 12)
                plat.uart_irq = gic_intid(irq);
            const uint8_t *clk = fdt_getprop(n, "clocks", &len);
            if (clk && len >= 4)
                sc->uart_clk_phandle = fdt_be32(clk);
        }
        return 0;
    }

    for (unsigned i = 0; i < sizeof(gicv2_compat) / sizeof(gicv2_compat[0]); i++) {
        if (fdt_is_compatible(n, gicv2_compat[i])) {
            uint64_t d, c;
            if (fdt_get_reg(n, 0, &d, 0) == 0 && fdt_get_reg(n, 1, &c, 0) == 0) {
                plat.gic_found = 1;
                plat.gicd = (uintptr_t)d;
                plat.gicc = (uintptr_t)c;
            }
            return 0;
        }
    }
    if (fdt_is_compatible(n, "arm,gic-v3")) {
        plat.gic_unsupported = 1;
        return 0;
    }

    if (fdt_is_compatible(n, "arm,armv8-timer")) {
        /* interrupts: secure phys, non-secure phys, virtual, hypervisor */
        const uint8_t *irq = fdt_getprop(n, "interrupts", &len);
        if (irq && len >= 36)
            plat.timer_irq = gic_intid(irq + 24);
        return 0;
    }

    if (fdt_is_compatible(n, "arm,psci-1.0") || fdt_is_compatible(n, "arm,psci-0.2")) {
        const char *method = fdt_getprop_str(n, "method");
        if (method && strcmp(method, "hvc") == 0)
            plat.psci = PSCI_HVC;
        else if (method && strcmp(method, "smc") == 0)
            plat.psci = PSCI_SMC;
        return 0;
    }

    return 0;
}

void platform_discover(uintptr_t dtb, unsigned boot_el)
{
    memset(&plat, 0, sizeof(plat));
    plat.boot_el = boot_el;

    /* Board defaults; overwritten by anything the DT provides. */
    plat.uart_base = BOARD_UART_BASE;
    plat.uart_clk = BOARD_UART_CLK_HZ;
    plat.uart_irq = 33;                 /* SPI 1 on virt */
    plat.gicd = BOARD_GICD_BASE;
    plat.gicc = BOARD_GICC_BASE;
    plat.timer_irq = 27;                /* PPI 11: EL1 virtual timer */

    /* Linux boot protocol passes the DTB in x0; QEMU puts it at the start
     * of RAM for bare ELF images. Try both. */
    if (dtb && fdt_init((const void *)dtb) == 0)
        plat.dtb = dtb;
    else if (fdt_init((const void *)(uintptr_t)BOARD_DTB_FALLBACK) == 0)
        plat.dtb = BOARD_DTB_FALLBACK;

    if (plat.dtb) {
        struct scan sc = { 0, 0 };
        plat.dt_found = 1;
        fdt_walk(scan_node, &sc);

        struct fdt_node clk;
        uint32_t hz;
        if (sc.uart_clk_phandle && fdt_find_phandle(sc.uart_clk_phandle, &clk) == 0 &&
            fdt_getprop_u32(&clk, "clock-frequency", &hz) == 0 && hz)
            plat.uart_clk = hz;
    }

    if (!plat.dt_found || !plat.model[0])
        memcpy(plat.model, BOARD_NAME, sizeof(BOARD_NAME));
    if (plat.nram == 0) {
        plat.ram[0].base = BOARD_RAM_BASE;
        plat.ram[0].size = BOARD_RAM_SIZE;
        plat.nram = 1;
    }
    if (plat.ncpus == 0) {
        plat.ncpus = 1;
        plat.mpidr[0] = 0;
    }
    if (!plat.dt_found)
        plat.gic_found = 1;             /* trust board defaults */
}
