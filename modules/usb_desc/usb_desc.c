#include "../../common/myrtos_abi.h"

// Device descriptor for the USB console. No configuration tail: the identity
// lives in the USB descriptors, not here.
__attribute__((section(".rodata.descriptor"), used))
const myrtos_descriptor_t usb_descriptor = {
    .device_name   = "usb",
    .driver_name   = "usbcdc",
    .device_class  = MYRTOS_CLASS_CHAR,
    .reserved      = 0,
    .config_offset = 0,
    .config_size   = 0,
};
