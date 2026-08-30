// The ESP32-C6 on the Fruit Jam, over SPI.
//
// The chip runs NINA firmware, which carries its own TCP/IP stack: the host does
// not run one. It sends commands -- connect to this network, open this socket,
// send these bytes -- and the chip does the rest. That is why there is no lwIP
// here, and why the port of it in modules/lwipd is set aside rather than
// finished. See docs/lwip.
//
// The framing is Arduino's WiFiNINA protocol, which Adafruit forked:
//
//   to the chip     0xE0, command, parameter count, [parameters,] 0xEE
//   from the chip   0xE0, command|0x80, parameter count, length, data, 0xEE
//
// with a handshake line either side of it. The chip holds ACK low when it has
// nothing to say; it raises it once selected and ready to transfer. Both waits
// have timeouts, because a chip that is not there would otherwise hang the boot
// -- which is a lesson this project has already paid for once.

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

#define WIFI_SPI    spi1
#define WIFI_SCK    30
#define WIFI_MOSI   31
#define WIFI_MISO   28
#define WIFI_CS     46
#define WIFI_ACK     3

#define START_CMD   0xE0u
#define END_CMD     0xEEu
#define ERR_CMD     0xEFu
#define REPLY_FLAG  0x80u
#define GET_FW_VERSION_CMD 0x37u

static uint8_t xfer(uint8_t v) {
    uint8_t r = 0;
    spi_write_read_blocking(WIFI_SPI, &v, &r, 1);
    return r;
}

// Wait for the handshake line to reach a level, giving up rather than spinning.
static bool wait_ack(bool level, uint32_t ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (gpio_get(WIFI_ACK) != level) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
    }
    return true;
}

static bool select_chip(void) {
    if (!wait_ack(false, 10)) return false;      // not busy
    gpio_put(WIFI_CS, 0);
    return wait_ack(true, 10);                   // selected and ready
}

static void deselect_chip(void) { gpio_put(WIFI_CS, 1); }

void myrtos_wifi_init(void) {
    spi_init(WIFI_SPI, 8 * 1000 * 1000);
    gpio_set_function(WIFI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(WIFI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(WIFI_MISO, GPIO_FUNC_SPI);

    gpio_init(WIFI_CS);
    gpio_set_dir(WIFI_CS, GPIO_OUT);
    gpio_put(WIFI_CS, 1);

    gpio_init(WIFI_ACK);
    gpio_set_dir(WIFI_ACK, GPIO_IN);
}

// Ask the chip what firmware it is running. A version string coming back settles
// three things at once: the wiring, the handshake, and that the chip really does
// speak NINA rather than something that would have needed a stack of our own.
bool myrtos_wifi_firmware(char *out, uint32_t max) {
    if (!select_chip()) { myrtos_print("WiFi: no handshake\n"); return false; }
    xfer(START_CMD);
    xfer(GET_FW_VERSION_CMD & ~REPLY_FLAG);
    xfer(0);                                     // no parameters
    xfer(END_CMD);
    deselect_chip();

    if (!select_chip()) { myrtos_print("WiFi: no reply\n"); return false; }

    // The chip pads with 0xFF until it has something; read past that to the
    // start byte rather than assuming the first byte is meaningful.
    uint8_t b = 0;
    for (int i = 0; i < 64; i++) {
        b = xfer(0xff);
        if (b == START_CMD || b == ERR_CMD) break;
    }
    if (b != START_CMD) { deselect_chip(); myrtos_print("WiFi: no start byte\n"); return false; }

    uint8_t cmd = xfer(0xff);
    uint8_t nparam = xfer(0xff);
    if (cmd != (GET_FW_VERSION_CMD | REPLY_FLAG) || nparam != 1) {
        deselect_chip();
        myrtos_print("WiFi: unexpected reply\n");
        return false;
    }

    uint32_t len = xfer(0xff);
    uint32_t i = 0;
    for (; i < len; i++) {
        uint8_t v = xfer(0xff);
        if (i < max - 1) out[i] = (char)v;
    }
    out[i < max ? i : max - 1] = 0;
    xfer(0xff);                                  // END_CMD
    deselect_chip();
    return true;
}

void myrtos_wifi_probe(void) {
    char version[16];
    myrtos_wifi_init();
    if (myrtos_wifi_firmware(version, sizeof(version))) {
        myrtos_print("WiFi: ESP32-C6 firmware ");
        myrtos_print(version);
        myrtos_print("\n");
    }
}
