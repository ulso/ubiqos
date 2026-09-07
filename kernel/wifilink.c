// The kernel's side of the wifi library.
//
// wifi used to be kernel/wifi.c: 6.9 kB of SRAM that mattered only to somebody
// who typed "wifi". It is a LIBRARY module now, loaded where modules are loaded
// -- PSRAM -- and this is what links it and hands it the addresses it needs.
//
// The three functions at the bottom keep the names the rest of the kernel calls,
// so main.c and syscalls.c do not know the difference. That is the point: what
// changed is where the code lives, not what it is.
#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "../common/myrtos_abi.h"
#include "moddir.h"
#include "sdcard.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes, uint32_t priority);
void *myrtos_tlsf_malloc(void *pool, uint32_t size);
extern void *myrtos_bulk_pool;

// Wrappers rather than the functions themselves, and for two different reasons.
// gpio_put, gpio_get and gpio_set_dir are inline in the SDK's headers, so there
// is no address to take -- these give them one, which is exactly what lets the
// library include no SDK header at all. spi_init returns the baud rate it
// achieved and the table says void; a wrapper is cheaper than an interface that
// carries a value nobody reads.
static void   k_spi_init(void *spi, uint32_t baud) { spi_init((spi_inst_t*)spi, baud); }
static void   k_spi_write_read(void *spi, const uint8_t *out, uint8_t *in, uint32_t len)
{ spi_write_read_blocking((spi_inst_t*)spi, out, in, (size_t)len); }
static void   k_gpio_init(uint32_t pin)                    { gpio_init(pin); }
static void   k_gpio_set_function(uint32_t pin, uint32_t f){ gpio_set_function(pin, (gpio_function_t)f); }
static void   k_gpio_set_dir(uint32_t pin, bool out)       { gpio_set_dir(pin, out); }
static void   k_gpio_put(uint32_t pin, bool v)             { gpio_put(pin, v); }
static bool   k_gpio_get(uint32_t pin)                     { return gpio_get(pin); }
static void   k_gpio_set_pulls(uint32_t p, bool u, bool d) { gpio_set_pulls(p, u, d); }
static void   k_busy_wait(uint64_t us)                     { busy_wait_us(us); }
static uint64_t k_time_us(void)                            { return time_us_64(); }
static void  *k_bulk_alloc(uint32_t n)
{ return myrtos_bulk_pool ? myrtos_tlsf_malloc(myrtos_bulk_pool, n) : 0; }

// Version 3. SRAM rather than the bulk pool, because this is where a library
// puts anything DMA will touch -- see the note on mem_alloc in the ABI.
void *myrtos_mem_alloc(uint32_t size);
void myrtos_putc(char c);
uint32_t myrtos_psram_bytes(void);
static void  *k_mem_alloc(uint32_t n)                      { return myrtos_mem_alloc(n); }
static void   k_putc(char c)                               { myrtos_putc(c); }

// The PSRAM window, bounded at BOTH ends. SRAM is at 0x20000000 and PSRAM at
// 0x11000000, so "above the PSRAM base" is true of SRAM too -- a mistake this
// repository has made before and paid for twice.
static bool   k_dma_safe(const void *p)
{
    uintptr_t a = (uintptr_t)p;
    uint32_t n = myrtos_psram_bytes();
    return !(n && a >= MYRTOS_PSRAM_BASE && a < (uintptr_t)MYRTOS_PSRAM_BASE + n);
}
static void   k_spi_set_baudrate(void *spi, uint32_t baud)
{ spi_set_baudrate((spi_inst_t*)spi, baud); }
static void   k_sleep_ms(uint32_t ms)                      { sleep_ms(ms); }
static int32_t k_pio_add_program(void *pio, const void *p)
{ return pio_add_program((PIO)pio, (const pio_program_t*)p); }
static void   k_pio_sm_init(void *pio, uint32_t sm, uint32_t pc, const void *c)
{ pio_sm_init((PIO)pio, sm, pc, (const pio_sm_config*)c); }
static void   k_pio_sm_set_pindirs_with_mask64(void *pio, uint32_t sm,
                                               uint64_t v, uint64_t m)
{ pio_sm_set_pindirs_with_mask64((PIO)pio, sm, v, m); }
static void   k_pio_set_gpio_base(void *pio, uint32_t base)
{ pio_set_gpio_base((PIO)pio, base); }
static void   k_uart_init(void *uart, uint32_t baud)
{ uart_init((uart_inst_t*)uart, baud); }
static void   k_spi_write(void *spi, const uint8_t *src, uint32_t len)
{ spi_write_blocking((spi_inst_t*)spi, src, (size_t)len); }
static void   k_spi_read(void *spi, uint8_t repeated, uint8_t *dst, uint32_t len)
{ spi_read_blocking((spi_inst_t*)spi, repeated, dst, (size_t)len); }

// Not static any more: fat32link.c wants the same table, and there is only
// one kernel to describe.
const myrtos_kernel_api_t myrtos_kernel_api = {
    .abi               = MYRTOS_KERNEL_API_ABI,
    .print             = myrtos_print,
    .print_u32         = myrtos_print_u32,
    .kernel_thread     = myrtos_kernel_thread,
    .bulk_alloc        = k_bulk_alloc,
    .spi               = spi1,
    .spi_init          = k_spi_init,
    .spi_write_read    = k_spi_write_read,
    .gpio_init         = k_gpio_init,
    .gpio_set_function = k_gpio_set_function,
    .gpio_set_dir      = k_gpio_set_dir,
    .gpio_put          = k_gpio_put,
    .gpio_get          = k_gpio_get,
    .gpio_set_pulls    = k_gpio_set_pulls,
    .busy_wait_us      = k_busy_wait,
    .time_us           = k_time_us,
    .sd_init           = myrtos_sd_init,
    .sd_try_sdio       = myrtos_sd_try_sdio,
    .sd_read_block     = myrtos_sd_read_block,
    .sd_write_block    = myrtos_sd_write_block,

    .mem_alloc         = k_mem_alloc,
    .putc              = k_putc,
    .dma_safe          = k_dma_safe,
    .spi_set_baudrate  = k_spi_set_baudrate,
    .sleep_ms          = k_sleep_ms,
    .pio_add_program   = k_pio_add_program,
    .pio_sm_init       = k_pio_sm_init,
    .pio_sm_set_pindirs_with_mask64 = k_pio_sm_set_pindirs_with_mask64,
    .pio_set_gpio_base = k_pio_set_gpio_base,
    .uart_init         = k_uart_init,
    .spi_write         = k_spi_write,
    .spi_read          = k_spi_read,
};

// The entries wifilib publishes, in the order its table documents them.
enum { WIFI_INIT = 0, WIFI_PROBE = 1, WIFI_START = 2, WIFI_PID = 3,
       WIFI_FORGET = 4, WIFI_RESET = 5 };

static const myrtos_lib_table_t *lib;

// Linked on the first call and not at boot, so that a machine with no wifi
// module in flash pays nothing and says nothing. Failing is not an error worth
// a message here: myrtos_lib_link has already said what went wrong if anything
// did, and a board without the module simply has no wifi.
static bool ensure_linked(void)
{
    if (lib) return true;
    lib = myrtos_lib_link("wifilib", 0);
    if (!lib || lib->count <= WIFI_RESET) { lib = 0; return false; }

    bool (*init)(const myrtos_kernel_api_t *) = (bool (*)(const myrtos_kernel_api_t *))lib->fn[WIFI_INIT];
    if (!init(&myrtos_kernel_api)) { lib = 0; return false; }

    // Said out loud, because a library that does not link fails silently by
    // design -- a board without the module simply has no wifi -- and silence
    // is indistinguishable from success when you are looking for either.
    myrtos_print("wifi: library linked, running from the module pool\n");
    return true;
}

void myrtos_wifi_probe(void)
{
    if (!ensure_linked()) return;
    ((void (*)(void))lib->fn[WIFI_PROBE])();
}

void myrtos_wifi_start_server(void)
{
    if (!ensure_linked()) return;
    ((void (*)(void))lib->fn[WIFI_START])();
}

int32_t myrtos_wifi_server_pid(void)
{
    if (!lib) return -1;                 // not linked means no server to ask
    return ((int32_t (*)(void))lib->fn[WIFI_PID])();
}

// A process has been reaped, so anything it held on the coprocessor is nobody's
// -- which for a listening socket means a port that would otherwise stay taken
// until the board was restarted. Killing httpd and starting it again is exactly
// how that was found.
//
// This only MARKS. Releasing a socket means SPI transactions with handshakes
// and waits, and reap() runs in kernel context where waiting stops the machine;
// the wifi thread does the closing at the top of the next request it handles.
// Called on every process teardown, so it must also be free when there is no
// wifi library at all.
void myrtos_wifi_forget_pid(int32_t pid)
{
    if (!lib) return;                    // never linked: no sockets to lose
    ((void (*)(int32_t))lib->fn[WIFI_FORGET])(pid);
}

// The only recovery that does not need the protocol to be in a fit state to be
// asked anything, which is exactly when it is wanted. It disconnects the
// machine -- nina-fw keeps no credentials across a reset -- so it is asked for
// and never done quietly.
int32_t myrtos_wifi_hard_reset(void)
{
    if (!ensure_linked()) return -1;
    ((void (*)(void))lib->fn[WIFI_RESET])();
    return 0;
}
