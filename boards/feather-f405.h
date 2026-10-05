// Adafruit's Feather STM32F405 Express: an STM32F405RG, Cortex-M4 at 168 MHz,
// 1 MB of flash, 128 kB of SRAM and 64 kB of core-coupled memory, with a 2 MB
// SPI flash, a micro-SD socket, a STEMMA QT I2C connector and USB on the chip.
//
// No debugger on the board: SWDIO and SWCLK are pads underneath, and a J-Link
// soldered to them flashes it. The console is USART3 on the Feather's TX and
// RX pins, to a USB-serial adapter.
//
// The contract a board header owes is written out in boards/fruit-jam.h.
#ifndef UBIQOS_BOARD_FEATHER_F405_H
#define UBIQOS_BOARD_FEATHER_F405_H

#define UBIQOS_BOARD_NAME   "Adafruit Feather STM32F405 Express"
#define UBIQOS_DEFAULT_HOSTNAME "feather-f405"

// Pins port by port, sixteen to a port: PA0 is 0, PB10 is 26. The package
// bonds out ports A to C, PD2 and PH0-1; the numbering leaves room for A to I.
#define UBIQOS_PIN(port, n) ((uint32_t)((port) - 'A') * 16u + (n))
#define UBIQOS_PIN_COUNT    144u

// One serial line to the computer, shared by the kernel and the shell.
#define UBIQOS_HAS_DIAG_UART 0

// The key store opens itself, as on the NUCLEO-H563ZI and for the same reason:
// a bench board with nowhere to keep a key file. Whoever holds a debugger to
// its pads can read the store; give it a passphrase of its own to keep it shut.
#define UBIQOS_KEYS_OPEN_BY_DEFAULT 1

// --- CLOCK AND CONSOLE ------------------------------------------------------
// A 12 MHz crystal for HSE, to 168 MHz, the chip's limit; the PLL's 48 MHz
// output then falls out exactly for USB, SDIO and the RNG.
#define UBIQOS_STM32_HSE_HZ        12000000u
#define UBIQOS_STM32_HSE_BYPASS    0
#define UBIQOS_STM32_SYSCLK_HZ     168000000u

// USART3 on PB10 (TX) and PB11 (RX), AF7: the pins marked TX and RX.
#define UBIQOS_STM32_CONSOLE_PORT  'B'
#define UBIQOS_STM32_CONSOLE_TX    10u
#define UBIQOS_STM32_CONSOLE_RX    11u
#define UBIQOS_STM32_CONSOLE_AF    7u

// --- FLASH AND RAM ----------------------------------------------------------
// See port/stm32/f4/stm32f405rg.ld, which must say the same: the key store in
// sectors 1 and 2, the settings in sector 3 (the sector after the key store,
// as kernel/flashmod.h has it), the kernel in sectors 4 and 5, and the modules
// in the six 128 kB sectors after. No application region: it starts where it
// ends.
#define UBIQOS_BOARD_FLASH_MODULE_BASE 0x08040000u
#define UBIQOS_BOARD_FLASH_APP_BASE    0x08100000u
#define UBIQOS_BOARD_FLASH_END         0x08100000u
#define UBIQOS_BOARD_FLASH_KEYS_BASE   0x08004000u
#define UBIQOS_BOARD_FLASH_KEYS_SIZE   0x8000u

// No fixed data area yet: the programs that use one are the network's.
#define UBIQOS_BOARD_FIXED_DATA_BASE   0u
#define UBIQOS_BOARD_FIXED_DATA_SIZE   0u

#define UBIQOS_BOARD_FIXED_PINS                                              \
    { UBIQOS_PIN('B', 10), "console" }, { UBIQOS_PIN('B', 11), "console" },   \
    /* SWD, and the crystal across OSC_IN and OSC_OUT. */                     \
    { UBIQOS_PIN('A', 13), "swd" }, { UBIQOS_PIN('A', 14), "swd" },           \
    { UBIQOS_PIN('H', 0), "hse" }, { UBIQOS_PIN('H', 1), "hse" },

#endif
