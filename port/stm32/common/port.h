// The STM32H5 port: what the kernel asks of this chip, and nothing else.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "stm32h5xx.h"

// The system clock after h5_clock_init: 240 MHz from the PLL when the ST-LINK's
// 8 MHz reaches HSE, or the 32 MHz reset clock when it does not.
extern uint32_t h5_sysclk_hz;
extern uint32_t h5_pclk1_hz;
extern bool     h5_clock_from_hse;

void h5_clock_init(void);

// The microsecond clock, TIM2 and its overflows: time_us_64 in sdk/pico/time.h.
void h5_time_init(void);

// USART3 on PD8/PD9: the ST-LINK's virtual serial port on a NUCLEO-H563ZI.
void h5_console_init(uint32_t baud);
void h5_console_putc(char c);
void h5_console_puts(const char *s);
int  h5_console_getc(void);          // -1 when nothing has arrived
uint32_t h5_console_write(const uint8_t *buf, uint32_t len);   // what fitted
uint32_t h5_console_room(void);
uint32_t h5_console_read(uint8_t *buf, uint32_t len);
uint32_t h5_console_available(void);
void h5_console_start_thread(void);          // Ctrl-C; see console.c
void h5_net_start(void);                     // Ethernet and lwIP; see ethnet.c

// What the kernel asks of the chip that the pico-sdk answered on the RP2350.
const char *h5_reset_reason(void);           // why this boot happened, in words
void h5_unique_id(uint8_t out[12]);          // the 96-bit device id
int32_t h5_rng_read(uint8_t *out, uint32_t len);   // bytes from the RNG, or fewer
void h5_reboot_bootloader(void);             // ST's system bootloader
