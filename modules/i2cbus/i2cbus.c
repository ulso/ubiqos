// The I2C bus, as a driver module.
//
// A process opens /dev/i2c, writes a myrtos_i2c_xfer_t followed by the bytes to
// send, and reads back whatever came in. The bus is not a byte stream -- every
// exchange names a device and says how much to say and how much to hear -- so
// the device takes a description of the exchange rather than pretending to be
// a pipe. See the note beside myrtos_i2c_xfer_t in the ABI.
//
// I2C0 on GP20 and GP21, which is where the Fruit Jam puts the Stemma QT
// connector and the audio DAC. The board has pull-ups on both, so the internal
// ones are switched on only as a courtesy to a bus with nothing on it: they are
// far too weak to drive a real one, and a Stemma cable brings its own.
#include <stdint.h>
#include <stdbool.h>
#include "../../common/myrtos_abi.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#define I2C_SDA   20
#define I2C_SCL   21
#define I2C_HZ    (100 * 1000)   // standard mode, which every Stemma board takes

static const myrtos_kernel_api_t *K;
static bool ready;
static uint8_t rx[MYRTOS_I2C_MAX_READ];
static uint32_t rx_len;

static int32_t i2c_configure(const void *config, uint32_t size)
{
    (void)config; (void)size;

    K->i2c_init(K->i2c, I2C_HZ);
    // GPIO_FUNC_I2C out of the SDK header rather than a number written
    // here. The neopixel driver had to learn that the host build's copy of
    // these differs from the target's.
    if (K->pin_claim(I2C_SDA, "i2c") < 0 || K->pin_claim(I2C_SCL, "i2c") < 0)
        K->print("i2c: a pin was already claimed\n");
    K->gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    K->gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    // Courtesy only: the RP2350's internal pull-ups are tens of kiloohms and a
    // real bus wants a few. The board has the real ones.
    K->gpio_set_pulls(I2C_SDA, true, false);
    K->gpio_set_pulls(I2C_SCL, true, false);

    ready = true;
    K->print("  i2c driver: I2C0 on GP20/GP21, 100 kHz\n");
    return 0;
}

static int32_t i2c_open(void)  { return ready ? 0 : -1; }
static int32_t i2c_close(void) { return 0; }

// One exchange. The write half ends with a repeated start rather than a stop
// when a read follows, so nothing else can take the bus between naming a
// register and reading it.
static int32_t i2c_do_write(const uint8_t *buf, uint32_t len)
{
    if (!ready) return -1;
    if (len < sizeof(myrtos_i2c_xfer_t)) return -1;

    myrtos_i2c_xfer_t x;
    const uint8_t *p = buf;
    x.addr = p[0]; x.nwrite = p[1]; x.nread = p[2]; x.reserved = p[3];

    if (x.nread > MYRTOS_I2C_MAX_READ) return -1;
    if (len < sizeof x + x.nwrite) return -1;

    rx_len = 0;

    if (x.nwrite) {
        int32_t n = K->i2c_write(K->i2c, x.addr, buf + sizeof x, x.nwrite, x.nread > 0);
        // A device that is not there does not acknowledge, and the SDK answers
        // with a negative number rather than a short count. That is the whole
        // of how a scan works.
        if (n < 0) return -1;
    }

    if (x.nread) {
        int32_t n = K->i2c_read(K->i2c, x.addr, rx, x.nread, false);
        if (n < 0) return -1;
        rx_len = (uint32_t)n;
    }

    return (int32_t)len;
}

static int32_t i2c_do_read(uint8_t *buf, uint32_t len)
{
    uint32_t n = len < rx_len ? len : rx_len;
    for (uint32_t i = 0; i < n; i++) buf[i] = rx[i];
    return (int32_t)n;
}

static int32_t i2c_readable(void) { return (int32_t)rx_len; }

static bool i2c_lib_init(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = i2c_lib_init,
    .ops = {
        .module_name = "i2cbus",
        .configure = i2c_configure,
        .open = i2c_open, .write = i2c_do_write, .read = i2c_do_read,
        .close = i2c_close, .readable = i2c_readable,
    },
};
