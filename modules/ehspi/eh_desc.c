#include "../../common/ubiqos_abi.h"

// Device descriptor for ESP-Hosted's SPI transport. No code -- the module is
// just these bytes.
//
// The four SPI pins are the ones WiFiNINA used, because they go to the same
// chip; what is new is the pair after them. docs/esp-hosted/README.md has the
// whole table, read out of the board's schematic and since confirmed by the
// co-processor's own log.

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
        .sck_pin         = 30,
        .mosi_pin        = 31,
        .miso_pin        = 28,
        .cs_pin          = 46,
        .handshake_pin   = 3,    // the C6's IO18
        .data_ready_pin  = 23,   // the C6's IO9, which is also its boot strap
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
