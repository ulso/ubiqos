#include "../../common/myrtos_abi.h"

// Enhetsbeskrivare för USB-konsolen. Ingen konfigurationssvans: identiteten
// ligger i USB-deskriptorerna, inte här.
__attribute__((section(".rodata.descriptor"), used))
const myrtos_descriptor_t usb_descriptor = {
    .device_name   = "usb",
    .driver_name   = "USBCDC  MOD",
    .device_class  = MYRTOS_CLASS_CHAR,
    .reserved      = 0,
    .config_offset = 0,
    .config_size   = 0,
};
