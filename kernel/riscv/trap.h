#ifndef MYRTOS_RISCV_TRAP_H
#define MYRTOS_RISCV_TRAP_H

#include <stdint.h>

// Must match the save order in myrtos_trap_vector (kernel/scheduler.S) exactly.
// FRAME_SIZE there is 144, i.e. 33 words plus padding to 16-byte alignment.
//
// a0 to a7 keep their machine names because on this machine they are the same
// registers the syscall ABI names: the number in a7, the arguments in a0 to a2,
// the answer back in a0. pc, cause and fault do not -- they were mepc, mcause
// and mtval, and the code that reads them is the same code on both machines.
typedef struct {
    uint32_t ra, gp, tp;
    uint32_t t0, t1, t2;
    uint32_t s0, s1;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t t3, t4, t5, t6;
    uint32_t pc, cause, fault;
    uint32_t _pad[3];
} myrtos_frame_t;

_Static_assert(sizeof(myrtos_frame_t) == 144, "the frame must match FRAME_SIZE");

#define MCAUSE_INTERRUPT_BIT    0x80000000u
#define MCAUSE_CODE_MASK        0x7fffffffu
#define MCAUSE_ECALL_M          11u
#define MCAUSE_BREAKPOINT        3u
#define MCAUSE_MACHINE_TIMER     7u

// What happened, asked in the way the shared code asks it.
#define MYRTOS_TRAP_IS_INTERRUPT(f) (((f)->cause & MCAUSE_INTERRUPT_BIT) != 0)
#define MYRTOS_TRAP_IS_TIMER(f)     (MYRTOS_TRAP_IS_INTERRUPT(f) && \
                                     ((f)->cause & MCAUSE_CODE_MASK) == MCAUSE_MACHINE_TIMER)
#define MYRTOS_TRAP_IS_SYSCALL(f)   (!MYRTOS_TRAP_IS_INTERRUPT(f) && \
                                     ((f)->cause & MCAUSE_CODE_MASK) == MCAUSE_ECALL_M)
#define MYRTOS_TRAP_IS_BREAKPOINT(f) (!MYRTOS_TRAP_IS_INTERRUPT(f) && \
                                     ((f)->cause & MCAUSE_CODE_MASK) == MCAUSE_BREAKPOINT)

// Past the ecall, and back onto it. mepc points AT the instruction that
// trapped, so a system call that has been served must step over it, and one
// that must be made again -- a read with nothing to read -- steps back. Four
// bytes, because ecall has no compressed form.
#define MYRTOS_TRAP_SKIP(f)  ((f)->pc += 4)
#define MYRTOS_TRAP_REDO(f)  ((f)->pc -= 4)

// The address that faulted, which this machine hands over in the frame.
#define MYRTOS_TRAP_FAULT(f)  ((f)->fault)

#endif
