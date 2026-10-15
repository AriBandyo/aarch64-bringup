/*
 * Bring-up debug shell on the UART.
 *
 * The kind of monitor you want on first silicon: inspect the CPU, the
 * device tree and the MMU, poke registers, and exercise exceptions,
 * interrupts and SMP on demand.
 */
#include <stdint.h>
#include "exceptions.h"
#include "fdt.h"
#include "gic.h"
#include "kprintf.h"
#include "kstring.h"
#include "mmu.h"
#include "pl011.h"
#include "platform.h"
#include "psci.h"
#include "smp.h"
#include "sysreg.h"
#include "timer.h"

#define LINE_MAX        128
#define MAX_ARGS        8
#define PROMPT          "bringup> "

/* Inside the 39-bit VA range but never mapped: L1 slot 256 stays invalid. */
#define UNMAPPED_ADDR   0x4000000000ull

extern char __image_start[], __image_end[];

struct command {
    const char *name;
    const char *usage;
    const char *help;
    void (*fn)(int argc, char **argv);
};

static const struct command commands[];

/* ---- helpers ------------------------------------------------------------ */

static const char *arm_part_name(uint32_t part)
{
    switch (part) {
    case 0xd03: return "Cortex-A53";
    case 0xd04: return "Cortex-A35";
    case 0xd05: return "Cortex-A55";
    case 0xd07: return "Cortex-A57";
    case 0xd08: return "Cortex-A72";
    case 0xd09: return "Cortex-A73";
    case 0xd0a: return "Cortex-A75";
    case 0xd0b: return "Cortex-A76";
    case 0xd0c: return "Neoverse-N1";
    case 0xd0d: return "Cortex-A77";
    case 0xd40: return "Neoverse-V1";
    case 0xd41: return "Cortex-A78";
    case 0xd44: return "Cortex-X1";
    case 0xd46: return "Cortex-A510";
    case 0xd47: return "Cortex-A710";
    case 0xd48: return "Cortex-X2";
    case 0xd49: return "Neoverse-N2";
    case 0xd4f: return "Neoverse-V2";
    default:    return 0;
    }
}

static const char *mair_name(uint8_t attr)
{
    switch (attr) {
    case 0x00: return "Device-nGnRnE";
    case 0x04: return "Device-nGnRE";
    case 0xff: return "Normal, write-back cacheable";
    case 0x44: return "Normal, non-cacheable";
    default:   return "other";
    }
}

static int parse_args(char *line, char **argv)
{
    int argc = 0;
    while (*line && argc < MAX_ARGS) {
        while (*line == ' ' || *line == '\t')
            *line++ = '\0';
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && *line != ' ' && *line != '\t')
            line++;
    }
    return argc;
}

static int arg_u64(const char *s, uint64_t *v, const char *what)
{
    if (parse_u64(s, v) == 0)
        return 0;
    kprintf("invalid %s: '%s'\n", what, s);
    return -1;
}

/* ---- commands ----------------------------------------------------------- */

static void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (const struct command *c = commands; c->name; c++) {
        char left[40];
        ksnprintf(left, sizeof(left), "%s %s", c->name, c->usage);
        kprintf("  %-26s %s\n", left, c->help);
    }
}

static void cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint64_t midr = read_sysreg(midr_el1);
    uint32_t impl = (uint32_t)(midr >> 24) & 0xff;
    uint32_t part = (uint32_t)(midr >> 4) & 0xfff;
    const char *pname = impl == 0x41 ? arm_part_name(part) : 0;
    uint64_t ctr = read_sysreg(ctr_el0);
    uint64_t sctlr = read_sysreg(sctlr_el1);

    kprintf("Board:       %s\n", plat.model);
    kprintf("CPU:         %s%s r%lup%lu (MIDR 0x%08lx, implementer 0x%02x, part 0x%03x)\n",
            impl == 0x41 ? "Arm " : "", pname ? pname : "unknown core",
            (midr >> 20) & 0xf, midr & 0xf, midr, impl, part);
    kprintf("CPUs:        %u/%d online\n", smp_online(), plat.ncpus);
    kprintf("EL:          entered at EL%u, running at EL%u\n", plat.boot_el, current_el());
    kprintf("MMU:         %s, %u-bit VA, 4 KiB granule, %u translation table(s); D-cache %s, I-cache %s\n",
            (sctlr & 1) ? "on" : "off", mmu_va_bits(), mmu_tables_used(),
            (sctlr & (1 << 2)) ? "on" : "off", (sctlr & (1 << 12)) ? "on" : "off");
    kprintf("Cache lines: I %lu B, D %lu B (CTR_EL0)\n",
            4ul << (ctr & 0xf), 4ul << ((ctr >> 16) & 0xf));
    kprintf("Counter:     %lu Hz\n", counter_freq());
    for (int i = 0; i < plat.nram; i++)
        kprintf("RAM:         0x%08lx - 0x%08lx (%lu MiB)\n", plat.ram[i].base,
                plat.ram[i].base + plat.ram[i].size - 1, plat.ram[i].size >> 20);
    kprintf("Image:       0x%08lx - 0x%08lx\n",
            (uint64_t)(uintptr_t)__image_start, (uint64_t)(uintptr_t)__image_end);
    kprintf("UART:        PL011 at 0x%08lx, IRQ %u, clock %u Hz\n",
            (uint64_t)plat.uart_base, plat.uart_irq, plat.uart_clk);
    if (plat.gic_found)
        kprintf("GIC:         GICv2, distributor 0x%08lx, CPU interface 0x%08lx, %u IRQ lines\n",
                (uint64_t)plat.gicd, (uint64_t)plat.gicc, gic_num_lines());
    kprintf("Timer:       EL1 virtual timer, IRQ %u, %u Hz tick\n", plat.timer_irq, timer_hz());
    if (psci_available()) {
        uint32_t v = psci_version();
        kprintf("PSCI:        v%u.%u via %s\n", v >> 16, v & 0xffff, psci_conduit_name());
    } else {
        kprintf("PSCI:        not available\n");
    }
    if (plat.dt_found)
        kprintf("DTB:         0x%08lx (%u bytes)\n", (uint64_t)plat.dtb, fdt_total_size());
    else
        kprintf("DTB:         not found (board defaults in use)\n");
}

static int dt_print_cb(const struct fdt_node *n, void *ctx)
{
    (void)ctx;
    const char *compat = fdt_getprop_str(n, "compatible");
    kprintf("%*s%s%s%s%s\n", n->depth * 2, "", n->depth ? n->name : "/",
            compat ? "  [" : "", compat ? compat : "", compat ? "]" : "");
    return 0;
}

static void cmd_dt(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!fdt_valid()) {
        kprintf("no device tree\n");
        return;
    }
    fdt_walk(dt_print_cb, 0);
}

static void cmd_md(int argc, char **argv)
{
    uint64_t addr, count = 16;

    if (argc < 2 || arg_u64(argv[1], &addr, "address"))
        return;
    if (argc > 2 && arg_u64(argv[2], &count, "count"))
        return;
    if (addr & 3) {
        kprintf("md: address must be 4-byte aligned\n");
        return;
    }
    if (count > 1024)
        count = 1024;

    exceptions_set_verbose(0);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t a = addr + 4 * i;
        uint32_t v;
        if (i % 4 == 0)
            kprintf("%s%08lx:", i ? "\n" : "", a);
        if (safe_read32(a, &v) != 0) {
            kprintf(" <abort>\nmd: access to 0x%lx aborted (run 'pt 0x%lx' to check the mapping)\n",
                    a, a);
            exceptions_set_verbose(1);
            return;
        }
        kprintf(" %08x", v);
    }
    kprintf("\n");
    exceptions_set_verbose(1);
}

static void cmd_mw(int argc, char **argv)
{
    uint64_t addr, val;

    if (argc < 3 || arg_u64(argv[1], &addr, "address") || arg_u64(argv[2], &val, "value"))
        return;
    if (addr & 3) {
        kprintf("mw: address must be 4-byte aligned\n");
        return;
    }
    if (safe_write32(addr, (uint32_t)val) == 0)
        kprintf("0x%08lx <- 0x%08x\n", addr, (uint32_t)val);
}

static void cmd_pt(int argc, char **argv)
{
    uint64_t va, pa;
    uint8_t attr;
    unsigned fsc;

    if (argc < 2 || arg_u64(argv[1], &va, "address"))
        return;
    if (mmu_translate(va, &pa, &attr, &fsc) == 0) {
        kprintf("VA 0x%016lx -> PA 0x%016lx  attr 0x%02x (%s)\n", va, pa, attr, mair_name(attr));
    } else {
        const char *kind = (fsc >> 2) == 1 ? "translation fault" :
                           (fsc >> 2) == 2 ? "access flag fault" :
                           (fsc >> 2) == 3 ? "permission fault" : "fault";
        kprintf("VA 0x%016lx -> %s, level %u (FSC 0x%02x)\n", va, kind, fsc & 3, fsc);
    }
}

static void cmd_irqs(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("  INTID  count       source\n");
    for (unsigned i = 0; i < GIC_MAX_IRQ; i++) {
        uint64_t n = irq_count(i);
        if (!n)
            continue;
        const char *src = i == plat.timer_irq ? "arch timer (PPI)" :
                          i == plat.uart_irq  ? "PL011 UART RX (SPI)" :
                          i == SGI_WORK       ? "SGI: work dispatch" :
                          i == SGI_PING       ? "SGI: ping" : "";
        kprintf("  %5u  %-10lu  %s\n", i, n, src);
    }
    kprintf("  UART RX overruns: %u\n", pl011_rx_overruns());
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint64_t us = uptime_us();
    kprintf("up %lu.%06lu s, %lu timer ticks at %u Hz\n",
            us / 1000000, us % 1000000, timer_ticks(), timer_hz());
}

static void cmd_smp(int argc, char **argv)
{
    uint64_t iters = 100000, elapsed;

    if (argc > 1 && arg_u64(argv[1], &iters, "iterations"))
        return;

    unsigned online = smp_online();
    kprintf("CPUs online: %u/%u\n", online, smp_possible());

    uint64_t expect = (uint64_t)online * iters;
    uint64_t got = smp_counter_test(1, iters, &elapsed);
    kprintf("spinlock test: %lu/%lu increments across %u CPUs (%s) in %lu us\n",
            got, expect, online, got == expect ? "OK" : "FAIL", elapsed);

    if (online > 1) {
        unsigned ans = smp_ipi_test();
        kprintf("IPI test: %u/%u CPUs answered SGI %u\n", ans, online - 1, SGI_PING);
    }
}

static void cmd_race(int argc, char **argv)
{
    uint64_t iters = 100000, elapsed;

    if (argc > 1 && arg_u64(argv[1], &iters, "iterations"))
        return;
    unsigned online = smp_online();
    uint64_t expect = (uint64_t)online * iters;
    uint64_t got = smp_counter_test(0, iters, &elapsed);
    kprintf("unlocked increments: %lu/%lu across %u CPUs, %lu lost updates\n",
            got, expect, online, expect - got);
    kprintf("(non-atomic load/add/store; compare with 'smp', which uses the spinlock)\n");
}

static void cmd_fault(int argc, char **argv)
{
    uint64_t addr = UNMAPPED_ADDR;
    uint32_t v;

    if (argc > 1 && arg_u64(argv[1], &addr, "address"))
        return;
    kprintf("reading 0x%lx...\n", addr);
    if (safe_read32(addr, &v) == 0)
        kprintf("read 0x%08x (no fault)\n", v);
}

static void cmd_svc(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    trigger_svc();
    kprintf("returned from SVC handler\n");
}

static void cmd_undef(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    trigger_undef();
    kprintf("returned from undefined-instruction handler\n");
}

static void cmd_crash(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("writing to unmapped address 0x%llx with no recovery...\n", UNMAPPED_ADDR);
    *(volatile uint32_t *)(uintptr_t)UNMAPPED_ADDR = 0xdead;
}

static void cmd_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!psci_available()) {
        kprintf("PSCI not available\n");
        return;
    }
    kprintf("resetting...\n");
    psci_system_reset();
}

static void cmd_poweroff(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!psci_available()) {
        kprintf("PSCI not available\n");
        return;
    }
    kprintf("powering off\n");
    psci_system_off();
}

static const struct command commands[] = {
    { "help",     "",              "list commands",                          cmd_help },
    { "info",     "",              "CPU, memory map and peripheral summary", cmd_info },
    { "dt",       "",              "print the device tree",                  cmd_dt },
    { "md",       "<addr> [n]",    "dump n 32-bit words (fault-safe)",       cmd_md },
    { "mw",       "<addr> <val>",  "write a 32-bit word (fault-safe)",       cmd_mw },
    { "pt",       "<va>",          "translate a VA with AT S1E1R",           cmd_pt },
    { "irqs",     "",              "per-interrupt counts",                   cmd_irqs },
    { "uptime",   "",              "time since boot and timer ticks",        cmd_uptime },
    { "smp",      "[iters]",       "spinlock + IPI test on all CPUs",        cmd_smp },
    { "race",     "[iters]",       "same counter test without the lock",     cmd_race },
    { "fault",    "[addr]",        "read an unmapped address and recover",   cmd_fault },
    { "svc",      "",              "take a supervisor call exception",       cmd_svc },
    { "undef",    "",              "execute UDF and recover",                cmd_undef },
    { "crash",    "",              "unrecoverable abort: register dump",     cmd_crash },
    { "reset",    "",              "PSCI SYSTEM_RESET",                      cmd_reset },
    { "poweroff", "",              "PSCI SYSTEM_OFF",                        cmd_poweroff },
    { 0, 0, 0, 0 },
};

/* ---- line input --------------------------------------------------------- */

static int read_line(char *buf, int max)
{
    int len = 0;
    char prev = 0;

    for (;;) {
        char c = pl011_getc();
        if (c == '\n' && prev == '\r') {
            prev = c;
            continue;                   /* CRLF: already handled the CR */
        }
        prev = c;
        if (c == '\r' || c == '\n') {
            console_puts("\n");
            buf[len] = '\0';
            return len;
        }
        if (c == 0x7f || c == '\b') {
            if (len) {
                len--;
                console_puts("\b \b");
            }
            continue;
        }
        if (c == 0x15) {                /* Ctrl-U: kill line */
            while (len) {
                len--;
                console_puts("\b \b");
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7f && len < max - 1) {
            buf[len++] = c;
            console_putc(c);
        }
    }
}

void shell_run(void)
{
    char line[LINE_MAX];
    char *argv[MAX_ARGS];

    kprintf("\nType 'help' for commands.\n");
    for (;;) {
        console_puts(PROMPT);
        read_line(line, sizeof(line));
        int argc = parse_args(line, argv);
        if (!argc)
            continue;

        const struct command *c;
        for (c = commands; c->name; c++)
            if (strcmp(c->name, argv[0]) == 0)
                break;
        if (c->name)
            c->fn(argc, argv);
        else
            kprintf("unknown command '%s' (try 'help')\n", argv[0]);
    }
}
