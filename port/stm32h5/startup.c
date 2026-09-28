// Reset and the vector table for the STM32H5.
//
// What the pico-sdk's runtime does before main on the RP2350, done here by
// hand: the data copied in from flash, the bss cleared, the FPU turned on, the
// clocks started. Nothing of ST's HAL is used -- only the register definitions
// from CMSIS -- so what runs is what is written in this directory.

#include <stdint.h>
#include "stm32h5xx.h"
#include "port.h"

extern uint32_t __data_start, __data_end, __data_load;
extern uint32_t __bss_start, __bss_end;
extern uint32_t __stack_top;

int main(void);

// Anything that arrives with no handler of its own stops here, with the
// exception number in reach of a debugger: IPSR says which one it was.
void ubiqos_default_handler(void)
{
    for (;;) __asm__ volatile("bkpt #0");
}

#define WEAK_HANDLER(name) \
    void name(void) __attribute__((weak, alias("ubiqos_default_handler")))

WEAK_HANDLER(NMI_Handler);
WEAK_HANDLER(HardFault_Handler);
WEAK_HANDLER(MemManage_Handler);
WEAK_HANDLER(BusFault_Handler);
WEAK_HANDLER(UsageFault_Handler);
WEAK_HANDLER(SecureFault_Handler);
WEAK_HANDLER(SVC_Handler);
WEAK_HANDLER(DebugMon_Handler);
WEAK_HANDLER(PendSV_Handler);
WEAK_HANDLER(SysTick_Handler);

void ubiqos_reset(void);

// 16 system entries and the chip's interrupts. The H563's last is number 130
// in ST's IRQn_Type, so 131 of them; the rest of the table is the default
// handler until something claims an entry.
#define H5_IRQ_COUNT 131

typedef void (*vector_t)(void);

__attribute__((section(".vectors"), used))
const vector_t ubiqos_vectors[16 + H5_IRQ_COUNT] = {
    [0]  = (vector_t)&__stack_top,
    [1]  = ubiqos_reset,
    [2]  = NMI_Handler,
    [3]  = HardFault_Handler,
    [4]  = MemManage_Handler,
    [5]  = BusFault_Handler,
    [6]  = UsageFault_Handler,
    [7]  = SecureFault_Handler,
    [11] = SVC_Handler,
    [12] = DebugMon_Handler,
    [14] = PendSV_Handler,
    [15] = SysTick_Handler,
    [16 ... 16 + H5_IRQ_COUNT - 1] = ubiqos_default_handler,
};

void ubiqos_reset(void)
{
    // The stack pointer came from the table; its limit did not. MSPLIM is the
    // same guard PSPLIM is for a process -- this is a Cortex-M33 like the
    // RP2350's -- and costs nothing to set.
    extern uint32_t __stack_limit;
    __set_MSPLIM((uint32_t)&__stack_limit);

    uint32_t *src = &__data_load, *dst = &__data_start;
    while (dst < &__data_end) *dst++ = *src++;
    for (dst = &__bss_start; dst < &__bss_end; dst++) *dst = 0;

    // CP10 and CP11, full access: the FPU. The compiler is free to use it for
    // an ordinary 8-byte copy, so it is on before any C that could.
    SCB->CPACR |= (3u << 20) | (3u << 22);
    __DSB();
    __ISB();

    SCB->VTOR = (uint32_t)ubiqos_vectors;

    h5_clock_init();
    main();
    for (;;) __WFI();
}
