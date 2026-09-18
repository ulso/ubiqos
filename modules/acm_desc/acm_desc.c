#include "../../common/ubiqos_abi.h"

// Device descriptor for a CDC-ACM device on the host side: the serial port at
// the other end of the USB socket rather than at the other end of the cable to
// the Mac. A BLE dongle, a modem, a sensor.
//
// No configuration tail. Which device is plugged in is not the descriptor's
// business, and the line coding is asked for as the device is enumerated --
// there is no wire to run at that rate anyway.
typedef struct {
    ubiqos_descriptor_t desc;
} acm_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const acm_descriptor_t acm_descriptor = {
    .desc = {
        .device_name   = "acm",
        .driver_name   = "acm",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = 0,
        .config_size   = 0,
    },
};
