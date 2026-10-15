# Code walkthrough

A reading order for the source, with the concepts each file depends on and the
questions worth being able to answer about it. About 3,300 lines of C and
assembly including comments, plus a 350-line Python test harness.

## 1. `arch/aarch64/start.S`: getting to C

Read first. Everything after this is ordinary C.

- **Exception levels.** EL3 = secure monitor/firmware, EL2 = hypervisor,
  EL1 = OS kernel, EL0 = user. Hardware may enter the image at any of the top
  three; QEMU picks based on `secure=` and `virtualization=`.
- **Dropping a level** is an `eret` from a fake exception: write the target state
  to `SPSR_ELx` (mode + masked interrupts) and the target address to `ELR_ELx`.
- **What must be set before EL1 works:** `SCR_EL3.NS/RW` (non-secure, AArch64
  below), `HCR_EL2.RW` (EL1 is AArch64), `CNTHCTL_EL2` (EL1 may use the timer),
  `SCTLR_EL1` to a known value (MMU off, little-endian).
- **C runtime:** a stack (`sp`), a zeroed `.bss`, and `VBAR_EL1` pointing at the
  vector table before anything can fault.

Questions: Why does only CPU 0 run this? What would break if `.bss` were not
zeroed? Why does the EL3 path touch the GIC?

## 2. `arch/aarch64/vectors.S` and `exceptions.c`: faults and IRQs

- **Vector table:** 16 slots x 0x80 bytes; which slot is used depends on
  where the exception came from (same EL using SP_EL0 or SP_ELx, or a lower EL in
  AArch64 or AArch32) and its type (sync, IRQ, FIQ, SError). The `.org` line
  makes the build fail if a slot overflows.
- **ESR_EL1** says why: `EC` (class, e.g. 0x25 data abort at same EL),
  `ISS` (details: fault status code with the translation level, read vs write).
  **FAR_EL1** is the faulting address, **ELR_EL1** the faulting instruction.
- **Recovery:** for an expected fault the handler adds 4 to the saved ELR, so
  `eret` resumes after the bad instruction. That is how `md` can probe any
  address safely.
- **IRQ path:** read `GICC_IAR` (acknowledge, get the interrupt ID), dispatch,
  write `GICC_EOIR` (end of interrupt).

Questions: Why is SVC handled without changing ELR? Why must the probed access
be a single instruction?

## 3. `drivers/pl011.c`: first device driver

- MMIO through `volatile` accessors so the compiler never merges, reorders or
  drops register accesses.
- Baud: divisor = clk / (16 x baud) in 16.6 fixed point (`IBRD`, `FBRD`).
- PrimeCell ID registers at offset 0xFE0 identify the block before trusting it.
- RX ring buffer: the IRQ handler produces, the shell consumes; a `dmb` orders
  the data write before the index update.
- The interrupt is cleared before draining the FIFO (see README, bug 3).

## 4. `lib/fdt.c` and `kernel/platform.c`: hardware discovery

- DTB = header + token stream (`BEGIN_NODE`, `PROP`, `END_NODE`) + string table,
  all big-endian.
- `reg` is decoded with the **parent's** `#address-cells`/`#size-cells`, which
  is why the walker tracks them per depth.
- Interrupt specifiers in the GIC binding: `<type number flags>`, type 0 = SPI
  (ID 32+n), 1 = PPI (ID 16+n).
- The UART clock comes from following the `clocks` phandle to a fixed-clock node.

Question: what in this kernel would change to run on a different SoC?

## 5. `arch/aarch64/mmu.c`: virtual memory

- 4 KiB granule, `T0SZ = 25` -> 39-bit VA -> walk starts at level 1. A level 1
  entry maps 1 GiB; a level 2 entry maps 2 MiB.
- Descriptor bits: type (block/table), `AttrIndx` (row in `MAIR_EL1`), `SH`
  (shareability), `AF` (access flag; without it the first access faults), `PXN`/
  `UXN` (execute-never).
- Memory types: **Device-nGnRE** for MMIO (no gathering, no reordering, no early
  write acknowledgement; never cached or speculated), **Normal write-back** for RAM.
- Enable order: tables written -> `dsb` -> MAIR/TCR/TTBR0 -> `isb` -> TLB and
  I-cache invalidate -> `dsb`/`isb` -> set `SCTLR_EL1.M/C/I` -> `isb`.
- `pt` uses `AT S1E1R`: the CPU walks the tables itself and reports in `PAR_EL1`.

Questions: why identity-map? Why must MMIO never be mapped Normal? Why does the
`fault` test address produce a *level 1* translation fault?

## 6. `drivers/gicv2.c` and `drivers/timer.c`: interrupts

- GIC distributor (shared, routes and prioritizes) vs CPU interface (per core,
  acknowledges). IDs 0-15 SGIs (software, used as IPIs), 16-31 PPIs (per core,
  e.g. the timer), 32+ SPIs (shared peripherals, e.g. the UART).
- SGI/PPI registers are banked per core, so every core runs `gic_cpu_init()`.
- Lower priority value = more urgent; `GICC_PMR` masks anything not more urgent
  than it.
- Timer: `CNTVCT_EL0` counts at `CNTFRQ_EL0`; the IRQ fires when it passes
  `CNTV_CVAL_EL0`. Adding a fixed interval to CVAL avoids drift.

## 7. `arch/aarch64/psci.c` and `kernel/smp.c`: multiple cores

- PSCI is a firmware interface reached by `hvc` or `smc` with a function ID in
  `x0` (SMC Calling Convention). `CPU_ON(mpidr, entry, context)` starts a core at
  `entry` with `x0 = context`.
- Each secondary: own stack (from its index), own MMU enable with the shared
  tables, own GIC interface, then sleep in WFI until an SGI arrives.
- Spinlock (`include/spinlock.h`): `ldaxr`/`stxr` exclusive pair, waiters in
  `wfe`, release with `stlr`. Acquire/release semantics order the critical
  section without full barriers.
- `race` shows why: four cores doing load/add/store on one counter lose updates.

Questions: why must a core's MMU be on before it takes a shared lock? What do the
`a`/`l` in `ldaxr`/`stlr` guarantee?

## 8. `tests/run_tests.py`: verification

Starts QEMU with the serial port on a pipe, waits for patterns, sends commands,
checks output. Four machine configurations plus a crash test and a GDB session.
Bug 3 in the README was found by this suite.
