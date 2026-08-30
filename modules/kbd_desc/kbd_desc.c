#include "../../common/myrtos_abi.h"

// Device descriptor for the USB keyboard. No configuration tail: the pins
// belong to the host controller, which there is only one of, and the layout is
// the driver's for now.
//
// The point of having this at all is that the keyboard becomes a device with a
// name. A process opens "kbd" and reads it; nothing in a utility knows that
// two PIO state machines are bit-banging USB underneath.

typedef struct {
    myrtos_descriptor_t desc;
} kbd_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const kbd_descriptor_t kbd_descriptor = {
    .desc = {
        .device_name   = "kbd",
        .driver_name   = "USBKBD  MOD",
        .device_class  = MYRTOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = 0,
        .config_size   = 0,
    },
};
