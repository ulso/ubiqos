// What the kernel asks of the chip beyond time and the console: why it reset,
// who it is, random bytes, and how to go back to the bootloader.

#include "stm32h5xx.h"
#include "port.h"

// RCC_RSR keeps every reason since it was last cleared, so it is read once,
// remembered, and cleared -- the next boot's answer is then about that boot.
const char *stm32_reset_reason(void)
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

// Copied by stm32_clock_init before the instruction cache goes on -- see there.
extern uint32_t stm32_uid_words[3];

void stm32_unique_id(uint8_t out[12])
{
    for (int w = 0; w < 3; w++)
        for (int b = 0; b < 4; b++) out[w * 4 + b] = (uint8_t)(stm32_uid_words[w] >> (8 * b));
}

// The RNG runs from HSI48, which nothing else has started yet. Its words have
// been through the chip's own conditioning and health tests.
//
// It is set up the way ST gives for this family -- the NIST configuration
// 0x00F00D00 written together with the conditioning reset, and the health test
// threshold 0xAAC7 -- as Zephyr's driver does it. Left at its reset values the
// RNG ran, but ran out: sshd seeds its generator with more bytes than a TLS
// connection does, met a word that never came, and refused to start with "no
// entropy", every time. A seed error is recovered the way the reference
// manual says, by pulsing the conditioning reset until it and the error flags
// are clear, and a word is waited for long enough to arrive: in the NIST
// configuration the RNG delivers sixteen bytes every 343 microseconds.
#define RNG_NIST_CONFIG  0x00F00D00u
#define RNG_NIST_HTCR    0x0000AAC7u
#define RNG_CONFIG_BITS  (RNG_CR_NISTC | RNG_CR_CLKDIV | RNG_CR_RNG_CONFIG1 \
                        | RNG_CR_RNG_CONFIG2 | RNG_CR_RNG_CONFIG3)
#define RNG_WORD_SPINS   2000000u   // a few milliseconds at 240 MHz, several words' worth

static bool rng_up;
uint32_t stm32_rng_seed_errors, stm32_rng_timeouts;

static bool rng_condition_reset(void)
{
    RNG->CR |= RNG_CR_CONDRST;
    RNG->CR &= ~RNG_CR_CONDRST;
    for (uint32_t n = 0; n < 100000u; n++)
        if (!(RNG->CR & RNG_CR_CONDRST)) return true;
    return false;
}

static void rng_start(void)
{
    RCC->CR |= RCC_CR_HSI48ON;
    while (!(RCC->CR & RCC_CR_HSI48RDY)) { }
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
    (void)RCC->AHB2ENR;

    RNG->CR = (RNG->CR & ~RNG_CONFIG_BITS) | RNG_NIST_CONFIG | RNG_CR_CONDRST;
    RNG->HTCR = RNG_NIST_HTCR;
    RNG->CR &= ~RNG_CR_CONDRST;
    while (RNG->CR & RNG_CR_CONDRST) { }
    RNG->CR |= RNG_CR_RNGEN;
    rng_up = true;
}

// Whether the RNG is in a state to deliver: a seed error is cleared and the
// conditioning started again; a clock error is not this driver's to fix.
static bool rng_healthy(void)
{
    const uint32_t sr = RNG->SR;
    if (!(sr & (RNG_SR_SECS | RNG_SR_SEIS))) return !(sr & RNG_SR_CECS);
    stm32_rng_seed_errors++;
    RNG->SR = ~RNG_SR_SEIS;                   // write-zero-to-clear
    if (!rng_condition_reset()) return false;
    return !(RNG->SR & (RNG_SR_SECS | RNG_SR_CECS));
}

int32_t stm32_rng_read(uint8_t *out, uint32_t len)
{
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

// ST's system bootloader is reached through BOOT0 or the option bytes, and a
// jump into it from a running system needs the clocks and interrupts put back
// the way the ROM expects. Not done yet: the ST-LINK flashes this board, so a
// reboot is what bootsel does here for now.
void stm32_reboot_bootloader(void)
{
    NVIC_SystemReset();
}
