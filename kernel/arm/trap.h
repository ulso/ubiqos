#ifndef MYRTOS_ARM_TRAP_H
#define MYRTOS_ARM_TRAP_H

#include <stdint.h>

// The frame kernel/arm/scheduler.S builds, and it is two frames end to end.
//
// The core pushes the second half itself on the way into the exception -- r0 to
// r3, r12, lr, pc and xPSR -- and pops it again on the way out. Software saves
// what is left, immediately below it, so the two are one structure and a
// pointer to the bottom of it is the whole context. That is the promise
// kernel/riscv/trap.h makes as well, and the reason the handler's signature is
// the same on both: it is given a frame and answers with the frame to resume.
//
// Eighteen words against thirty-three, because half the work is the core's --
// ten pushed by scheduler.S and eight by the hardware.
//
// a0 to a3 and a7 are the syscall ABI's names, not the machine's. The number
// travels in r7 and the arguments in r0 to r2, which is the same shape RISC-V
// uses with a7 and a0 to a2, so the syscall layer needs to know neither.
typedef struct {
    uint32_t cause;              // IPSR: which exception arrived
    uint32_t r4, r5, r6;
    uint32_t a7;                 // r7, and the system call number
    uint32_t r8, r9, r10, r11;

    // Not a return address. EXC_RETURN says which stack to resume on and
    // whether floating-point state was stacked, and the call to the handler
    // destroys lr, so it is saved and put back like any other register.
    uint32_t exc_return;

    // From here down the core did the work, and will undo it.
    uint32_t a0, a1, a2, a3;     // r0 to r3
    uint32_t r12;
    uint32_t lr;                 // the interrupted code's own return address
    uint32_t pc;                 // where it resumes: mepc's opposite number
    uint32_t xpsr;
} myrtos_frame_t;

_Static_assert(sizeof(myrtos_frame_t) == 72, "the frame must match scheduler.S");

// IPSR exception numbers, which is what cause holds here.
#define ARM_EXC_HARDFAULT   3u
#define ARM_EXC_SVCALL     11u
#define ARM_EXC_PENDSV     14u
#define ARM_EXC_SYSTICK    15u

// What happened, asked in the way the shared code asks it. There is no
// interrupt bit to test: the number says which, and anything at or above 16 is
// a peripheral interrupt.
#define MYRTOS_TRAP_IS_INTERRUPT(f)  ((f)->cause >= ARM_EXC_PENDSV)
#define MYRTOS_TRAP_IS_TIMER(f)      ((f)->cause == ARM_EXC_SYSTICK)
#define MYRTOS_TRAP_IS_SYSCALL(f)    ((f)->cause == ARM_EXC_SVCALL)
#define MYRTOS_TRAP_IS_BREAKPOINT(f) ((f)->cause == ARM_EXC_HARDFAULT)

// Past the call, and back onto it.
//
// Nothing to skip: the stacked pc already points after the svc, where RISC-V's
// mepc points at the ecall and has to be stepped over. Going back is two bytes,
// because svc has only a sixteen-bit form -- where ecall has only a
// thirty-two-bit one. The asymmetry is the whole reason these are macros.
#define MYRTOS_TRAP_SKIP(f)  ((void)0)
#define MYRTOS_TRAP_REDO(f)  ((f)->pc -= 2)

// The address that faulted. RISC-V hands it over in the frame as mtval; here it
// stays in a peripheral register until someone asks, so asking is what this is.
#define ARM_SCB_BFAR  (*(volatile uint32_t *)0xE000ED38u)
#define MYRTOS_TRAP_FAULT(f)  ((void)(f), ARM_SCB_BFAR)

#endif
