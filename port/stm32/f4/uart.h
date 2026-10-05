// The console USART's registers on the STM32F4, for common/console.c: USART3,
// on the pins the board header names. The older USART: one status register,
// one data register both ways, and an overrun cleared by reading the two.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx.h"
#include "board.h"

#define CONSOLE_GPIO   ((GPIO_TypeDef *)(GPIOA_BASE + (UBIQOS_STM32_CONSOLE_PORT - 'A') * 0x400u))
#define CONSOLE_GPIOEN (1u << (UBIQOS_STM32_CONSOLE_PORT - 'A'))   // RCC_AHB1ENR, A=0 to I=8

static inline void uart_clocks_on(void)
{
    RCC->AHB1ENR |= CONSOLE_GPIOEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    (void)RCC->APB1ENR;               // the enable takes effect before the first access
}

static inline void uart_start(uint32_t brr)
{
    USART3->CR1 = 0;
    USART3->BRR = brr;
    USART3->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
}

// The byte that arrived, or -1. An overrun sets ORE and keeps raising the
// interrupt until SR and then DR are read, which is what this does either way.
static inline int uart_receive(void)
{
    const uint32_t sr = USART3->SR;
    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        const uint8_t c = (uint8_t)USART3->DR;
        return (sr & USART_SR_RXNE) ? (int)c : -1;
    }
    return -1;
}

static inline bool uart_can_send(void)      { return (USART3->SR & USART_SR_TXE) != 0; }
static inline void uart_send(uint8_t b)     { USART3->DR = b; }
static inline bool uart_send_irq_on(void)   { return (USART3->CR1 & USART_CR1_TXEIE) != 0; }
static inline void uart_send_irq(bool on)
{
    if (on) USART3->CR1 |= USART_CR1_TXEIE;
    else    USART3->CR1 &= ~USART_CR1_TXEIE;
}
