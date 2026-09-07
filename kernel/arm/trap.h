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
#define MYRTOS_CAUSE_IS_SYSCALL(c)   ((c) == ARM_EXC_SVCALL)
#define MYRTOS_TRAP_IS_SYSCALL(f)    MYRTOS_CAUSE_IS_SYSCALL((f)->cause)

// A breakpoint is not a cause of its own here. With no debugger attached bkpt
// raises a debug monitor exception that nobody has enabled, so it escalates --
// and arrives as a HardFault, indistinguishable by number from a bad pointer.
//
// What tells them apart is HFSR.DEBUGEVT, which is set for the escalated
// breakpoint and for nothing else. Asking that register rather than reading the
// instruction at pc matters: a HardFault's pc may be exactly the wild address
// that caused it, and a second fault inside the fault handler is a lockup.
#define ARM_SCB_HFSR  (*(volatile uint32_t *)0xE000ED2Cu)
#define ARM_HFSR_DEBUGEVT  0x80000000u
#define MYRTOS_TRAP_IS_BREAKPOINT(f) ((f)->cause == ARM_EXC_HARDFAULT && \
                                      (ARM_SCB_HFSR & ARM_HFSR_DEBUGEVT) != 0)

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
// The address that faulted. RISC-V hands it over in the frame as mtval; here it
// stays in a peripheral register until someone asks -- and worse, the register
// only holds an address when a bit elsewhere says it does. Printing BFAR
// unconditionally is printing whatever was left there by an earlier fault, or
// by nothing at all, which is how the first ARM crash reported a fault address
// of 0xE000ED38: the register had never been written.
//
// So ask CFSR first. If neither address is valid there is no address, and CFSR
// itself is the more useful number -- it says what kind of fault it was.
#define ARM_SCB_CFSR   (*(volatile uint32_t *)0xE000ED28u)
#define ARM_SCB_MMFAR  (*(volatile uint32_t *)0xE000ED34u)
#define ARM_SCB_BFAR   (*(volatile uint32_t *)0xE000ED38u)
#define ARM_CFSR_MMARVALID  (1u << 7)
#define ARM_CFSR_BFARVALID  (1u << 15)

static inline uint32_t myrtos_arm_fault_address(void)
{
    uint32_t cfsr = ARM_SCB_CFSR;
    if (cfsr & ARM_CFSR_BFARVALID) return ARM_SCB_BFAR;
    if (cfsr & ARM_CFSR_MMARVALID) return ARM_SCB_MMFAR;
    return cfsr;
}

#define MYRTOS_TRAP_FAULT(f)  ((void)(f), myrtos_arm_fault_address())

// Past the breakpoint. Two bytes always: bkpt has no wide form, where RISC-V
// has to read the instruction to find out which ebreak it was. The sticky bit
// in HFSR is written back to clear it, or the next real HardFault would look
// like another assertion and be stepped over into whatever follows.
#define MYRTOS_TRAP_STEP_BREAKPOINT(f) do { \
        ARM_SCB_HFSR = ARM_HFSR_DEBUGEVT; \
        (f)->pc += 2u; \
    } while (0)

// Thread mode, on the process stack, with no floating-point state stacked.
//
// That last part is about a NEW process and nothing more. It used to claim the
// FPU was switched off and every module soft-floated, and both halves were
// wrong: the SDK enables CP10 unconditionally, this port does not opt out, and
// gcc reaches for d8 to move eight-byte blocks in code that does no arithmetic
// at all. What is true is that a process which has never executed a
// floating-point instruction has no floating-point context, so the frame it
// starts from names none -- and the core sets FType itself the moment that
// stops being true. See the note in scheduler.S about s16 to s31.
#define ARM_EXC_RETURN_THREAD_PSP  0xFFFFFFFDu

// The Thumb bit in xPSR. Without it the first instruction faults, and the fault
// says nothing about why.
#define ARM_XPSR_THUMB             0x01000000u

// Lay out a frame so that resuming it starts the process, as though it had been
// interrupted immediately before its first instruction.
//
// Three things here have no counterpart on RISC-V. The stacked pc must have bit
// zero CLEAR even though every Thumb function pointer has it set -- the
// instruction set comes from xPSR, and a pc with the bit still in it faults.
// EXC_RETURN has to be manufactured, because there was no exception to take one
// from. And there is no gp: the kernel is reached through svc rather than
// through a register.
//
// r9 stands in for tp. It is the static base by ARM convention, it is saved and
// restored with the frame like any other register, and __aeabi_read_tp is a
// two-instruction function that hands it back -- which is what the compiler
// calls for __thread.
// Bytes reserved before the thread-local block.
//
// The ARM TLS ABI puts a thread control block first, and the linker knows it:
// __aeabi_read_tp is defined to return the address of that block, and a
// local-exec relocation is resolved to eight plus the variable's offset within
// the block. Reserve nothing and a module writes eight bytes past the end of
// what the kernel prepared -- which zeroes the wrong four bytes and leaves the
// variable holding whatever the previous process left there. zhello's counter
// came back as 4, 5, 6 across three separate runs instead of 1, 1, 1.
//
// Nothing uses the eight bytes. They are the ABI's, and the price of them is
// eight bytes per process.
#define MYRTOS_TLS_TCB_BYTES  8u

static inline void myrtos_frame_start(myrtos_frame_t *f, uintptr_t entry,
                                      uintptr_t ret, uint32_t a0, uint32_t a1,
                                      uint32_t tls)
{
    f->pc   = (uint32_t)entry & ~1u;
    f->lr   = (uint32_t)ret;
    f->a0   = a0;
    f->a1   = a1;
    f->r9   = tls;
    f->xpsr = ARM_XPSR_THUMB;
    f->exc_return = ARM_EXC_RETURN_THREAD_PSP;
}

// The kernel stops being the kernel and becomes the idle process: PSP takes
// over the stack it is standing on, and MSP moves to an interrupt stack of its
// own. Why that second half is not optional is written out in kernel/arm/stack.c,
// where the interrupt stack lives.
void myrtos_arch_become_process(void);

#endif
