/*
 * Board defaults for the QEMU "virt" machine.
 *
 * These are FALLBACKS only. At boot the kernel reads the flattened device
 * tree (FDT) and uses the addresses it finds there; these values are used
 * for the early console (before the FDT is parsed) and if no FDT is found.
 *
 * This header is included from C, assembly and the linker script, so it
 * must contain only plain #defines with no C-only syntax (no UL suffixes).
 *
 * To port to another board, add board/<name>/board.h and build with
 * `make BOARD=<name>`.
 */
#ifndef BOARD_H
#define BOARD_H

#define BOARD_NAME          "qemu-virt"

/* Where the image is linked. RAM starts at 0x40000000 on virt. For bare ELF
 * images QEMU places the DTB at the start of RAM and only if it fits below the
 * image; the virt DTB is 1 MiB, so the image starts 2 MiB in. */
#define BOARD_LOAD_ADDR     0x40200000

/* Early console: PL011 UART */
#define BOARD_UART_BASE     0x09000000
#define BOARD_UART_CLK_HZ   24000000
#define BOARD_UART_BAUD     115200

/* GICv2 distributor / CPU interface */
#define BOARD_GICD_BASE     0x08000000
#define BOARD_GICC_BASE     0x08010000

/* Peripheral window mapped as Device memory by the MMU, so the shell's
 * md/mw commands can reach any MMIO block. Virt puts flash, GIC, UART,
 * RTC, GPIO and virtio-mmio below 1 GiB. */
#define BOARD_MMIO_BASE     0x00000000
#define BOARD_MMIO_SIZE     0x40000000

/* RAM (fallback if the FDT has no /memory node) */
#define BOARD_RAM_BASE      0x40000000
#define BOARD_RAM_SIZE      0x08000000

/* Where to look for the DTB if x0 at entry does not point at one */
#define BOARD_DTB_FALLBACK  0x40000000

/* SMP limits */
#define MAX_CPUS            8
#define STACK_SIZE          0x4000      /* 16 KiB per CPU */

#endif
