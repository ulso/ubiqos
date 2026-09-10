#include "../../common/myrtos_abi.h"

// Device descriptor for the wire to the ESP32-C6. No code -- the module is
// just these bytes.
//
// Every number here was read out of "Adafruit Fruit Jam.sch" in
// adafruit/Adafruit-Fruit-Jam-PCB, because Adafruit's own pinout page has the
// strap on the wrong pin. docs/esp-hosted/README.md has the whole table and
// the parsing.

typedef struct {
    myrtos_descriptor_t desc;
    myrtos_esp_config_t esp;
} esp_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const esp_descriptor_t esp_descriptor = {
    .desc = {
        .device_name   = "esp",
        .driver_name   = "esplink",
        .device_class  = MYRTOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = sizeof(myrtos_descriptor_t),
        .config_size   = sizeof(myrtos_esp_config_t),
    },
    .esp = {
        .uart_base = 0x40078000u,   // UART1
        .tx_pin    = 8,             // D8 on the header, and the C6's RXD0
        .rx_pin    = 9,             // D9, and the C6's TXD0 through R28
        // What the C6's ROM listens at when it has been strapped into the
        // serial bootloader. Not a choice: it is the chip's own default.
        .baud_rate = 115200,
        .strap_pin = 23,            // I2S_ESP_IRQ, which is the C6's IO9/BOOT9
        .reset_pin = 22,            // PERIPH_RST: the C6's EN, and the DAC's
    },
};
