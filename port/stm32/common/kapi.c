// The table a library or driver module is handed at init: the kernel's own
// services, and on the RP2350 the SDK's hardware calls as well. Those have no
// meaning on this chip -- a module that drives an RP2350 SPI block has nothing
// to drive here -- so they are left empty, and a module that reaches for one
// is a module built for the other machine.

#include <stdint.h>
#include <stdbool.h>
#include "../../../common/ubiqos_abi.h"
#include "io.h"
#include "pico/time.h"

void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
void ubiqos_putc(char c);
int32_t ubiqos_kernel_thread(void (*entry)(void), uint32_t stack_bytes, uint32_t priority);
void *ubiqos_mem_alloc(uint32_t size);
void *ubiqos_tlsf_malloc(void *pool, uint32_t size);
extern void *ubiqos_mem_pool;
extern uint32_t stm32_sysclk_hz;

static void     k_busy_wait(uint64_t us)   { busy_wait_us(us); }
static uint64_t k_time_us(void)            { return time_us_64(); }
static void    *k_mem_alloc(uint32_t n)    { return ubiqos_mem_alloc(n); }
static void    *k_bulk_alloc(uint32_t n)   { return ubiqos_mem_alloc(n); }   // no PSRAM: one pool
static void     k_putc(char c)             { ubiqos_putc(c); }
#if UBIQOS_STM32_F4
// The F4's core-coupled memory, where the kernel keeps its own data, is on the
// CPU's data bus only: no DMA reaches it. Everything else is SRAM.
static bool     k_dma_safe(const void *p)  { return (uintptr_t)p >= 0x20000000u; }
#else
static bool     k_dma_safe(const void *p)  { (void)p; return true; }         // all of it is SRAM
#endif
static void     k_sleep_ms(uint32_t ms)    { busy_wait_ms(ms); }
static uint32_t k_clock_hz(void)           { return stm32_sysclk_hz; }
static void    *k_driver_alloc(uint32_t n)
{ return ubiqos_mem_pool ? ubiqos_tlsf_malloc(ubiqos_mem_pool, n) : 0; }

const ubiqos_kernel_api_t ubiqos_kernel_api = {
    .abi            = UBIQOS_KERNEL_API_ABI,
    .print          = ubiqos_print,
    .print_u32      = ubiqos_print_u32,
    .kernel_thread  = ubiqos_kernel_thread,
    .bulk_alloc     = k_bulk_alloc,
    .busy_wait_us   = k_busy_wait,
    .time_us        = k_time_us,
    .mem_alloc      = k_mem_alloc,
    .putc           = k_putc,
    .dma_safe       = k_dma_safe,
    .sleep_ms       = k_sleep_ms,
    .clock_hz       = k_clock_hz,
    .driver_alloc   = k_driver_alloc,
    .pin_claim      = ubiqos_pin_claim,
    .pin_release    = ubiqos_pin_release,
    .pin_owner      = ubiqos_pin_owner,
};
