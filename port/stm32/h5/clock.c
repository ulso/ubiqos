// The STM32H5's clocks: PLL1 from HSE, at whatever the board header asks for.
//
// The board says three things -- UBIQOS_STM32_HSE_HZ, UBIQOS_STM32_HSE_BYPASS and
// UBIQOS_STM32_SYSCLK_HZ -- and the PLL is worked out from them: HSE divided down
// to a 4 MHz reference, multiplied to twice the system clock, divided by two.
// On the NUCLEO-H563ZI that is the ST-LINK's 8 MHz, in bypass, to 240 MHz; on
// the NUCLEO-H503RB a 24 MHz crystal to 250.
//
// If HSE never comes up -- an ST-LINK that is not driving MCO, a crystal that
// does not start -- the chip stays on its reset clock, HSI divided by two, 32
// MHz. Slower, but a console that works at the wrong speed says why; one that
// never starts says nothing.

#include "stm32h5xx.h"
#include "port.h"
#include "board.h"

#if !defined(UBIQOS_STM32_HSE_HZ) || !defined(UBIQOS_STM32_HSE_BYPASS) || !defined(UBIQOS_STM32_SYSCLK_HZ)
#error "the board header must say UBIQOS_STM32_HSE_HZ, UBIQOS_STM32_HSE_BYPASS and UBIQOS_STM32_SYSCLK_HZ"
#endif
#define PLL_REF_HZ 4000000u         // input range 4-8 MHz, PLL1RGE 2
#define PLL_M (UBIQOS_STM32_HSE_HZ / PLL_REF_HZ)
#define PLL_N (UBIQOS_STM32_SYSCLK_HZ * 2u / PLL_REF_HZ)
_Static_assert(UBIQOS_STM32_HSE_HZ % PLL_REF_HZ == 0, "HSE must be a multiple of 4 MHz");
_Static_assert(UBIQOS_STM32_SYSCLK_HZ * 2u % PLL_REF_HZ == 0, "the system clock must be a multiple of 2 MHz");
_Static_assert(PLL_M >= 1 && PLL_M <= 63, "PLL1M is six bits");
_Static_assert(PLL_N >= 4 && PLL_N <= 512, "PLL1N is 4 to 512");
_Static_assert(UBIQOS_STM32_SYSCLK_HZ <= 250000000u, "250 MHz is the STM32H5's limit");

uint32_t stm32_uid_words[3];       // the device id; see the note before ICACHE below
uint32_t stm32_sysclk_hz = 32000000u;
uint32_t stm32_pclk1_hz  = 32000000u;
bool     stm32_clock_from_hse;

#define HSE_TIMEOUT 2000000u    // loop turns, some tens of milliseconds at 32 MHz

void stm32_clock_init(void)
{
    // The core voltage first, VOS0, the highest: every scale below it caps the
    // clock, and the flash's wait states are counted per scale.
    PWR->VOSCR = (PWR->VOSCR & ~PWR_VOSCR_VOS) | (3u << PWR_VOSCR_VOS_Pos);
    while (!(PWR->VOSSR & PWR_VOSSR_VOSRDY)) { }

    // HSE: a clock input in analog bypass when something drives OSC_IN, as the
    // ST-LINK's MCO does; the oscillator when a crystal sits across the pins.
    RCC->CR = (RCC->CR & ~(RCC_CR_HSEEXT | RCC_CR_HSEBYP))
            | (UBIQOS_STM32_HSE_BYPASS ? RCC_CR_HSEBYP : 0u);
    RCC->CR |= RCC_CR_HSEON;
    uint32_t n = 0;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
        if (++n == HSE_TIMEOUT) {
            RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_HSEBYP);
            return;
        }
    }

    // PLL1: HSE / M is 4 MHz, input range 4-8 MHz (RGE 2), times N is a VCO at
    // twice the system clock in the wide range, and / P=2 is the system clock.
    // Q and R are set to what Zephyr sets and not enabled: nothing takes them.
    RCC->CR &= ~RCC_CR_PLL1ON;
    while (RCC->CR & RCC_CR_PLL1RDY) { }
    RCC->PLL1CFGR = (3u << RCC_PLL1CFGR_PLL1SRC_Pos)
                  | (2u << RCC_PLL1CFGR_PLL1RGE_Pos)
                  | (PLL_M << RCC_PLL1CFGR_PLL1M_Pos)
                  | RCC_PLL1CFGR_PLL1PEN;
    RCC->PLL1DIVR = ((PLL_N - 1u) << RCC_PLL1DIVR_PLL1N_Pos)
                  | ((2u - 1u)   << RCC_PLL1DIVR_PLL1P_Pos)
                  | ((4u - 1u)   << RCC_PLL1DIVR_PLL1Q_Pos)
                  | ((2u - 1u)   << RCC_PLL1DIVR_PLL1R_Pos);
    RCC->CR |= RCC_CR_PLL1ON;
    while (!(RCC->CR & RCC_CR_PLL1RDY)) { }

    // Five wait states and the high-frequency write delay, for 210 to 250 MHz at VOS0,
    // set and read back before the clock goes up, never after.
    const uint32_t acr = (FLASH->ACR & ~(FLASH_ACR_LATENCY | FLASH_ACR_WRHIGHFREQ))
                       | (5u << FLASH_ACR_LATENCY_Pos)
                       | (2u << FLASH_ACR_WRHIGHFREQ_Pos)
                       | FLASH_ACR_PRFTEN;
    FLASH->ACR = acr;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != (5u << FLASH_ACR_LATENCY_Pos)) { }

    // AHB and APB2 at the full clock, APB1 and APB3 at half -- the ceiling for
    // the buses is the system clock, but the half is what ST's own boards use
    // and what the USART3 divider below is worked out from.
    RCC->CFGR2 = (RCC->CFGR2 & ~(RCC_CFGR2_HPRE | RCC_CFGR2_PPRE1 | RCC_CFGR2_PPRE2 | RCC_CFGR2_PPRE3))
               | (4u << RCC_CFGR2_PPRE1_Pos)
               | (4u << RCC_CFGR2_PPRE3_Pos);

    RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW) | (3u << RCC_CFGR1_SW_Pos);
    while (((RCC->CFGR1 & RCC_CFGR1_SWS) >> RCC_CFGR1_SWS_Pos) != 3u) { }

    // The device id, while it can still be read. It sits in the flash's
    // read-only area at 0x08FFF800, and with the instruction cache on a read
    // there is a bus fault: ST's answer is an MPU region marking the area
    // uncacheable, and this one is to read the twelve bytes once, first. Found
    // by the kernel faulting in stm32_unique_id with BFARVALID set.
    for (int w = 0; w < 3; w++) stm32_uid_words[w] = ((const volatile uint32_t *)UID_BASE)[w];

    // The instruction cache in front of the flash: with five wait states,
    // running without it is running at a fraction of the clock.
    ICACHE->CR |= ICACHE_CR_EN;

    stm32_sysclk_hz = UBIQOS_STM32_SYSCLK_HZ;
    stm32_pclk1_hz  = UBIQOS_STM32_SYSCLK_HZ / 2u;
    stm32_clock_from_hse = true;
}
