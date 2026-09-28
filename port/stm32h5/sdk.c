// What the kernel asks of the chip beyond time and the console: why it reset,
// who it is, random bytes, and how to go back to the bootloader.

#include "stm32h5xx.h"
#include "port.h"

// RCC_RSR keeps every reason since it was last cleared, so it is read once,
// remembered, and cleared -- the next boot's answer is then about that boot.
const char *h5_reset_reason(void)
{
    static const char *why;
    if (why) return why;
    const uint32_t rsr = RCC->RSR;
    why = (rsr & RCC_RSR_BORRSTF)  ? "power-on or BROWN-OUT"
        : (rsr & RCC_RSR_IWDGRSTF) ? "independent watchdog"
        : (rsr & RCC_RSR_WWDGRSTF) ? "window watchdog"
        : (rsr & RCC_RSR_LPWRRSTF) ? "low-power reset"
        : (rsr & RCC_RSR_SFTRSTF)  ? "software (a reboot, or the debugger)"
        : (rsr & RCC_RSR_PINRSTF)  ? "the reset pin (the button, or the ST-LINK)"
        :                            "none of the recorded causes";
    RCC->RSR = RCC_RSR_RMVF;
    return why;
}

// Copied by h5_clock_init before the instruction cache goes on -- see there.
uint32_t h5_uid_words[3];

void h5_unique_id(uint8_t out[12])
{
    for (int w = 0; w < 3; w++)
        for (int b = 0; b < 4; b++) out[w * 4 + b] = (uint8_t)(h5_uid_words[w] >> (8 * b));
}

// The RNG runs from HSI48, which nothing else has started yet. Its words have
// been through the chip's own conditioning and health tests; a seed or clock
// error clears itself with a conditioning reset, and a word that never comes
// ends the read short rather than holding a trap for ever.
static bool rng_up;

static void rng_start(void)
{
    RCC->CR |= RCC_CR_HSI48ON;
    while (!(RCC->CR & RCC_CR_HSI48RDY)) { }
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
    (void)RCC->AHB2ENR;
    RNG->CR = RNG_CR_RNGEN;
    rng_up = true;
}

int32_t h5_rng_read(uint8_t *out, uint32_t len)
{
    if (!rng_up) rng_start();
    uint32_t n = 0;
    while (n < len) {
        if (RNG->SR & (RNG_SR_SECS | RNG_SR_CECS)) {
            RNG->CR |= RNG_CR_CONDRST;
            RNG->CR &= ~RNG_CR_CONDRST;
        }
        uint32_t spins = 0;
        while (!(RNG->SR & RNG_SR_DRDY) && ++spins < 100000u) { }
        if (!(RNG->SR & RNG_SR_DRDY)) break;
        const uint32_t w = RNG->DR;
        for (int b = 0; b < 4 && n < len; b++) out[n++] = (uint8_t)(w >> (8 * b));
    }
    return (int32_t)n;
}

// ST's system bootloader is reached through BOOT0 or the option bytes, and a
// jump into it from a running system needs the clocks and interrupts put back
// the way the ROM expects. Not done yet: the ST-LINK flashes this board, so a
// reboot is what bootsel does here for now.
void h5_reboot_bootloader(void)
{
    NVIC_SystemReset();
}
