#include "../../common/myrtos_abi.h"

typedef struct { myrtos_descriptor_t desc; } gpio_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const gpio_descriptor_t gpio_descriptor = {
    .desc = {
        .device_name   = "gpio",
        .driver_name   = "gpiodev",
        .device_class  = MYRTOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = 0,
        .config_size   = 0,
    },
};
