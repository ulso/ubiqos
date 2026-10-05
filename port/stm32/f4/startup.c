// Reset and the vector table for the STM32F4.
//
// The H5's startup with the F4's table: no SecureFault, an ARMv8-M exception
// the Cortex-M4 does not have, and 82 interrupts in place of 131. The rest --
// data copied in, bss cleared, FPU on, clocks started -- is the same, and so is
// the reason: nothing of ST's HAL, only CMSIS's register definitions.

#include <stdint.h>
#include "stm32f4xx.h"
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

// The four the kernel takes -- kernel/arm/scheduler.S, one body for all four.
WEAK_HANDLER(isr_hardfault);
WEAK_HANDLER(isr_svcall);
WEAK_HANDLER(isr_pendsv);
WEAK_HANDLER(isr_systick);

WEAK_HANDLER(NMI_Handler);
WEAK_HANDLER(MemManage_Handler);
WEAK_HANDLER(BusFault_Handler);
WEAK_HANDLER(UsageFault_Handler);
WEAK_HANDLER(DebugMon_Handler);

// The port's own device interrupts: the microsecond clock's overflow, the
// console, and USB.
WEAK_HANDLER(TIM2_IRQHandler);
WEAK_HANDLER(USART3_IRQHandler);
WEAK_HANDLER(OTG_FS_IRQHandler);

void ubiqos_reset(void);

// 16 system entries and the chip's interrupts: the F405's last is the FPU's,
// number 81 in ST's IRQn_Type.
#define F4_IRQ_COUNT (FPU_IRQn + 1)

typedef void (*vector_t)(void);

#pragma GCC diagnostic ignored "-Woverride-init"

__attribute__((section(".vectors"), used))
const vector_t ubiqos_vectors[16 + F4_IRQ_COUNT] = {
    [0]  = (vector_t)&__stack_top,
    [1]  = ubiqos_reset,
    [2]  = NMI_Handler,
    [3]  = isr_hardfault,
    [4]  = MemManage_Handler,
    [5]  = BusFault_Handler,
    [6]  = UsageFault_Handler,
    [11] = isr_svcall,
    [12] = DebugMon_Handler,
    [14] = isr_pendsv,
    [15] = isr_systick,
    [16 ... 16 + F4_IRQ_COUNT - 1] = ubiqos_default_handler,
    [16 + TIM2_IRQn]   = TIM2_IRQHandler,
    [16 + USART3_IRQn] = USART3_IRQHandler,
    [16 + OTG_FS_IRQn] = OTG_FS_IRQHandler,
};

void ubiqos_reset(void)
{
    uint32_t *src = &__data_load, *dst = &__data_start;
    while (dst < &__data_end) *dst++ = *src++;
    for (dst = &__bss_start; dst < &__bss_end; dst++) *dst = 0;

    // CP10 and CP11, full access: the FPU, before any C that could use it.
    SCB->CPACR |= (3u << 20) | (3u << 22);
    __DSB();
    __ISB();

    SCB->VTOR = (uint32_t)ubiqos_vectors;

    stm32_clock_init();
    main();
    for (;;) __WFI();
}
