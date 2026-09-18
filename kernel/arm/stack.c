// The interrupt stack, and the moment the kernel stops being the kernel.
//
// RISC-V needs neither. One stack pointer serves thread and trap alike: a trap
// pushes its frame onto whatever stack was running and the handler carries on
// below it. Cortex-M has two, and a handler always runs on MSP -- so the two
// have to be told apart, and given somewhere separate to live.
//
// The first attempt gave PSP the stack the kernel was already on and left MSP
// pointing at the same address. UbiqOS's own vector survived that by moving MSP
// down to the frame it had just saved, which was true to how RISC-V works and
// wrong about everything else in the system: every SDK interrupt handler runs
// on MSP too. The USB device interrupt arrived, the core stacked its frame just
// below the shared address, the SDK's handler started writing its own locals
// from that same address downwards, and the frame it was going to return
// through was gone. On hardware that read
//
//     USB device started, CDC console on the USB port
//     *** UBIQOS TRAP: unhandled exception ***
//       pc 4294967292  cause 3
//
// -- pc 0xFFFFFFFC, a return into nothing. Two stack pointers need two stacks.
#include <stdint.h>

// Deep enough for the deepest system call, because that is what runs here: the
// handler and everything it calls, including the file server request path. The
// process stacks no longer carry any of it -- on this machine they hold the
// 72-byte frame and nothing more, where on RISC-V they carry the kernel's work
// as well. If this is ever too small the symptom will be a fault with a pc
// inside the kernel and a stack pointer just below ubiqos_irq_stack.
#define UBIQOS_IRQ_STACK_WORDS 1024   // 4 kB

__attribute__((aligned(8)))
uint32_t ubiqos_irq_stack[UBIQOS_IRQ_STACK_WORDS];

// System handler priority registers. SVCall is the top byte of SHPR2; PendSV
// and SysTick are the third and top bytes of SHPR3.
#define ARM_SCB_SHPR2  (*(volatile uint32_t *)0xE000ED1Cu)
#define ARM_SCB_SHPR3  (*(volatile uint32_t *)0xE000ED20u)

// The scheduler must not outrank the devices, and on this machine it does by
// default: every system handler resets to priority 0, the highest there is,
// while the SDK gives every peripheral interrupt PICO_DEFAULT_IRQ_PRIORITY,
// which is 0x80. So SysTick -- once a millisecond, for ever -- preempts every
// driver in the system.
//
// For most drivers that is merely rude. For the USB host it is fatal. PIO-USB
// receives a packet in a tight loop against the state machine's FIFO, and that
// loop cannot be paused: a scheduler pass in the middle of an IN transfer is
// tens of microseconds, the packet is gone, and the endpoint's failed_count
// climbs. It looked exactly like a keyboard that enumerates, mounts, arms its
// endpoint, gets polled for ever and never delivers a report.
//
// RISC-V never had this. Hazard3 does not put the machine timer above external
// interrupts, so the same code came out the other way round -- which is why
// this is written here and has no counterpart on that side.
//
// 0xFF is the lowest priority the machine offers whatever number of priority
// bits it implements. Delaying a tick behind a device is the right trade: the
// tick is a quantum boundary and can wait, and a USB packet cannot.
#define UBIQOS_SCHED_PRIORITY  0xFFu

void ubiqos_arch_become_process(void)
{
    ARM_SCB_SHPR2 = (ARM_SCB_SHPR2 & 0x00FFFFFFu) | (UBIQOS_SCHED_PRIORITY << 24);
    ARM_SCB_SHPR3 = (ARM_SCB_SHPR3 & 0x0000FFFFu)
                  | (UBIQOS_SCHED_PRIORITY << 16)     // PendSV
                  | (UBIQOS_SCHED_PRIORITY << 24);    // SysTick

    // PSP takes the stack we are standing on, so nothing is copied and no local
    // goes out from under us -- only the name of the pointer changes. CONTROL
    // bit 1 is SPSEL; bit 0 stays clear so thread mode remains privileged,
    // which the kernel-as-idle-process needs. The ISB is required, because
    // CONTROL does not apply to instructions already in the pipeline.
    //
    // MSP moves only after SPSEL has taken effect. Until then MSP is the stack
    // under our feet, and writing it would be writing the ground away.
    register uint32_t *top = ubiqos_irq_stack + UBIQOS_IRQ_STACK_WORDS;
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
