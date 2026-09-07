#ifndef MYRTOS_CRITICAL_H
#define MYRTOS_CRITICAL_H

#include <stdint.h>
#include "hardware/sync.h"

// Critical sections, in one place instead of a dozen.
//
// The shape is ThreadX's, from ports/cortex_m33/gnu/inc/tx_port.h: a saved
// posture goes in and comes back out, and which register holds that posture is
// the port's business rather than the caller's. Ulf found it; the call sites
// here already had exactly that shape, so this is a change of name and not of
// code -- what it buys is that the POLICY now lives somewhere, and the policy is
// what will want changing.
//
// Today both machines do what they did before, instruction for instruction:
// PRIMASK on Arm and mstatus.MIE on RISC-V, through the SDK's own helpers. The
// interesting part is the switch below.

typedef uint32_t myrtos_critical_t;

// --- BASEPRI, WHICH IS OFF ------------------------------------------------
// Define MYRTOS_CRITICAL_BASEPRI to a priority VALUE to mask down to, and
// interrupts more urgent than it are never masked by the kernel at all --
// ARMv8-M has BASEPRI where ARMv6-M has only the hammer.
//
// The number is a raw priority register value, not a level. This part has four
// priority bits (__NVIC_PRIO_BITS is 4 on the RP2350), so priorities live in
// the top nibble: 0x00, 0x10 ... 0xF0. ThreadX writes the same thing as
// (level << (8 - bits)). An interrupt is masked when its value is numerically
// GREATER THAN OR EQUAL to BASEPRI, so a smaller number is more urgent.
//
// Where things sit here: every peripheral takes PICO_DEFAULT_IRQ_PRIORITY,
// which is 0x80, and the scheduler was deliberately put at the bottom, 0xF0 --
// see kernel/arm/stack.c for why. So 0x00 to 0x70 is empty, and
// MYRTOS_CRITICAL_BASEPRI 0x80 would leave all of it unmasked while masking
// everything that exists today.
//
// THE RULE THAT DOES NOT SHOW IN THE CODE. A handler above the threshold runs
// while the kernel is halfway through its own data structures, so it may not
// touch them: no system calls, no messages, no allocation, nothing that reaches
// the scheduler or the I/O manager. It owns its hardware and its own buffers
// and hands anything over through something lock-free. FreeRTOS calls the same
// threshold configMAX_SYSCALL_INTERRUPT_PRIORITY and the rule there is the
// same. Breaking it gives corruption that looks like anything but its cause.
//
// Nothing needs this yet. It is here so that the day a driver does -- something
// with a hard latency bound, where being held off by an allocator's walk is not
// acceptable -- the decision is one symbol rather than a sweep through the
// kernel.
#if defined(MYRTOS_CRITICAL_BASEPRI) && (defined(__arm__) || defined(__thumb__))

static inline myrtos_critical_t myrtos_critical_enter(void)
{
    myrtos_critical_t saved;
    __asm__ volatile ("mrs %0, basepri" : "=r"(saved));
    __asm__ volatile ("msr basepri, %0" :: "r"((uint32_t)MYRTOS_CRITICAL_BASEPRI) : "memory");
    return saved;
}

static inline void myrtos_critical_exit(myrtos_critical_t saved)
{
    __asm__ volatile ("msr basepri, %0" :: "r"(saved) : "memory");
}

#else

// The hammer, and what has always been here. On Arm this is PRIMASK, which
// masks everything but NMI and HardFault; on RISC-V it is mstatus.MIE, which
// masks everything full stop. The SDK's helpers rather than our own assembly,
// so that this path is provably the code that was here before.
static inline myrtos_critical_t myrtos_critical_enter(void)
{
    return save_and_disable_interrupts();
}

static inline void myrtos_critical_exit(myrtos_critical_t saved)
{
    restore_interrupts(saved);
}

#endif

#endif
