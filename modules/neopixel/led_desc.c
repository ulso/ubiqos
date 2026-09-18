#include "../../common/ubiqos_abi.h"

// Device descriptor for the five RGB LEDs. No configuration tail: there is one
// strip, on one pin, and the driver knows where it is. If a second board ever
// wires them elsewhere, the pin belongs in a tail here rather than in the code.
typedef struct {
    ubiqos_descriptor_t desc;
} led_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const led_descriptor_t led_descriptor = {
    .desc = {
        .device_name   = "leds",
        .driver_name   = "neopixel",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = 0,
        .config_size   = 0,
    },
};
