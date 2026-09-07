// The serial terminal, as a driver module.
//
// This was the first driver in kernel/io.c, and the note above it said for
// weeks that the next step was to lift it out as a module of its own. This is
// that step. It is not about SRAM -- the six drivers in io.c came to 863 bytes
// between them -- it is about the model: a descriptor has always been a file on
// the card saying which driver handles a device, and until now the driver it
// named had to be one the kernel was built with. Now it does not.
//
// Everything about the device is unchanged. /dev/term is registered from the
// same descriptor, by the same myrtos_io_add_descriptor, through the same
// vtable. What changed is where the vtable comes from.
//
// Like sdlib, this compiles against the SDK's headers -- the UART register
// layout is a fixed address that travels with nobody -- and takes the one real
// function it needs, uart_init, from the kernel's table.
#include <stdint.h>
#include <stdbool.h>
#include "../../common/myrtos_abi.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

static const myrtos_kernel_api_t *K;
static uart_inst_t *term_uart;
static uint32_t term_tx_pin;

static void print_hex(uint32_t v)
{
    // print_u32 is decimal, and a peripheral base address wants to be readable
    // as the number in the datasheet. Eight digits, no shortening: 0x40070000
    // and 0x40078000 differ in the middle.
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t d = (v >> shift) & 0xf;
        K->putc((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
}

static int32_t term_configure(const void *config, uint32_t size)
{
    if (size < sizeof(myrtos_uart_config_t)) return -1;
    const myrtos_uart_config_t *c = (const myrtos_uart_config_t*)config;

    term_uart = (c->uart_base == 0x40070000u) ? uart0 : uart1;
    term_tx_pin = c->tx_pin;

    K->uart_init(term_uart, c->baud_rate);
    K->gpio_set_function(term_tx_pin, UART_FUNCSEL_NUM(term_uart, term_tx_pin));

    K->print("  uart driver: base 0x");
    print_hex(c->uart_base);
    K->print(", tx GP");
    K->print_u32(c->tx_pin);
    K->print(", ");
    K->print_u32(c->baud_rate);
    K->print(" baud\n");
    return 0;
}

static int32_t term_open(void) { return term_uart ? 0 : -1; }
static int32_t term_close(void) { return 0; }

static int32_t term_write(const uint8_t *buf, uint32_t len)
{
    // Called from the trap handler, hence with interrupts off. The whole
    // write is therefore atomic without any lock.
    //
    // uart_putc_raw is inline in the SDK and writes the data register after a
    // busy-wait on the FIFO, so it compiles into this module and needs nothing
    // from the kernel. That is the ordinary case for the SDK's hardware layer
    // and the reason a driver module is cheap: only the real functions have to
    // be handed over.
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == '\n') uart_putc_raw(term_uart, '\r');
        uart_putc_raw(term_uart, (char)buf[i]);
    }
    return (int32_t)len;
}

// UART receive: the descriptor does not set rx_pin yet, so there is nothing to
// read. The function exists to keep the interface complete.
static int32_t term_read(uint8_t *buf, uint32_t len)
{
    (void)buf; (void)len;
    return 0;
}

static bool uart_init_module(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = uart_init_module,
    .ops = {
        .module_name = "uart",
        .configure = term_configure,
        .open = term_open, .write = term_write, .read = term_read,
        .close = term_close,
    },
};
