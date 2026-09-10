#include "../../common/myrtos_abi.h"

// Device descriptor for ESP-Hosted's SPI transport. No code -- the module is
// just these bytes.
//
// The four SPI pins are the ones WiFiNINA used, because they go to the same
// chip; what is new is the pair after them. docs/esp-hosted/README.md has the
// whole table, read out of the board's schematic and since confirmed by the
// co-processor's own log.

typedef struct {
    myrtos_descriptor_t desc;
    myrtos_ehspi_config_t spi;
} eh_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const eh_descriptor_t eh_descriptor = {
    .desc = {
        .device_name   = "eh",
        .driver_name   = "ehspi",
        .device_class  = MYRTOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = sizeof(myrtos_descriptor_t),
        .config_size   = sizeof(myrtos_ehspi_config_t),
    },
    .spi = {
        .sck_pin         = 30,
        .mosi_pin        = 31,
        .miso_pin        = 28,
        .cs_pin          = 46,
        .handshake_pin   = 3,    // the C6's IO18
        .data_ready_pin  = 23,   // the C6's IO9, which is also its boot strap
        // Eight megahertz, which is what the NINA driver ran these same pins
        // at for months. The co-processor takes whatever the host gives it --
        // its log says Freq:ConfigAtHost -- so this is a number to raise once
        // the link is proven, not a number the other end cares about now.
        .baud_rate       = 8u * 1000u * 1000u,
    },
};
