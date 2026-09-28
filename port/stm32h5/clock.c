// The STM32H5's clocks: 240 MHz from PLL1, fed by the ST-LINK's 8 MHz.
//
// The NUCLEO-H563ZI has no crystal fitted for HSE by default. The ST-LINK
// drives its MCO output into the chip's OSC_IN instead, which makes HSE an
// external clock in bypass mode rather than an oscillator. 8 MHz / 2 * 120 / 2
// is 240 MHz, the same PLL setting Zephyr uses on this board, below the chip's
// 250 MHz limit.
//
// If HSE never comes up -- an ST-LINK that is not driving MCO, a board with the
// solder bridges moved to a crystal -- the chip stays on its reset clock, HSI
// divided by two, 32 MHz. Slower, but a console that works at the wrong speed
// says why; one that never starts says nothing.

#include "stm32h5xx.h"
#include "port.h"

uint32_t h5_uid_words[3];       // the device id; see the note before ICACHE below
uint32_t h5_sysclk_hz = 32000000u;
uint32_t h5_pclk1_hz  = 32000000u;
bool     h5_clock_from_hse;

#define HSE_TIMEOUT 2000000u    // loop turns, some tens of milliseconds at 32 MHz

void h5_clock_init(void)
{
    // The core voltage first, VOS0, the highest: every scale below it caps the
    // clock, and the flash's wait states are counted per scale.
    PWR->VOSCR = (PWR->VOSCR & ~PWR_VOSCR_VOS) | (3u << PWR_VOSCR_VOS_Pos);
    while (!(PWR->VOSSR & PWR_VOSSR_VOSRDY)) { }

    // HSE as a clock input, analog bypass: what the ST-LINK's MCO is.
    RCC->CR = (RCC->CR & ~RCC_CR_HSEEXT) | RCC_CR_HSEBYP;
    RCC->CR |= RCC_CR_HSEON;
    uint32_t n = 0;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
        if (++n == HSE_TIMEOUT) {
            RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_HSEBYP);
            return;
        }
    }

    // PLL1: HSE / M=2 is 4 MHz, input range 4-8 MHz (RGE 2), times N=120 is a
    // 480 MHz VCO in the wide range, and / P=2 is 240 MHz. Q and R are set to
    // what Zephyr sets and not enabled: nothing takes them yet.
    RCC->CR &= ~RCC_CR_PLL1ON;
    while (RCC->CR & RCC_CR_PLL1RDY) { }
    RCC->PLL1CFGR = (3u << RCC_PLL1CFGR_PLL1SRC_Pos)
                  | (2u << RCC_PLL1CFGR_PLL1RGE_Pos)
                  | (2u << RCC_PLL1CFGR_PLL1M_Pos)
                  | RCC_PLL1CFGR_PLL1PEN;
    RCC->PLL1DIVR = ((120u - 1u) << RCC_PLL1DIVR_PLL1N_Pos)
                  | ((2u - 1u)   << RCC_PLL1DIVR_PLL1P_Pos)
                  | ((4u - 1u)   << RCC_PLL1DIVR_PLL1Q_Pos)
                  | ((2u - 1u)   << RCC_PLL1DIVR_PLL1R_Pos);
    RCC->CR |= RCC_CR_PLL1ON;
    while (!(RCC->CR & RCC_CR_PLL1RDY)) { }

    // Five wait states and the high-frequency write delay for 240 MHz at VOS0,
    // set and read back before the clock goes up, never after.
    const uint32_t acr = (FLASH->ACR & ~(FLASH_ACR_LATENCY | FLASH_ACR_WRHIGHFREQ))
                       | (5u << FLASH_ACR_LATENCY_Pos)
                       | (2u << FLASH_ACR_WRHIGHFREQ_Pos)
                       | FLASH_ACR_PRFTEN;
    FLASH->ACR = acr;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != (5u << FLASH_ACR_LATENCY_Pos)) { }

    // AHB and APB2 at the full 240, APB1 and APB3 at 120 -- the ceiling for
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
    // by the kernel faulting in h5_unique_id with BFARVALID set.
    for (int w = 0; w < 3; w++) h5_uid_words[w] = ((const volatile uint32_t *)UID_BASE)[w];

    // The instruction cache in front of the flash: with five wait states,
    // running without it is running at a fraction of the clock.
    ICACHE->CR |= ICACHE_CR_EN;

    h5_sysclk_hz = 240000000u;
    h5_pclk1_hz  = 120000000u;
    h5_clock_from_hse = true;
}
