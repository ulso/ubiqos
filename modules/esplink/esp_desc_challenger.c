#include "../../common/ubiqos_abi.h"

// Device descriptor for the wire to the ESP32-C6 on iLabs' Challenger+ RP2350
// WiFi6/BLE5: the serial line its bootloader listens on, and the two pins that
// put it there. No code -- the module is just these bytes, and they differ from
// the Fruit Jam's (esp_desc.c) only in the pins.
//
// From iLabs' schematic 54-00314-1 rev P1.2: UART1 on GP4 and GP5, the C6's EN
// on GP15 and its IO9 on GP14.

typedef struct {
    ubiqos_descriptor_t desc;
    ubiqos_esp_config_t esp;
} esp_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const esp_descriptor_t esp_descriptor = {
    .desc = {
        .device_name   = "esp",
        .driver_name   = "esplink",
        .device_class  = UBIQOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = sizeof(ubiqos_descriptor_t),
        .config_size   = sizeof(ubiqos_esp_config_t),
    },
    .esp = {
        .uart_base = 0x40078000u,   // UART1
        .tx_pin    = 4,             // the C6's RXD0
        .rx_pin    = 5,             // the C6's TXD0
        // What the C6's ROM listens at when it has been strapped into the
        // serial bootloader. Not a choice: it is the chip's own default.
        .baud_rate = 115200,
        .strap_pin = 14,            // the C6's IO9: boot strap, then DATA READY
        .reset_pin = 15,            // the C6's EN, its own
    },
};
