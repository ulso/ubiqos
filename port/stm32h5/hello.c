// The first program on the NUCLEO-H563ZI: the clock, the console and a LED,
// before any of the kernel. What it proves is that the board, the toolchain,
// the linker script and the flashing all agree, so that when the kernel does
// not start, none of those is the reason.

#include "stm32h5xx.h"
#include "port.h"

static void put_u32(uint32_t v)
{
    char b[11];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) h5_console_putc(b[--n]);
}

int main(void)
{
    h5_console_init(115200);
    h5_console_puts("\nUbiqOS port test, STM32H563ZI\n");
    h5_console_puts("clock ");
    put_u32(h5_sysclk_hz / 1000000u);
    h5_console_puts(h5_clock_from_hse ? " MHz from the ST-LINK's 8 MHz\n"
                                      : " MHz, HSE did not start: on the reset clock\n");
    h5_console_puts("type, and it comes back\n");

    // LD1, the green one, on PB0.
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    (void)RCC->AHB2ENR;
    GPIOB->MODER = (GPIOB->MODER & ~(3u << 0)) | (1u << 0);

    // SysTick as a plain counter: half a second a toggle, whatever the clock.
    SysTick->LOAD = h5_sysclk_hz / 8u / 2u - 1u;     // the /8 reference clock
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_ENABLE_Msk;         // CLKSOURCE 0: the core clock / 8

    for (;;) {
        if (SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) GPIOB->ODR ^= 1u;
        const int c = h5_console_getc();
        if (c >= 0) h5_console_putc(c == '\r' ? '\n' : (char)c);
    }
}
