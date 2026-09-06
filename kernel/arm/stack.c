// The interrupt stack, and the moment the kernel stops being the kernel.
//
// RISC-V needs neither. One stack pointer serves thread and trap alike: a trap
// pushes its frame onto whatever stack was running and the handler carries on
// below it. Cortex-M has two, and a handler always runs on MSP -- so the two
// have to be told apart, and given somewhere separate to live.
//
// The first attempt gave PSP the stack the kernel was already on and left MSP
// pointing at the same address. myrtos's own vector survived that by moving MSP
// down to the frame it had just saved, which was true to how RISC-V works and
// wrong about everything else in the system: every SDK interrupt handler runs
// on MSP too. The USB device interrupt arrived, the core stacked its frame just
// below the shared address, the SDK's handler started writing its own locals
// from that same address downwards, and the frame it was going to return
// through was gone. On hardware that read
//
//     USB device started, CDC console on the USB port
//     *** MYRTOS TRAP: unhandled exception ***
//       pc 4294967292  cause 3
//
// -- pc 0xFFFFFFFC, a return into nothing. Two stack pointers need two stacks.
#include <stdint.h>

// Deep enough for the deepest system call, because that is what runs here: the
// handler and everything it calls, including the file server request path. The
// process stacks no longer carry any of it -- on this machine they hold the
// 72-byte frame and nothing more, where on RISC-V they carry the kernel's work
// as well. If this is ever too small the symptom will be a fault with a pc
// inside the kernel and a stack pointer just below myrtos_irq_stack.
#define MYRTOS_IRQ_STACK_WORDS 1024   // 4 kB

__attribute__((aligned(8)))
uint32_t myrtos_irq_stack[MYRTOS_IRQ_STACK_WORDS];

void myrtos_arch_become_process(void)
{
    // PSP takes the stack we are standing on, so nothing is copied and no local
    // goes out from under us -- only the name of the pointer changes. CONTROL
    // bit 1 is SPSEL; bit 0 stays clear so thread mode remains privileged,
    // which the kernel-as-idle-process needs. The ISB is required, because
    // CONTROL does not apply to instructions already in the pipeline.
    //
    // MSP moves only after SPSEL has taken effect. Until then MSP is the stack
    // under our feet, and writing it would be writing the ground away.
    register uint32_t *top = myrtos_irq_stack + MYRTOS_IRQ_STACK_WORDS;
    __asm__ volatile(
        "mrs  r0, msp\n"
        "msr  psp, r0\n"
        "movs r0, #2\n"
        "msr  control, r0\n"
        "isb\n"
        "msr  msp, %0\n"
        :
        : "r"(top)
        : "r0", "memory");
}
