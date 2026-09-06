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
// Asked of a bare cause as well as of a frame, because the trap self-test at
// startup has only the number: it makes one call and then wants to know that
// what came back was the system call it made.
#define MYRTOS_CAUSE_IS_SYSCALL(c)  (((c) & MCAUSE_INTERRUPT_BIT) == 0 && \
                                     ((c) & MCAUSE_CODE_MASK) == MCAUSE_ECALL_M)
#define MYRTOS_TRAP_IS_SYSCALL(f)   MYRTOS_CAUSE_IS_SYSCALL((f)->cause)
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

// Past the breakpoint, so an assertion returns false instead of parking the
// board. The instruction's own low two bits say how wide it is: compressed
// ebreak is two bytes and the wide one is four. Reading the instruction is safe
// here and only here -- the cause has already said a breakpoint executed at
// this address, so the address is one that fetched.
#define MYRTOS_TRAP_STEP_BREAKPOINT(f) do { \
        uint16_t _insn = *(const uint16_t *)(uintptr_t)(f)->pc; \
        (f)->pc += ((_insn & 3u) == 3u) ? 4u : 2u; \
    } while (0)

// The global pointer every process runs with. Captured from the kernel once at
// startup, because on this machine gp addresses the kernel's own small data and
// a process that does not have it cannot call into the kernel at all.
extern uint32_t myrtos_kernel_gp;

// Lay out a frame so that resuming it starts the process, as though it had been
// interrupted immediately before its first instruction. That is the whole trick
// and it is why there is no separate "start a process" path in the scheduler.
static inline void myrtos_frame_start(myrtos_frame_t *f, uintptr_t entry,
                                      uintptr_t ret, uint32_t a0, uint32_t a1,
                                      uint32_t tls)
{
    f->pc = (uint32_t)entry;
    f->ra = (uint32_t)ret;
    f->a0 = a0;
    f->a1 = a1;
    f->gp = myrtos_kernel_gp;
    f->tp = tls;
}

// Nothing to become. One stack pointer serves both, and the kernel is already
// running on the stack the first timer trap will save a frame onto. See the
// ARM half for what this costs on a machine with two stack pointers.
static inline void myrtos_arch_become_process(void) { }

#endif
