#include "../../common/myrtos_abi.h"

// Enhetsbeskrivare för terminalen. Ingen kod -- modulen är bara de här byten.
//
// I dag är motsvarande uppgifter hårdkodade i kernel/io.c och kernel/main.c:
// enhetsnamnet "term", UART0, GP44, 115200. Som beskrivare på kortet blir de
// utbytbara utan att kärnan byggs om.

typedef struct {
    myrtos_descriptor_t desc;
    myrtos_uart_config_t uart;
} term_descriptor_t;

__attribute__((section(".rodata.descriptor"), used))
const term_descriptor_t term_descriptor = {
    .desc = {
        .device_name   = "term",
        .driver_name   = "UART    MOD",
        .device_class  = MYRTOS_CLASS_CHAR,
        .reserved      = 0,
        .config_offset = sizeof(myrtos_descriptor_t),
        .config_size   = sizeof(myrtos_uart_config_t),
    },
    .uart = {
        .uart_base = 0x40070000u,   // UART0
        .tx_pin    = 44,            // GP44, märkt A4 på listen
        .rx_pin    = 0xffffffffu,   // ingen mottagning än
        .baud_rate = 115200,
    },
};
