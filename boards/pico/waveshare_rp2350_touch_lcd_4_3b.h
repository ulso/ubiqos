// Waveshare RP2350-Touch-LCD-4.3B, for the Pico SDK.
//
// Written here rather than taken from Waveshare's demo: the SDK does not ship a
// header for this board, and what the SDK needs is a short list of facts that
// are better stated with their source than copied. Each is from the board's own
// documentation or its schematic.
//
// UbiqOS's own view of the same board is boards/ws43b.h. This file is only what
// the SDK asks for -- clocks, flash, PSRAM and the default peripherals.
#ifndef _BOARDS_WAVESHARE_RP2350_TOUCH_LCD_4_3B_H
#define _BOARDS_WAVESHARE_RP2350_TOUCH_LCD_4_3B_H

#define WAVESHARE_RP2350_TOUCH_LCD_4_3B

// An RP2350B: 48 GPIOs, not the A's 30. The LCD alone wants twenty of them.
#define PICO_RP2350A 0
#define PICO_RP2350_A2_SUPPORTED 1

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#define PICO_FLASH_SPI_CLKDIV 2
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)

// 2 MB, against the Fruit Jam's 8. Modules are loaded into PSRAM, so this is
// the number to watch when a build stops fitting.
//
// Chip select 47 and not 0. Waveshare's own psram_tool.h carries 0 as a default
// and their rp_pico_alloc.h says 47; 47 is the one that can be true here,
// because GPIO0 is this board's UART0 TX pad and cannot also be a chip select.
#define PICO_PSRAM_CS_PIN 47
#define PICO_PSRAM_SIZE_BYTES (2 * 1024 * 1024)

// The pads marked RX and TX. On this board they are the only broken-out pins,
// which is why UbiqOS uses them for a PIO USB host instead -- see
// boards/ws43b.h, where there is deliberately no diagnostic UART as a result.
#define PICO_DEFAULT_UART 0
#define PICO_DEFAULT_UART_TX_PIN 0
#define PICO_DEFAULT_UART_RX_PIN 1

// The touch controller and the real-time clock share this bus.
#define PICO_DEFAULT_I2C 1
#define PICO_DEFAULT_I2C_SDA_PIN 6
#define PICO_DEFAULT_I2C_SCL_PIN 7

#endif
