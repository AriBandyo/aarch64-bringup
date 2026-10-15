/*
 * EL1 exception vector table.
 *
 * 16 entries of 0x80 bytes, table aligned to 2 KiB (VBAR_EL1 requirement):
 *   0x000  current EL, SP_EL0   : sync / IRQ / FIQ / SError
 *   0x200  current EL, SP_ELx   : sync / IRQ / FIQ / SError   <- we run here
 *   0x400  lower EL, AArch64    : sync / IRQ / FIQ / SError
 *   0x600  lower EL, AArch32    : sync / IRQ / FIQ / SError
 *
 * Every entry saves the full register state into a struct trap_frame on the
 * stack, calls a C handler with a pointer to it, restores (C may have edited
 * ELR to skip an instruction) and returns with ERET.
 */

#define FRAME_SIZE  272     /* 31 GPRs + ELR + SPSR, padded to 16 bytes */

.macro save_frame
    sub     sp, sp, #FRAME_SIZE
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    stp     x30, x21, [sp, #16 * 15]
    str     x22, [sp, #16 * 16]
.endm

.macro restore_frame_and_eret
    ldr     x22, [sp, #16 * 16]
    ldp     x30, x21, [sp, #16 * 15]
    msr     elr_el1, x21
    msr     spsr_el1, x22
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #FRAME_SIZE
    eret
.endm

/* One vector slot: save state, call handler(frame, kind), return. */
.macro vector_entry handler, kind
    .balign 0x80
    save_frame
    mov     x0, sp
    mov     x1, #\kind
    bl      \handler
    b       exception_return
.endm

/* Must match enum vector_kind in exceptions.h */
#define VEC_SYNC     0
#define VEC_IRQ      1
#define VEC_FIQ      2
#define VEC_SERROR   3
#define VEC_SP0      0x10
#define VEC_LOWER64  0x20
#define VEC_LOWER32  0x30

    .text
    .balign 0x800
    .global vectors
vectors:
    vector_entry handle_sync,       (VEC_SP0 | VEC_SYNC)
    vector_entry handle_unexpected, (VEC_SP0 | VEC_IRQ)
    vector_entry handle_unexpected, (VEC_SP0 | VEC_FIQ)
    vector_entry handle_unexpected, (VEC_SP0 | VEC_SERROR)

    vector_entry handle_sync,       VEC_SYNC
    vector_entry handle_irq,        VEC_IRQ
    vector_entry handle_unexpected, VEC_FIQ
    vector_entry handle_unexpected, VEC_SERROR

    vector_entry handle_sync,       (VEC_LOWER64 | VEC_SYNC)
    vector_entry handle_irq,        (VEC_LOWER64 | VEC_IRQ)
    vector_entry handle_unexpected, (VEC_LOWER64 | VEC_FIQ)
    vector_entry handle_unexpected, (VEC_LOWER64 | VEC_SERROR)

    vector_entry handle_unexpected, (VEC_LOWER32 | VEC_SYNC)
    vector_entry handle_unexpected, (VEC_LOWER32 | VEC_IRQ)
    vector_entry handle_unexpected, (VEC_LOWER32 | VEC_FIQ)
    vector_entry handle_unexpected, (VEC_LOWER32 | VEC_SERROR)

    /* Fails to assemble if any entry overflowed its 0x80-byte slot. */
    .org vectors + 0x800

exception_return:
    restore_frame_and_eret
