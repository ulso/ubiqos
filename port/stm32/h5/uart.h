// The console USART's registers on the STM32H5, for common/console.c: USART3,
// on the pins the board header names. Everything above the registers -- the
// rings, /dev/term, Ctrl-C -- is the same on every family.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "stm32h5xx.h"
#include "board.h"

#define CONSOLE_GPIO   ((GPIO_TypeDef *)(GPIOA_BASE + (UBIQOS_STM32_CONSOLE_PORT - 'A') * 0x400u))
#define CONSOLE_GPIOEN (1u << (UBIQOS_STM32_CONSOLE_PORT - 'A'))   // RCC_AHB2ENR, A=0 to I=8

static inline void uart_clocks_on(void)
{
    RCC->AHB2ENR  |= CONSOLE_GPIOEN;
    RCC->APB1LENR |= RCC_APB1LENR_USART3EN;
    (void)RCC->APB1LENR;              // the enable takes effect before the first access
}

// USART3's kernel clock is PCLK1 out of reset (CCIPR1.USART3SEL = 0).
static inline void uart_start(uint32_t brr)
{
    USART3->CR1 = 0;
    USART3->BRR = brr;
    USART3->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE;
}

// The byte that arrived, or -1; an overrun is cleared on the way.
static inline int uart_receive(void)
{
    const uint32_t isr = USART3->ISR;
    if (isr & USART_ISR_ORE) USART3->ICR = USART_ICR_ORECF;
    return (isr & USART_ISR_RXNE_RXFNE) ? (int)(uint8_t)USART3->RDR : -1;
}

static inline bool uart_can_send(void)      { return (USART3->ISR & USART_ISR_TXE_TXFNF) != 0; }
static inline void uart_send(uint8_t b)     { USART3->TDR = b; }
static inline bool uart_send_irq_on(void)   { return (USART3->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0; }
static inline void uart_send_irq(bool on)
{
    if (on) USART3->CR1 |= USART_CR1_TXEIE_TXFNFIE;
    else    USART3->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
}
