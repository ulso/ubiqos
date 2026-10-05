// Time on the STM32H5: the free-running microsecond clock and the tick.
//
// The RP2350 has a 64-bit microsecond timer in hardware, and the kernel was
// written against one -- time_us_64 in the log's stamps, ubiqos_timer_now in the
// scheduler. This chip has no such thing, so it is made: TIM2, a 32-bit timer,
// counts microseconds, and its overflow interrupt counts the top half. A read
// that races the overflow is caught by looking at the pending flag, so the
// answer is right with interrupts on or off.
//
// The tick is SysTick, as on the RP2350 -- kernel/arm/timer.c is the same three
// functions for that chip. Only the reference clock differs: 1 MHz there, the
// core clock divided by eight here.

#include "stm32h5xx.h"
#include "port.h"
#include "pico/time.h"

static volatile uint32_t us_high;

// APB1 timers run at twice PCLK1 whenever APB1 is divided at all -- RCC's TIMPRE
// left at its reset value -- and at PCLK1 when it is not.
static uint32_t tim2_clock_hz(void)
{
    return stm32_pclk1_hz == stm32_sysclk_hz ? stm32_pclk1_hz : 2u * stm32_pclk1_hz;
}

void stm32_time_init(void)
{
    RCC->APB1LENR |= RCC_APB1LENR_TIM2EN;
    (void)RCC->APB1LENR;

    TIM2->CR1 = 0;
    TIM2->PSC = tim2_clock_hz() / 1000000u - 1u;
    TIM2->ARR = 0xFFFFFFFFu;
    TIM2->CNT = 0;
    TIM2->EGR = TIM_EGR_UG;          // load the prescaler now, not at the first wrap
    TIM2->SR  = 0;                   // and forget the update that loading made
    TIM2->DIER = TIM_DIER_UIE;
    TIM2->CR1 = TIM_CR1_CEN;

    // Below the kernel's critical sections (0x80 masks 0x80 and lower), with the
    // other devices: a wrap every 71 minutes can wait out any of them.
    NVIC_SetPriority(TIM2_IRQn, 0x80u >> (8u - __NVIC_PRIO_BITS));
    NVIC_EnableIRQ(TIM2_IRQn);
}

void TIM2_IRQHandler(void)
{
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR = ~TIM_SR_UIF;
        us_high++;
    }
}

uint64_t time_us_64(void)
{
    const uint32_t st = __get_PRIMASK();
    __disable_irq();
    uint32_t hi = us_high;
    uint32_t lo = TIM2->CNT;
    // Wrapped, and the interrupt that counts it has not run -- because it is
    // masked, or because it is about to. Read the counter again after seeing
    // the flag, so that a wrap between the two reads is not counted twice.
    if (TIM2->SR & TIM_SR_UIF) {
        lo = TIM2->CNT;
        hi++;
    }
    __set_PRIMASK(st);
    return ((uint64_t)hi << 32) | lo;
}

void busy_wait_us(uint64_t us)
{
    const uint64_t end = time_us_64() + us;
    while (time_us_64() < end) { }
}

// --- the tick ---------------------------------------------------------------

void ubiqos_timer_rearm(void) { }       // SysTick reloads itself

void ubiqos_timer_init(uint32_t interval_us)
{
    uint32_t per_us = stm32_sysclk_hz / 8u / 1000000u;
    uint32_t reload = interval_us * per_us;
    reload = reload ? reload - 1u : 0u;
    if (reload > 0x00ffffffu) reload = 0x00ffffffu;

    SysTick->CTRL = 0;
    SysTick->LOAD = reload;
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk;   // core clock / 8

    __enable_irq();
}

uint64_t ubiqos_timer_now(void) { return time_us_64(); }
