#include "../../common/ubiqos_abi.h"

// Device descriptor for the I2S output. No configuration tail: one bus, on the
// pins the board fixes, at one sample rate. A second rate is what a tail would
// carry, and it would have to be told to the DAC as well -- so the rate is not
// really this device's alone to change.
typedef struct {
    ubiqos_descriptor_t desc;
} audio_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const audio_descriptor_t audio_descriptor = {
    .desc = {
        .device_name   = "audio",
        .driver_name   = "i2sout",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = 0,
        .config_size   = 0,
    },
};
