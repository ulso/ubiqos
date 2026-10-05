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

// The four the kernel takes, under the names it gives them on the RP2350 --
// kernel/arm/scheduler.S, one body for all four, IPSR saying which. Weak, so
// the test program links without a kernel.
WEAK_HANDLER(isr_hardfault);
WEAK_HANDLER(isr_svcall);
WEAK_HANDLER(isr_pendsv);
WEAK_HANDLER(isr_systick);

WEAK_HANDLER(NMI_Handler);
WEAK_HANDLER(MemManage_Handler);
WEAK_HANDLER(BusFault_Handler);
WEAK_HANDLER(UsageFault_Handler);
WEAK_HANDLER(SecureFault_Handler);
WEAK_HANDLER(DebugMon_Handler);

// The port's own device interrupts: the microsecond clock's overflow and the
// console. Direct handlers, not through the kernel's trap -- the SDK's
// interrupt handlers work the same way on the RP2350.
WEAK_HANDLER(TIM2_IRQHandler);
WEAK_HANDLER(USART3_IRQHandler);

void ubiqos_reset(void);

// 16 system entries and the chip's interrupts. The H563's last is number 130
// in ST's IRQn_Type, so 131 of them; the rest of the table is the default
// handler until something claims an entry.
#define H5_IRQ_COUNT 131

typedef void (*vector_t)(void);

// The range fills every entry with the default, and the two after it replace
// theirs: GCC warns about the replacement under -Wextra, and it is the point.
#pragma GCC diagnostic ignored "-Woverride-init"

__attribute__((section(".vectors"), used))
const vector_t ubiqos_vectors[16 + H5_IRQ_COUNT] = {
    [0]  = (vector_t)&__stack_top,
    [1]  = ubiqos_reset,
    [2]  = NMI_Handler,
    [3]  = isr_hardfault,
    [4]  = MemManage_Handler,
    [5]  = BusFault_Handler,
    [6]  = UsageFault_Handler,
    [7]  = SecureFault_Handler,
    [11] = isr_svcall,
    [12] = DebugMon_Handler,
    [14] = isr_pendsv,
    [15] = isr_systick,
    [16 ... 16 + H5_IRQ_COUNT - 1] = ubiqos_default_handler,
    [16 + TIM2_IRQn]   = TIM2_IRQHandler,
    [16 + USART3_IRQn] = USART3_IRQHandler,
};

void ubiqos_reset(void)
{
    // MSPLIM is left at zero, as the RP2350's startup leaves it. The kernel
    // moves MSP to its own interrupt stack when it becomes the idle process
    // (kernel/arm/stack.c), and a limit set here for the startup stack would
    // then sit above the new one and fault the first exception.

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
