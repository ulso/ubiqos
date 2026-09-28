// The serial console: USART3 on PD8 (TX) and PD9 (RX), alternate function 7,
// which the NUCLEO-H563ZI wires to the ST-LINK's virtual serial port.
//
// Polled, for now. The kernel's console ring and an interrupt come when the
// kernel does; this is what proves the clock and the pins first.

#include "stm32h5xx.h"
#include "port.h"

#define CONSOLE_TX_PIN 8u
#define CONSOLE_RX_PIN 9u
#define CONSOLE_AF     7u

static void pin_af(GPIO_TypeDef *g, uint32_t pin, uint32_t af)
{
    g->MODER = (g->MODER & ~(3u << (pin * 2u))) | (2u << (pin * 2u));
    g->OSPEEDR |= (2u << (pin * 2u));
    volatile uint32_t *afr = &g->AFR[pin >> 3];
    const uint32_t shift = (pin & 7u) * 4u;
    *afr = (*afr & ~(0xFu << shift)) | (af << shift);
}

void h5_console_init(uint32_t baud)
{
    RCC->AHB2ENR  |= RCC_AHB2ENR_GPIODEN;
    RCC->APB1LENR |= RCC_APB1LENR_USART3EN;
    (void)RCC->APB1LENR;              // the enable takes effect before the first access

    pin_af(GPIOD, CONSOLE_TX_PIN, CONSOLE_AF);
    pin_af(GPIOD, CONSOLE_RX_PIN, CONSOLE_AF);

    // USART3's kernel clock is PCLK1 out of reset (CCIPR1.USART3SEL = 0).
    USART3->CR1 = 0;
    USART3->BRR = (h5_pclk1_hz + baud / 2u) / baud;
    USART3->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void h5_console_putc(char c)
{
    while (!(USART3->ISR & USART_ISR_TXE_TXFNF)) { }
    USART3->TDR = (uint8_t)c;
}

void h5_console_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') h5_console_putc('\r');
        h5_console_putc(*s++);
    }
}

int h5_console_getc(void)
{
    // An overrun stops the receiver until it is cleared, and a console that
    // stops listening because somebody typed too fast is worse than a lost key.
    if (USART3->ISR & USART_ISR_ORE) USART3->ICR = USART_ICR_ORECF;
    if (!(USART3->ISR & USART_ISR_RXNE_RXFNE)) return -1;
    return (int)(USART3->RDR & 0xFFu);
}
