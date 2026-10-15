/*
 * AArch64 boot entry.
 *
 * Handles entry at EL3, EL2 or EL1 and always ends up running the kernel at
 * EL1h with interrupts masked:
 *
 *   EL3 --(eret)--> EL2 (if implemented) --(eret)--> EL1 --> kmain()
 *
 * Primary CPU path:
 *   _start -> drop to EL1 -> set stack -> zero .bss -> install vectors -> kmain
 *
 * Secondary CPUs are started later by the primary through PSCI CPU_ON and
 * enter at secondary_entry with x0 = their logical CPU index.
 */
#include "board.h"

#define SPSR_EL1H_MASKED    0x3c5       /* EL1h, D/A/I/F masked */
#define SPSR_EL2H_MASKED    0x3c9       /* EL2h, D/A/I/F masked */

#define HCR_EL2_RW          (1 << 31)   /* EL1 is AArch64 */
#define CNTHCTL_EL1_ACCESS  0x3         /* EL1PCTEN | EL1PCEN */
#define CPTR_EL2_DEFAULT    0x33ff      /* RES1 bits only: no FP/SIMD traps */

#define SCR_EL3_NS          (1 << 0)
#define SCR_EL3_RES1        (3 << 4)
#define SCR_EL3_HCE         (1 << 8)
#define SCR_EL3_RW          (1 << 10)

/* SCTLR_EL1: RES1 bits for ARMv8.0, MMU/caches off, little-endian. */
#define SCTLR_EL1_RESET     0x30d00800

/* GICv2 distributor registers used for secure-world setup at EL3 */
#define GICD_TYPER          0x004
#define GICD_IGROUPR        0x080
#define GICC_PMR            0x004

    .section .text.boot, "ax"
    .global _start
_start:
    mov     x19, x0                 /* x0: DTB pointer (Linux boot protocol) */
    mrs     x20, CurrentEL
    lsr     x20, x20, #2            /* x20: EL we were entered at (kept for kmain) */

    /* Only CPU 0 boots. Others (e.g. released by a spin-table loader) park. */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, park

    cmp     x20, #3
    b.eq    el3_entry
    cmp     x20, #2
    b.eq    el2_entry
    b       el1_entry

/* ------------------------------------------------------------------------ */
/* EL3: route everything to the non-secure world, then drop to EL2 or EL1.  */
el3_entry:
    /*
     * GICv2 with Security Extensions resets every interrupt into Group 0
     * (secure). Non-secure EL1 could not see them, so mark all interrupts
     * Group 1 here. On real hardware this is BL31/TF-A's job.
     */
    ldr     x1, =BOARD_GICD_BASE
    ldr     w2, [x1, #GICD_TYPER]
    and     w2, w2, #0x1f           /* ITLinesNumber: (N+1)*32 interrupts */
    add     w2, w2, #1
    add     x3, x1, #GICD_IGROUPR
    mov     w4, #0xffffffff
1:  str     w4, [x3], #4
    subs    w2, w2, #1
    b.ne    1b

    /*
     * GICC_PMR resets to 0 (mask everything), which is a "secure" priority
     * value; the GICv2 spec makes non-secure writes to PMR ignored while it
     * holds one. Open it up from the secure side so EL1 can set its own mask.
     */
    ldr     x1, =BOARD_GICC_BASE
    mov     w2, #0xff
    str     w2, [x1, #GICC_PMR]

    msr     cptr_el3, xzr           /* no FP/SIMD traps to EL3 */

    /* Is EL2 implemented? ID_AA64PFR0_EL1.EL2, bits [11:8] */
    mrs     x1, id_aa64pfr0_el1
    ubfx    x1, x1, #8, #4
    cbz     x1, 2f

    mov     x1, #(SCR_EL3_RW | SCR_EL3_HCE | SCR_EL3_RES1 | SCR_EL3_NS)
    msr     scr_el3, x1
    mov     x1, #SPSR_EL2H_MASKED
    msr     spsr_el3, x1
    adr     x1, el2_entry
    msr     elr_el3, x1
    eret

2:  mov     x1, #(SCR_EL3_RW | SCR_EL3_RES1 | SCR_EL3_NS)
    msr     scr_el3, x1
    ldr     x1, =SCTLR_EL1_RESET
    msr     sctlr_el1, x1
    mov     x1, #SPSR_EL1H_MASKED
    msr     spsr_el3, x1
    adr     x1, el1_entry
    msr     elr_el3, x1
    eret

/* ------------------------------------------------------------------------ */
/* EL2: configure EL1 as AArch64 with timer access, then drop to EL1.        */
.macro el2_to_el1 target
    mrs     x1, cnthctl_el2
    orr     x1, x1, #CNTHCTL_EL1_ACCESS
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr        /* virtual counter == physical counter */

    mov     x1, #HCR_EL2_RW
    msr     hcr_el2, x1
    mov     x1, #CPTR_EL2_DEFAULT
    msr     cptr_el2, x1
    msr     hstr_el2, xzr

    mrs     x1, midr_el1            /* EL1 reads of MIDR/MPIDR see real IDs */
    msr     vpidr_el2, x1
    mrs     x1, mpidr_el1
    msr     vmpidr_el2, x1

    ldr     x1, =SCTLR_EL1_RESET
    msr     sctlr_el1, x1

    mov     x1, #SPSR_EL1H_MASKED
    msr     spsr_el2, x1
    adr     x1, \target
    msr     elr_el2, x1
    eret
.endm

el2_entry:
    el2_to_el1 el1_entry

/* ------------------------------------------------------------------------ */
/* EL1: C runtime setup for the primary CPU.                                 */
el1_entry:
    ldr     x1, =__stacks_start
    add     x1, x1, #STACK_SIZE     /* CPU 0 stack top */
    mov     sp, x1

    ldr     x1, =__bss_start        /* zero .bss (8-byte aligned by linker) */
    ldr     x2, =__bss_end
3:  cmp     x1, x2
    b.hs    4f
    str     xzr, [x1], #8
    b       3b
4:
    msr     tpidr_el1, xzr          /* logical CPU index 0 */
    ldr     x1, =vectors
    msr     vbar_el1, x1
    isb

    mov     x0, x19                 /* kmain(dtb, boot_el) */
    mov     x1, x20
    bl      kmain
    b       park

/* ------------------------------------------------------------------------ */
/* Secondary CPUs, started by PSCI CPU_ON. x0 = logical CPU index.           */
    .global secondary_entry
secondary_entry:
    mov     x19, x0
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    secondary_el1
    el2_to_el1 secondary_el1

secondary_el1:
    /* sp = __stacks_start + (idx + 1) * STACK_SIZE; no memory reads needed,
     * so this is safe before this core's MMU and caches are on. */
    ldr     x1, =__stacks_start
    add     x2, x19, #1
    mov     x3, #STACK_SIZE
    madd    x1, x2, x3, x1
    mov     sp, x1

    msr     tpidr_el1, x19          /* logical CPU index */
    ldr     x1, =vectors
    msr     vbar_el1, x1
    isb

    mov     x0, x19
    bl      secondary_main

park:
    wfe
    b       park
