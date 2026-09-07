// What the SD driver needs from outside itself, and where it comes from now.
//
// The driver used to be kernel/sdcard.c and the vendored SDIO driver under
// third_party. Together they were 11.6 kB of SRAM in a kernel that has to be
// resident from the first instruction, and they mattered only to somebody who
// touched the card. They are a LIBRARY module now, and a library has no SDK: it
// gets addresses from the kernel in a table.
//
// The SDK's own headers are still compiled against, because they are where the
// PIO and DMA register layouts live and those are fixed addresses that need
// nobody's help. What is not fixed is the handful of real functions -- as
// opposed to the many inline ones -- and each of those is given here under the
// name the SDK declares, so that the vendored driver compiles unmodified.
#include <stdint.h>
#include <stdbool.h>
#include "../../common/myrtos_abi.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/pio.h"
#include "pico/time.h"

const myrtos_kernel_api_t *myrtos_sd_k;

void gpio_init(uint pin)                            { myrtos_sd_k->gpio_init(pin); }
void gpio_set_function(uint pin, gpio_function_t f) { myrtos_sd_k->gpio_set_function(pin, (uint32_t)f); }
void gpio_set_pulls(uint pin, bool up, bool down)   { myrtos_sd_k->gpio_set_pulls(pin, up, down); }
void sleep_ms(uint32_t ms)                          { myrtos_sd_k->sleep_ms(ms); }
uint64_t time_us_64(void)                           { return myrtos_sd_k->time_us(); }

// The SDK returns an offset or a negative error from these three; the table
// carries the value through unchanged.
int pio_add_program(PIO pio, const pio_program_t *p)
{ return (int)myrtos_sd_k->pio_add_program(pio, p); }

int pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *c)
{ myrtos_sd_k->pio_sm_init(pio, sm, initial_pc, c); return 0; }

void pio_sm_set_pindirs_with_mask64(PIO pio, uint sm, uint64_t values, uint64_t mask)
{ myrtos_sd_k->pio_sm_set_pindirs_with_mask64(pio, sm, values, mask); }

int pio_set_gpio_base(PIO pio, uint base)
{ myrtos_sd_k->pio_set_gpio_base(pio, base); return 0; }

// spi_init returns the baud rate it achieved; nothing here reads it, and the
// table entry does not carry it.
uint spi_init(spi_inst_t *spi, uint baud)           { myrtos_sd_k->spi_init(spi, baud); return baud; }
uint spi_set_baudrate(spi_inst_t *spi, uint baud)
{ myrtos_sd_k->spi_set_baudrate(spi, baud); return baud; }

int spi_write_read_blocking(spi_inst_t *spi, const uint8_t *out, uint8_t *in, size_t len)
{ myrtos_sd_k->spi_write_read(spi, out, in, (uint32_t)len); return (int)len; }

// The kernel's print, under the names the driver already used.
void myrtos_print(const char *s)     { myrtos_sd_k->print(s); }
void myrtos_print_u32(uint32_t v)    { myrtos_sd_k->print_u32(v); }
void myrtos_putc(char c)             { myrtos_sd_k->putc(c); }

// The vendored driver calls panic() on three impossible conditions. In the
// kernel that was the SDK's, which resets the chip and takes the machine with
// it. This one says what happened and then stops, which it must -- panic is
// declared noreturn and there is no answer to give the caller. What it costs is
// the process that was reading the card, and not the console, the keyboard or
// the shell.
__attribute__((noreturn))
void panic(const char *fmt, ...)
{
    myrtos_print("SD: driver panic: ");
    myrtos_print(fmt);
    myrtos_print("\n");
    for (;;) { }
}
