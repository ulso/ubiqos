# Enough to get to C on qemu's virt machine: a stack, and a jump.
    .section .text.start,"ax"
    .globl _start
_start:
    la   sp, _stack_top
    call qemu_main
1:  j    1b
