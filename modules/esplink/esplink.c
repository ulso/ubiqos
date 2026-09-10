// /dev/esp -- the wire to the ESP32-C6's own UART, and the two pins that decide
// what the chip does when it comes out of reset.
//
// This is not the terminal driver with different pins. modules/uart is
// send-only and turns \n into \r\n, which is right for a console and fatal for
// a flash image: every 0x0a in it would arrive as two bytes. Here nothing is
// translated in either direction, because what travels over this wire is the
// ESP ROM loader's SLIP frames and not text.
//
// The pins are this board's, read out of "Adafruit Fruit Jam.sch" and stated in
// the descriptor rather than here -- see modules/esp_desc and
// docs/esp-hosted/README.md. Two of them are worth knowing about even so:
//
//   GP23 is the C6's IO9, which is the pin the ROM samples at reset to choose
//   between the application and the serial bootloader. It is pulled up by R27,
//   so a board nobody interferes with always boots the application. It is also
//   wired to the audio DAC's GPIO1, which is why the SDK's board header calls
//   it I2S_ESP_IRQ; myrtos never enables that DAC's GPIO, so the net has one
//   driver at a time.
//
//   GP22 is EN, and it is the DAC's reset as well. Resetting the C6 takes the
//   audio chip with it. There is no way round that in software: it is one net.
//
// The ROM wants those two moved in a sequence with tens of milliseconds between
// the steps, and a setstat runs in the trap handler with interrupts off, where
// waiting starves the display and USB. So each code here moves ONE pin and
// returns; the waiting belongs to whoever is driving, which is modules/espflash.
#include <stdint.h>
#include <stdbool.h>
#include "../../common/myrtos_abi.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

static const myrtos_kernel_api_t *K;
static uart_inst_t *esp_uart;
static uint32_t strap_pin = 0xffffffffu;
static uint32_t reset_pin = 0xffffffffu;

static int32_t esp_configure(const void *config, uint32_t size)
{
    if (size < sizeof(myrtos_esp_config_t)) return -1;
    const myrtos_esp_config_t *c = (const myrtos_esp_config_t*)config;

    esp_uart = (c->uart_base == 0x40070000u) ? uart0 : uart1;
    K->uart_init(esp_uart, c->baud_rate);

    // Claimed, so that `gpio` names this driver rather than letting two things
    // drive one pin and leaving the loser to wonder. GP22 is already the
    // board's "wifi reset" and stays that: it IS the wifi reset, and this
    // driver is the second thing with a reason to pull it.
    if (K->pin_claim(c->tx_pin, "esp") < 0 || K->pin_claim(c->rx_pin, "esp") < 0) {
        K->print("esp: the UART pins are already taken\n");
        return -1;
    }
    K->gpio_set_function(c->tx_pin, UART_FUNCSEL_NUM(esp_uart, c->tx_pin));
    K->gpio_set_function(c->rx_pin, UART_FUNCSEL_NUM(esp_uart, c->rx_pin));

    strap_pin = c->strap_pin;
    reset_pin = c->reset_pin;

    // Both left as inputs. The strap has a pull-up on the board and EN has one
    // too, so a driver that touches neither leaves the chip booting the way it
    // booted before this module existed. Nothing here may change that at
    // configure time: the wifi driver is talking to NINA over SPI by now.
    K->gpio_init(strap_pin);
    K->gpio_set_dir(strap_pin, false);
    K->gpio_init(reset_pin);
    K->gpio_set_dir(reset_pin, false);

    K->print("  esp driver: uart1, tx GP");
    K->print_u32(c->tx_pin);
    K->print(", rx GP");
    K->print_u32(c->rx_pin);
    K->print(", strap GP");
    K->print_u32(strap_pin);
    K->print(", reset GP");
    K->print_u32(reset_pin);
    K->print("\n");
    return 0;
}

static int32_t esp_open(void)  { return esp_uart ? 0 : -1; }
static int32_t esp_close(void) { return 0; }

static int32_t esp_write(const uint8_t *buf, uint32_t len)
{
    // Byte for byte. No \n translation, and no framing: this driver carries
    // what it is given.
    for (uint32_t i = 0; i < len; i++) uart_putc_raw(esp_uart, (char)buf[i]);
    return (int32_t)len;
}

static int32_t esp_read(uint8_t *buf, uint32_t len)
{
    uint32_t n = 0;
    while (n < len && uart_is_readable(esp_uart)) buf[n++] = (uint8_t)uart_getc(esp_uart);
    return (int32_t)n;
}

static int32_t esp_readable(void) { return uart_is_readable(esp_uart) ? 1 : 0; }

// A pin is held low by driving it, and released by letting go of it. Released
// is an INPUT, not a one: both nets have a pull-up on the board, and the audio
// DAC shares one of them. Driving a shared net high is how two outputs come to
// fight over it.
static void hold(uint32_t pin, bool low)
{
    if (pin == 0xffffffffu) return;
    if (low) {
        K->gpio_put(pin, false);
        K->gpio_set_dir(pin, true);
    } else {
        K->gpio_set_dir(pin, false);
    }
}

static int32_t esp_setstat(uint32_t code, const void *data, uint32_t len)
{
    if (len < sizeof(uint32_t)) return -1;
    uint32_t v = *(const uint32_t*)data;

    switch (code) {
    case MYRTOS_SS_ESP_STRAP: hold(strap_pin, v == 0); return 0;
    case MYRTOS_SS_ESP_RESET: hold(reset_pin, v == 0); return 0;
    default: return -1;
    }
}

static int32_t esp_getstat(uint32_t code, void *data, uint32_t len)
{
    if (len < sizeof(uint32_t)) return -1;
    uint32_t *out = (uint32_t*)data;

    switch (code) {
    case MYRTOS_SS_ESP_STRAP: *out = K->gpio_get(strap_pin) ? 1u : 0u; return 0;
    case MYRTOS_SS_ESP_RESET: *out = K->gpio_get(reset_pin) ? 1u : 0u; return 0;
    default: return -1;
    }
}

static bool esp_init_module(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = esp_init_module,
    .ops = {
        .module_name = "esplink",
        .configure = esp_configure,
        .open = esp_open, .write = esp_write, .read = esp_read,
        .readable = esp_readable,
        .close = esp_close,
        .getstat = esp_getstat, .setstat = esp_setstat,
    },
};
