// See ../README.md: only what the kernel uses, over this port's timer.
#pragma once
#include <stdint.h>

// Microseconds since reset, from TIM2 and a count of its overflows -- 64 bits,
// so it does not wrap in any life this board will have. See ../../timer.c.
uint64_t time_us_64(void);
void busy_wait_us(uint64_t us);
static inline void busy_wait_ms(uint32_t ms) { busy_wait_us((uint64_t)ms * 1000u); }
static inline void sleep_ms(uint32_t ms) { busy_wait_ms(ms); }
static inline void sleep_us(uint64_t us) { busy_wait_us(us); }
