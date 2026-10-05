// The STM32 port: what the kernel asks of the chip, and nothing else. Each
// family in its own directory answers it -- h5/ for now.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#if UBIQOS_STM32_H5
#include "stm32h5xx.h"
#else
#error "port.h: no STM32 family -- the build defines UBIQOS_STM32_H5"
#endif

// The system clock after stm32_clock_init: what the board header asks for, from
// the PLL when HSE starts, or the reset clock when it does not.
extern uint32_t stm32_sysclk_hz;
extern uint32_t stm32_pclk1_hz;
extern bool     stm32_clock_from_hse;

void stm32_clock_init(void);

// The microsecond clock, a 32-bit timer and its overflows: time_us_64 in
// sdk/pico/time.h.
void stm32_time_init(void);

// The USART the board header names: on a NUCLEO, the ST-LINK's virtual serial
// port.
void stm32_console_init(uint32_t baud);
void stm32_console_putc(char c);
void stm32_console_puts(const char *s);
int  stm32_console_getc(void);          // -1 when nothing has arrived
uint32_t stm32_console_write(const uint8_t *buf, uint32_t len);   // what fitted
uint32_t stm32_console_room(void);
uint32_t stm32_console_read(uint8_t *buf, uint32_t len);
uint32_t stm32_console_available(void);
void stm32_console_start_thread(void);          // Ctrl-C; see console.c
void stm32_net_start(void);                     // Ethernet and lwIP; see ethnet.c

// What the kernel asks of the chip that the pico-sdk answered on the RP2350.
const char *stm32_reset_reason(void);           // why this boot happened, in words
void stm32_unique_id(uint8_t out[12]);          // the 96-bit device id
int32_t stm32_rng_read(uint8_t *out, uint32_t len);   // bytes from the RNG, or fewer
void stm32_reboot_bootloader(void);             // ST's system bootloader
