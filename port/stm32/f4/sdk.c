// What the kernel asks of the chip beyond time and the console: why it reset,
// who it is, random bytes, and how to go back to the bootloader.

#include "stm32f4xx.h"
#include "port.h"

// RCC_CSR keeps every reason since it was last cleared, so it is read once,
// remembered, and cleared -- the next boot's answer is then about that boot.
// A power-on sets the brown-out and power-on flags as well as the pin's,
// which is why those are asked first.
const char *stm32_reset_reason(void)
{
    static const char *why;
    if (why) return why;
    const uint32_t csr = RCC->CSR;
    why = (csr & RCC_CSR_PORRSTF)  ? "power-on"
        : (csr & RCC_CSR_BORRSTF)  ? "BROWN-OUT"
        : (csr & RCC_CSR_IWDGRSTF) ? "independent watchdog"
        : (csr & RCC_CSR_WWDGRSTF) ? "window watchdog"
        : (csr & RCC_CSR_LPWRRSTF) ? "low-power reset"
        : (csr & RCC_CSR_SFTRSTF)  ? "software (a reboot, or the debugger)"
        : (csr & RCC_CSR_PINRSTF)  ? "the reset pin (the button, or the debugger)"
        :                            "none of the recorded causes";
    RCC->CSR |= RCC_CSR_RMVF;
    return why;
}

// 96 bits in the system memory, readable at any time on this family.
void stm32_unique_id(uint8_t out[12])
{
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
    for (int w = 0; w < 3; w++) {
        const uint32_t v = uid[w];
        for (int b = 0; b < 4; b++) out[w * 4 + b] = (uint8_t)(v >> (8 * b));
    }
}

// The RNG runs from the main PLL's 48 MHz output, which clock.c sets up. The
// F4's RNG is the simpler, older one: no conditioning configuration and no
// health test threshold to set, a word every forty RNG clocks. A seed error is
// recovered as the reference manual says, by clearing the flag and switching
// the RNG off and on; a clock error means the 48 MHz is missing, and is not
// this driver's to fix. Without HSE there is no PLL and no RNG, and it says so
// by returning fewer bytes.
#define RNG_WORD_SPINS 200000u      // far more than forty RNG clocks at 168 MHz

static bool rng_up;
uint32_t stm32_rng_seed_errors, stm32_rng_timeouts;

static void rng_start(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
    (void)RCC->AHB2ENR;
    RNG->CR |= RNG_CR_RNGEN;
    rng_up = true;
}

static bool rng_healthy(void)
{
    const uint32_t sr = RNG->SR;
    if (!(sr & (RNG_SR_SECS | RNG_SR_SEIS))) return !(sr & RNG_SR_CECS);
    stm32_rng_seed_errors++;
    RNG->SR = ~RNG_SR_SEIS;                   // write-zero-to-clear
    RNG->CR &= ~RNG_CR_RNGEN;
    RNG->CR |= RNG_CR_RNGEN;
    return !(RNG->SR & RNG_SR_CECS);
}

int32_t stm32_rng_read(uint8_t *out, uint32_t len)
{
    if (!stm32_clock_from_hse) return 0;
    if (!rng_up) rng_start();
    uint32_t n = 0;
    while (n < len) {
        uint32_t spins = 0;
        while (!(RNG->SR & RNG_SR_DRDY) && ++spins < RNG_WORD_SPINS) {
            if ((spins & 1023u) == 0 && !rng_healthy()) break;
        }
        if (!(RNG->SR & RNG_SR_DRDY)) { stm32_rng_timeouts++; break; }
        const uint32_t w = RNG->DR;
        // A word read while a seed error was being raised is not to be used.
        if (RNG->SR & RNG_SR_SEIS) { rng_healthy(); continue; }
        for (int b = 0; b < 4 && n < len; b++) out[n++] = (uint8_t)(w >> (8 * b));
    }
    return (int32_t)n;
}

// ST's system bootloader is reached through BOOT0 -- the Feather's B0 pin held
// high across a reset. A jump into it from a running system is not done yet:
// the J-Link flashes this board, so a reboot is what bootsel does here for now.
void stm32_reboot_bootloader(void)
{
    NVIC_SystemReset();
}
