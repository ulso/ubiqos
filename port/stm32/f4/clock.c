// The STM32F4's clocks: the main PLL from HSE, at whatever the board header
// asks for.
//
// The board says three things -- UBIQOS_STM32_HSE_HZ, UBIQOS_STM32_HSE_BYPASS
// and UBIQOS_STM32_SYSCLK_HZ -- and the PLL is worked out from them: HSE
// divided down to a 2 MHz reference, the reference manual's choice for the
// least jitter, multiplied to twice the system clock, divided by two. The VCO
// must also divide down to 48 MHz exactly, for USB, SDIO and the RNG. On the
// Feather STM32F405 that is a 12 MHz crystal to 168 MHz, the chip's limit, and
// a VCO of 336 MHz divided by seven.
//
// If HSE never comes up the chip stays on HSI, 16 MHz. Slower, but a console
// that works at the wrong speed says why; one that never starts says nothing.

#include "stm32f4xx.h"
#include "port.h"
#include "board.h"

#if !defined(UBIQOS_STM32_HSE_HZ) || !defined(UBIQOS_STM32_HSE_BYPASS) || !defined(UBIQOS_STM32_SYSCLK_HZ)
#error "the board header must say UBIQOS_STM32_HSE_HZ, UBIQOS_STM32_HSE_BYPASS and UBIQOS_STM32_SYSCLK_HZ"
#endif
#define PLL_REF_HZ 2000000u
#define PLL_VCO_HZ (UBIQOS_STM32_SYSCLK_HZ * 2u)
#define PLL_M (UBIQOS_STM32_HSE_HZ / PLL_REF_HZ)
#define PLL_N (PLL_VCO_HZ / PLL_REF_HZ)
#define PLL_Q (PLL_VCO_HZ / 48000000u)
_Static_assert(UBIQOS_STM32_HSE_HZ % PLL_REF_HZ == 0, "HSE must be a multiple of 2 MHz");
_Static_assert(PLL_VCO_HZ % PLL_REF_HZ == 0, "the system clock must be a multiple of 1 MHz");
_Static_assert(PLL_VCO_HZ >= 100000000u && PLL_VCO_HZ <= 432000000u, "the VCO runs at 100 to 432 MHz");
_Static_assert(PLL_VCO_HZ % 48000000u == 0, "the VCO must divide down to 48 MHz exactly");
_Static_assert(PLL_M >= 2 && PLL_M <= 63, "PLLM is 2 to 63");
_Static_assert(PLL_N >= 50 && PLL_N <= 432, "PLLN is 50 to 432");
_Static_assert(PLL_Q >= 2 && PLL_Q <= 15, "PLLQ is 2 to 15");
_Static_assert(UBIQOS_STM32_SYSCLK_HZ <= 168000000u, "168 MHz is the STM32F405's limit");

uint32_t stm32_sysclk_hz = 16000000u;
uint32_t stm32_pclk1_hz  = 16000000u;
bool     stm32_clock_from_hse;

#define HSE_TIMEOUT 1000000u    // loop turns, some tens of milliseconds at 16 MHz

void stm32_clock_init(void)
{
    // Scale 1, the highest core voltage, which 168 MHz needs. It is the reset
    // value on the F405; written anyway, so that nothing before us matters.
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;
    PWR->CR |= PWR_CR_VOS;

    RCC->CR = (RCC->CR & ~RCC_CR_HSEBYP) | (UBIQOS_STM32_HSE_BYPASS ? RCC_CR_HSEBYP : 0u);
    RCC->CR |= RCC_CR_HSEON;
    uint32_t n = 0;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
        if (++n == HSE_TIMEOUT) {
            RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_HSEBYP);
            return;
        }
    }

    // The main PLL: HSE / M is 2 MHz, times N is the VCO, / P=2 is the system
    // clock and / Q is 48 MHz.
    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) { }
    RCC->PLLCFGR = (PLL_M << RCC_PLLCFGR_PLLM_Pos)
                 | (PLL_N << RCC_PLLCFGR_PLLN_Pos)
                 | (0u    << RCC_PLLCFGR_PLLP_Pos)       // 0 is divide by two
                 | RCC_PLLCFGR_PLLSRC_HSE
                 | (PLL_Q << RCC_PLLCFGR_PLLQ_Pos);
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }

    // Five wait states for 150 to 168 MHz at 2.7 to 3.6 V, with the prefetch
    // and the ART accelerator's two caches -- set and read back before the
    // clock goes up, never after. Without the caches five wait states are
    // most of the clock thrown away.
    FLASH->ACR = FLASH_ACR_LATENCY_5WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_5WS) { }

    // AHB at the full clock, APB2 at half and APB1 at a quarter: their limits
    // are 84 and 42 MHz.
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2))
              | RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2;

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }

    stm32_sysclk_hz = UBIQOS_STM32_SYSCLK_HZ;
    stm32_pclk1_hz  = UBIQOS_STM32_SYSCLK_HZ / 4u;
    stm32_clock_from_hse = true;
}
