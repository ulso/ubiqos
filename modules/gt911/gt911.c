#include <stdint.h>
#include <stdbool.h>
#include "../../common/myrtos_abi.h"
#include "hardware/gpio.h"

// gt911 -- the capacitive touch controller on the Waveshare 4.3B panel.
//
// A driver module, so nothing about it is in the kernel, and its pins come from
// its descriptor rather than from here -- see myrtos_touch_config_t. The board
// is in the descriptor; this file is the chip.
//
// The bus is whichever I2C the descriptor names, and that is the reason this
// driver can exist at all without touching the kernel: K->i2c_init and friends
// take the instance as an argument, so a second bus is a parameter rather than
// a new entry in the kernel API. K->i2c is the first one; this asks for the
// other.

static const myrtos_kernel_api_t *K;
static myrtos_touch_config_t cfg;
static void *bus;
static bool ready;

// Sixteen-bit register addresses, big-endian on the wire.
#define GT911_PRODUCT_ID 0x8140u
#define GT911_STATUS     0x814eu     // bit 7 says the buffer is fresh, bits 3-0 count

// The configuration block, which is the chip's own answer to what range it
// reports in. Little endian here, unlike the addresses, and the datasheet's
// "output max" is a count: this panel answers 800 by 480 and a coordinate runs
// 0 to 799.
#define GT911_CONFIG_VER 0x8047u     // then the width, the height, and the finger count
#define GT911_FIRMWARE   0x8144u

static bool reg_read(uint16_t reg, uint8_t *dst, uint32_t n)
{
    const uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    if (K->i2c_write(bus, (uint8_t)cfg.addr, a, 2, true) != 2) return false;
    return K->i2c_read(bus, (uint8_t)cfg.addr, dst, n, false) == (int32_t)n;
}

static bool reg_write8(uint16_t reg, uint8_t v)
{
    const uint8_t a[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, v };
    return K->i2c_write(bus, (uint8_t)cfg.addr, a, 3, false) == 3;
}

// The reset decides the address, which is the part worth knowing: the chip
// samples its interrupt pin as it leaves reset, and low means 0x5d. So the pin
// is an OUTPUT held low across the reset and only afterwards an input.
static void chip_reset(void)
{
    K->gpio_set_function(cfg.rst_pin, GPIO_FUNC_SIO);
    K->gpio_set_function(cfg.int_pin, GPIO_FUNC_SIO);
    K->gpio_set_dir(cfg.rst_pin, true);
    K->gpio_set_dir(cfg.int_pin, true);
    K->gpio_put(cfg.int_pin, 0);

    K->gpio_put(cfg.rst_pin, 1);
    K->sleep_ms(50);
    K->gpio_put(cfg.rst_pin, 0);
    K->sleep_ms(50);
    K->gpio_put(cfg.rst_pin, 1);
    K->sleep_ms(250);

    K->gpio_set_dir(cfg.int_pin, false);   // and now it is the chip's to drive
}

static int32_t gt911_configure(const void *config, uint32_t size)
{
    if (!config || size < sizeof cfg) {
        K->print("  gt911: no configuration; the descriptor must carry one\n");
        return -1;
    }
    const uint8_t *src = (const uint8_t *)config;
    uint8_t *dst = (uint8_t *)&cfg;
    for (uint32_t i = 0; i < sizeof cfg; i++) dst[i] = src[i];

    bus = K->i2c_instance(cfg.i2c_index);
    if (!bus) { K->print("  gt911: no such I2C bus\n"); return -1; }

    if (K->pin_claim(cfg.sda_pin, "touch") < 0 || K->pin_claim(cfg.scl_pin, "touch") < 0 ||
        K->pin_claim(cfg.int_pin, "touch") < 0 || K->pin_claim(cfg.rst_pin, "touch") < 0)
        K->print("  gt911: a pin was already claimed\n");

    K->i2c_init(bus, cfg.baud);
    K->gpio_set_function(cfg.sda_pin, GPIO_FUNC_I2C);
    K->gpio_set_function(cfg.scl_pin, GPIO_FUNC_I2C);
    K->gpio_set_pulls(cfg.sda_pin, true, false);
    K->gpio_set_pulls(cfg.scl_pin, true, false);

    chip_reset();

    // Asked once and believed or not. The part answers "911" in its product id,
    // and saying so is the difference between "no fingers" and "no chip" --
    // which read alone can never tell apart.
    uint8_t id[5] = {0};
    if (!reg_read(GT911_PRODUCT_ID, id, 4)) {
        K->print("  gt911: the bus does not answer\n");
        return -1;
    }
    if (id[0] != '9' || id[1] != '1' || id[2] != '1') {
        K->print("  gt911: something answered, but not a GT911\n");
        return -1;
    }

    ready = true;
    K->print("  touch driver: GT911 on I2C");
    K->print_u32(cfg.i2c_index);
    K->print(", up to 5 points\n");     // what this driver returns, not what the
                                        // chip's configuration claims

    return 0;
}

static int32_t gt911_open(void)  { return ready ? 0 : -1; }
static int32_t gt911_close(void) { return 0; }

// A poll, never a wait. The controller sets bit 7 when the buffer is fresh and
// the count is in the low nibble; the buffer must then be cleared by hand or it
// is never refreshed again -- which is the one thing in this protocol that bites
// if it is forgotten.
static int32_t gt911_read(uint8_t *out, uint32_t len)
{
    if (!ready) return -1;

    myrtos_touch_t t = { 0, { { 0, 0 } } };

    uint8_t st = 0;
    if (reg_read(GT911_STATUS, &st, 1) && (st & 0x80u)) {
        uint32_t n = st & 0x0fu;
        if (n > MYRTOS_TOUCH_MAX) n = MYRTOS_TOUCH_MAX;

        uint8_t raw[MYRTOS_TOUCH_MAX * 8];
        if (n && reg_read(GT911_STATUS + 1u, raw, n * 8u)) {
            t.points = n;
            for (uint32_t i = 0; i < n; i++) {
                // Each point is eight bytes: a track id, then x and y little
                // endian, then a size nobody here has a use for.
                t.p[i].x = (uint16_t)(raw[i * 8 + 1] | ((uint16_t)raw[i * 8 + 2] << 8));
                t.p[i].y = (uint16_t)(raw[i * 8 + 3] | ((uint16_t)raw[i * 8 + 4] << 8));
            }
        }
        reg_write8(GT911_STATUS, 0);
    }

    uint32_t n = len < sizeof t ? len : sizeof t;
    const uint8_t *src = (const uint8_t *)&t;
    for (uint32_t i = 0; i < n; i++) out[i] = src[i];
    return (int32_t)n;
}

// Always: a poll has an answer even when the answer is nobody is touching it.
static int32_t gt911_readable(void) { return ready ? (int32_t)sizeof(myrtos_touch_t) : 0; }

// What the chip says it is, rather than what the board header assumes. Asked
// once by anything that has to turn a coordinate into a pixel: if these come
// back as the panel's own size there is nothing to calibrate, and if they do
// not, this is the pair to scale by.
static int32_t gt911_getstat(uint32_t code, void *data, uint32_t len)
{
    if (code != MYRTOS_SS_TOUCH_RANGE) return -1;
    if (!ready) return -1;
    if (len < sizeof(myrtos_touch_range_t)) return -1;

    uint8_t c[6] = { 0, 0, 0, 0, 0, 0 };     // version, width, height, points
    uint8_t fw[2] = { 0, 0 };
    if (!reg_read(GT911_CONFIG_VER, c, sizeof c)) return -1;
    if (!reg_read(GT911_FIRMWARE, fw, sizeof fw)) return -1;

    myrtos_touch_range_t r;
    r.width    = (uint16_t)(c[1] | ((uint16_t)c[2] << 8));
    r.height   = (uint16_t)(c[3] | ((uint16_t)c[4] << 8));
    r.points   = (uint16_t)(c[5] & 0x0fu);
    r.firmware = (uint16_t)(fw[0] | ((uint16_t)fw[1] << 8));
    r.config   = c[0];
    r.reserved = 0;

    uint8_t *dst = (uint8_t *)data;
    const uint8_t *src = (const uint8_t *)&r;
    for (uint32_t i = 0; i < sizeof r; i++) dst[i] = src[i];
    return 0;
}

static bool gt911_lib_init(const myrtos_kernel_api_t *api)
{
    K = api;
    return true;
}

const myrtos_driver_module_t myrtos_driver = {
    .abi = MYRTOS_DRIVER_ABI,
    .reserved = 0,
    .init = gt911_lib_init,
    .ops = {
        .module_name = "gt911",
        .configure = gt911_configure,
        .open = gt911_open, .write = 0, .read = gt911_read,
        .close = gt911_close, .readable = gt911_readable,
        .getstat = gt911_getstat,
    },
};
