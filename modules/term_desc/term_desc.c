#include "../../common/myrtos_abi.h"

// Device descriptor for the terminal. No code -- the module is just these bytes.
//
// The same facts are still hardcoded in kernel/io.c and kernel/main.c as a
// fallback: the device name "term", UART0, GP44, 115200. As a descriptor on the
// card they become replaceable without rebuilding the kernel.

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
        .tx_pin    = 44,            // GP44, marked A4 on the header
        .rx_pin    = 0xffffffffu,   // no receive yet
        .baud_rate = 115200,
    },
};
