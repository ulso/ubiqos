#include "../../common/ubiqos_abi.h"

// Device descriptor for the I2C bus. No configuration tail yet: there is one
// bus brought out to the Stemma connector, on the pins the board fixes. A
// second bus, or a different rate, is what a tail would carry.
typedef struct {
    ubiqos_descriptor_t desc;
} i2c_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const i2c_descriptor_t i2c_descriptor = {
    .desc = {
        .device_name   = "i2c",
        .driver_name   = "i2cbus",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = 0,
        .config_size   = 0,
    },
};
