# The allocator, off the board, two ways.
#
#   make host    on the Mac, where lldb is
#   make qemu    as 32-bit RISC-V, where the word is the board's width
#
# Both matter. The host cannot show a fault in the size arithmetic, because its
# block header is twice the board's; qemu can, and its "largest free at the
# start" is the same 36692 the board's own boot self-test prints.

RISCV ?= $(HOME)/.pico-sdk/toolchain/RISCV_ZCB_RPI_2_2_0_3/bin/riscv32-unknown-elf
SRC    = main.c checks.c ../../kernel/tlsf.c
QSRC   = qemu/boot.S qemu/qemu_main.c checks.c ../../kernel/tlsf.c

# The toolchain's libgcc is built with Zcb, so qemu's plain rv32 refuses its
# __clzsi2 with an illegal instruction -- which, with no trap vector, looks
# exactly like a hang. Hence the extensions below.
QCPU = rv32,zca=true,zcb=true,zba=true,zbb=true,zbs=true

host:
	clang -I. -O0 -g -Wall -o /tmp/tlsfhost $(SRC)
	/tmp/tlsfhost

qemu:
	$(RISCV)-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -ffreestanding \
	  -nostdlib -O2 -I. -T qemu/qemu.ld -Wl,--no-warn-rwx-segments \
	  -o /tmp/tlsfqemu.elf $(QSRC) -lgcc
	qemu-system-riscv32 -machine virt -cpu $(QCPU) -bios none \
	  -kernel /tmp/tlsfqemu.elf -nographic

.PHONY: host qemu
