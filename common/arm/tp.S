// The thread pointer, for code that says __thread.
//
// RISC-V has tp and the compiler reads it directly, one instruction and no
// call. ARM has no such register, so the ABI defines a function instead: the
// compiler emits a call to __aeabi_read_tp wherever a thread-local is reached,
// and the platform supplies it. This is that supply, and it is the whole of
// what ARM needs to have the thing tp gives away.
//
// r9 holds it. It is the platform register -- reserved by the AAPCS for exactly
// this kind of use -- and myrtos_frame_start puts the process's thread-local
// block there when the process is laid out. The trap frame saves and restores
// r9 like any other register, so a process keeps its own across every switch,
// and kernel or SDK code that uses r9 for its own purposes cannot disturb it.
//
// Assembly rather than C because the ABI is stricter than AAPCS here: this must
// preserve r1 to r3 as well as the callee-saved registers, since the compiler
// calls it in the middle of an expression without expecting to lose anything.
// A C function may clobber r1 to r3 and eventually would.
//
// Modules are compiled with -ffixed-r9 so the register stays theirs.

.syntax unified
.cpu cortex-m33
.thumb

.section .text.__aeabi_read_tp,"ax",%progbits
.global __aeabi_read_tp
.type __aeabi_read_tp, %function
.thumb_func
__aeabi_read_tp:
    mov     r0, r9
    bx      lr
.size __aeabi_read_tp, . - __aeabi_read_tp

// And the personality routine the unwinder would call, which nothing here ever
// will.
//
// LLVM emits an .ARM.exidx entry for every function on this target, and an
// entry that says "this function cannot be unwound" still names the personality
// routine __aeabi_unwind_cpp_pr0. C avoids it because gcc is told
// -fno-exceptions; Zig avoids it with -fno-unwind-tables; LDC emits the tables
// whatever -betterC says. So the symbol is supplied rather than argued with.
//
// Reaching it would mean an exception was thrown in a system that has no
// unwinder to catch it. Returning is as good an answer as any and is smaller
// than the alternative.
.section .text.__aeabi_unwind_cpp_pr0,"ax",%progbits
.global __aeabi_unwind_cpp_pr0
.type __aeabi_unwind_cpp_pr0, %function
.weak __aeabi_unwind_cpp_pr0
.thumb_func
__aeabi_unwind_cpp_pr0:
    bx      lr
.size __aeabi_unwind_cpp_pr0, . - __aeabi_unwind_cpp_pr0

// Assembly files have to say so explicitly, or the linker assumes the worst and
// warns that the stack is executable. Nothing here needs a trampoline on it.
.section .note.GNU-stack,"",%progbits
