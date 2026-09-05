// The tick, on Cortex-M33. The other half of this is at the bottom of
// kernel/scheduler.c, where the same three functions read the RISC-V machine
// timer.
//
// The contract is unchanged: a periodic interrupt at a stated interval, and a
// free-running clock that never wraps in any life this machine will have. What
// changes is that neither comes from one device here.
#include <stdint.h>

// SysTick, in the private peripheral block every Cortex-M has.
#define SYST_CSR    (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR    (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR    (*(volatile uint32_t *)0xE000E018u)

#define SYST_ENABLE     (1u << 0)
#define SYST_TICKINT    (1u << 1)
#define SYST_CLKSOURCE  (1u << 2)   // set: processor clock. clear: reference.

// The RP2350's microsecond timer, which both architectures can read and which
// is what makes "interval_ticks" mean the same thing on both. It counts from
// the TICKS block, so that block must be running -- the SDK's clock setup does
// it, and nothing here can if it has not.
#define TIMER0_BASE 0x400b0000u
#define TIMERAWH    (*(volatile uint32_t *)(TIMER0_BASE + 0x24u))
#define TIMERAWL    (*(volatile uint32_t *)(TIMER0_BASE + 0x28u))

// Read high, low, high again, and accept it when the high half did not move
// under us. The same shape as mtime_read on the other side, and for the same
// reason: two registers, one number.
static uint64_t timer_read(void)
{
    uint32_t hi, lo, hi2;
    do { hi = TIMERAWH; lo = TIMERAWL; hi2 = TIMERAWH; } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

// Nothing to do, and that is the difference worth naming. The machine timer on
// RISC-V is a comparator: it fires once and the handler must set the next
// deadline, so myrtos_timer_rearm exists. SysTick reloads itself. The call
// stays because the handler that makes it should not have to know which
// machine it is on.
void myrtos_timer_rearm(void) { }

void myrtos_timer_init(uint32_t interval_ticks)
{
    // Microseconds, from the reference clock rather than the processor clock,
    // so the interval means what it means on RISC-V without anyone having to
    // know what clk_sys was set to.
    //
    // The reload is twenty-four bits, so the longest tick this can ask for is
    // about 16.7 seconds. A quantum is a millisecond.
    uint32_t reload = interval_ticks ? interval_ticks - 1u : 0u;
    if (reload > 0x00ffffffu) reload = 0x00ffffffu;

    SYST_CSR = 0;                    // stop before changing anything
    SYST_RVR = reload;
    SYST_CVR = 0;                    // writing any value clears it, and the
                                     // flag with it, so the first tick is whole
    SYST_CSR = SYST_ENABLE | SYST_TICKINT;

    __asm__ volatile("cpsie i" ::: "memory");
}

uint64_t myrtos_timer_now(void) { return timer_read(); }
