// The STM32H5 port: what the kernel asks of this chip, and nothing else.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// The system clock after h5_clock_init: 240 MHz from the PLL when the ST-LINK's
// 8 MHz reaches HSE, or the 32 MHz reset clock when it does not.
extern uint32_t h5_sysclk_hz;
extern uint32_t h5_pclk1_hz;
extern bool     h5_clock_from_hse;

void h5_clock_init(void);

// USART3 on PD8/PD9: the ST-LINK's virtual serial port on a NUCLEO-H563ZI.
void h5_console_init(uint32_t baud);
void h5_console_putc(char c);
void h5_console_puts(const char *s);
int  h5_console_getc(void);          // -1 when nothing has arrived
