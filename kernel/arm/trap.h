#ifndef MYRTOS_ARM_TRAP_H
#define MYRTOS_ARM_TRAP_H

#include <stdint.h>

// The frame kernel/arm/scheduler.S builds, and it is two frames end to end.
//
// The core pushes the second half itself on the way into the exception -- r0
// to r3, r12, lr, pc and xPSR -- and pops it again on the way out. Software
// saves what is left, immediately below it, so that the two are one structure
// and a pointer to the bottom of it is the whole context. That is the same
// promise kernel/trap.h makes on RISC-V and the reason the handler's signature
// does not change: it is given a frame and answers with the frame to resume.
//
// The shape does change, and there is no honest way around it. There are no
// mcause, mepc and mtval to store; the exception number comes from IPSR, the
// return address is pc in the half the hardware pushed, and the fault detail
// lives in CFSR where the C side reads it directly. Seventeen words against
// thirty-three, because half the work is the core's -- ten words pushed by
// scheduler.S and eight by the hardware.
typedef struct {
    uint32_t cause;        // IPSR: which exception arrived
    uint32_t r4, r5, r6, r7, r8, r9, r10, r11;

    // Not a return address. EXC_RETURN says which stack to resume on and
    // whether floating-point state was stacked, and the call to the handler
    // destroys lr, so it is saved and put back like any other register.
    uint32_t exc_return;

    // From here down the core did the work, and will undo it.
    uint32_t r0, r1, r2, r3;
    uint32_t r12;
    uint32_t lr;           // the interrupted code's own return address
    uint32_t pc;           // where it will resume: mepc's opposite number
    uint32_t xpsr;
} myrtos_frame_t;

_Static_assert(sizeof(myrtos_frame_t) == 72, "the frame must match scheduler.S");

// Where the arguments to a system call are, and where its answer goes. On RISC-V
// this is a0..a7 and the handler indexes them by name; here it is r0..r3, and a
// call with more arguments than that reads them from the stack. Naming them
// once means the syscall layer can be written without knowing which machine it
// is on.
#define MYRTOS_FRAME_ARG0(f)   ((f)->r0)
#define MYRTOS_FRAME_ARG1(f)   ((f)->r1)
#define MYRTOS_FRAME_ARG2(f)   ((f)->r2)
#define MYRTOS_FRAME_ARG3(f)   ((f)->r3)
#define MYRTOS_FRAME_RESULT(f) ((f)->r0)
#define MYRTOS_FRAME_PC(f)     ((f)->pc)

#endif
