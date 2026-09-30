#include "../../common/ubiqos_abi.h"

// Device descriptor for ESP-Hosted's SPI transport on iLabs' Challenger+ RP2350
// WiFi6/BLE5. No code -- the module is just these bytes, and they differ from
// the Fruit Jam's (eh_desc.c) only in the pins.
//
// SPI1 on GP8 to GP11, the handshake on GP22 and DATA READY on GP14, from
// iLabs' schematic 54-00314-1 rev P1.2 and the SDK's board header, which agree.

typedef struct {
    ubiqos_descriptor_t desc;
    ubiqos_ehspi_config_t spi;
} eh_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const eh_descriptor_t eh_descriptor = {
    .desc = {
        .device_name   = "eh",
        .driver_name   = "ehspi",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = sizeof(ubiqos_descriptor_t),
        .config_size   = sizeof(ubiqos_ehspi_config_t),
    },
    .spi = {
        .sck_pin         = 10,
        .mosi_pin        = 11,
        .miso_pin        = 8,
        .cs_pin          = 9,
        .handshake_pin   = 22,   // the C6's IO3
        .data_ready_pin  = 14,   // the C6's IO9, which is also its boot strap
        // Thirty-two megahertz. The NINA driver ran these same pins at eight
        // for months, which is where this started, and the difference is
        // measured rather than assumed: a 1600-byte exchange took 1720 us at
        // 8 MHz and takes 534 us at 32, with the co-processor's announcement
        // still decoding and no checksum errors either way.
        //
        // That matters more than throughput. The exchange is CPU held at
        // priority 21, ABOVE THE SHELL, because the SDK's spi_write_read polls
        // a byte at a time. Espressif's own note says ESP32 up to 10 MHz and
        // everything else up to 40, and their reference numbers are taken at
        // 40 -- so there is more here, but the honest fix is DMA rather than
        // a bigger number.
        .baud_rate       = 32u * 1000u * 1000u,
    },
};
