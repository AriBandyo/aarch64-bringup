# aarch64-bringup
#
#   make            build build/kernel.elf
#   make run        boot in QEMU, entering at EL1 (Ctrl-A then X to quit)
#   make run-el2    boot entering at EL2 (virtualization extensions on)
#   make run-el3    boot entering at EL3 (security extensions on)
#   make debug      boot halted, waiting for GDB on :1234
#   make gdb        attach gdb-multiarch to a `make debug` session
#   make test       automated test suite in QEMU (all entry levels + GDB)

BOARD   ?= qemu_virt
CROSS   ?= $(shell command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 && echo aarch64-linux-gnu- || echo aarch64-none-elf-)

CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

BUILD   := build
TARGET  := $(BUILD)/kernel.elf

SRCS_C  := $(wildcard arch/aarch64/*.c drivers/*.c lib/*.c kernel/*.c)
SRCS_S  := $(wildcard arch/aarch64/*.S)
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(SRCS_C)) $(patsubst %.S,$(BUILD)/%.o,$(SRCS_S))

INCLUDES := -Iinclude -Iboard/$(BOARD)

# -mgeneral-regs-only: no FP/SIMD registers in kernel code (no FPU state to manage)
# -mstrict-align:      no unaligned accesses (Device memory faults on them, and
#                      everything is Device memory until the MMU is on)
# -mno-outline-atomics: inline LL/SC atomics instead of libgcc helpers
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror \
           -ffreestanding -fno-builtin -nostdlib -fno-stack-protector \
           -fno-pie -fno-pic -fno-common -fno-asynchronous-unwind-tables \
           -fno-tree-loop-distribute-patterns \
           -mcpu=cortex-a53 -mgeneral-regs-only -mstrict-align -mno-outline-atomics \
           $(INCLUDES) -MMD -MP
ASFLAGS := -g $(INCLUDES) -MMD -MP -mcpu=cortex-a53
LDFLAGS := -nostdlib -static -no-pie -Wl,--gc-sections -Wl,-Map,$(BUILD)/kernel.map \
           -Wl,-z,noexecstack -Wl,--no-warn-rwx-segments -Wl,--build-id=none

# ---- QEMU ------------------------------------------------------------------
QEMU    ?= qemu-system-aarch64
CPU     ?= cortex-a53
SMP     ?= 4
MEM     ?= 128M
MACHINE ?= virt
QEMU_ARGS = -M $(MACHINE) -cpu $(CPU) -smp $(SMP) -m $(MEM) -nographic -kernel $(TARGET)

.PHONY: all run run-el2 run-el3 debug gdb test disasm size clean

all: $(TARGET) size

$(BUILD)/linker.ld: linker.ld.S board/$(BOARD)/board.h
	@mkdir -p $(dir $@)
	$(CC) -E -P -x c $(INCLUDES) $< -o $@

$(TARGET): $(OBJS) $(BUILD)/linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -T $(BUILD)/linker.ld $(OBJS) -lgcc -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

size: $(TARGET)
	@$(SIZE) $(TARGET)

run: $(TARGET)
	$(QEMU) $(QEMU_ARGS)

run-el2: MACHINE := virt,virtualization=on
run-el2: run

run-el3: MACHINE := virt,secure=on
run-el3: run

debug: $(TARGET)
	@echo "QEMU halted at the reset vector; run 'make gdb' in another terminal."
	$(QEMU) $(QEMU_ARGS) -s -S

gdb: $(TARGET)
	gdb-multiarch -q -x scripts/gdbinit $(TARGET)

test: $(TARGET)
	python3 tests/run_tests.py --kernel $(TARGET)

disasm: $(TARGET)
	$(OBJDUMP) -d $(TARGET) > $(BUILD)/kernel.dis
	@echo "wrote $(BUILD)/kernel.dis"

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
