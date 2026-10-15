/*
 * Linker script (run through the C preprocessor so the load address and
 * stack sizes come from board.h).
 */
#include "board.h"

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = BOARD_LOAD_ADDR;
    __image_start = .;

    .text : {
        KEEP(*(.text.boot))         /* _start must be first */
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.*)
    }

    /* Zeroed by start.S, 8 bytes at a time. */
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* One stack per CPU; not zeroed. */
    .stacks (NOLOAD) : ALIGN(16) {
        __stacks_start = .;
        . += STACK_SIZE * MAX_CPUS;
        __stacks_end = .;
    }

    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
