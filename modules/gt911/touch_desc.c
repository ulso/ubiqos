#include "../../common/ubiqos_abi.h"

// /dev/touch on the Waveshare RP2350-Touch-LCD-4.3B.
//
// The board's half of the touch screen: which bus, which pins, which address.
// The driver knows the chip and nothing about the board, which is what lets the
// same gt911.c serve a second panel wired differently.
typedef struct {
    ubiqos_descriptor_t  desc;
    ubiqos_touch_config_t cfg;
} touch_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const touch_descriptor_t touch_descriptor = {
    .desc = {
        .device_name   = "touch",
        .driver_name   = "gt911",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = sizeof(ubiqos_descriptor_t),
        .config_size   = sizeof(ubiqos_touch_config_t),
    },
    .cfg = {
        .i2c_index = 1,          // I2C1; the RTC is on the same bus
        .sda_pin   = 6,
        .scl_pin   = 7,
        .int_pin   = 16,
        .rst_pin   = 17,
        .addr      = 0x5du,      // which the reset selects: see chip_reset
        .baud      = 400000u,
    },
};
