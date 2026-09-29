// ST's NUCLEO-H563ZI: an STM32H563ZI, Cortex-M33 at 240 MHz, 2 MB of flash and
// 640 kB of SRAM, with an ST-LINK V3 on the same board.
//
// The contract a board header owes is written out in boards/fruit-jam.h. This
// one says less, because less exists yet: a console and nothing else.
#ifndef UBIQOS_BOARD_NUCLEO_H563ZI_H
#define UBIQOS_BOARD_NUCLEO_H563ZI_H

#define UBIQOS_BOARD_NAME   "ST NUCLEO-H563ZI"

// Its name on the network, since there is no card to give it one: it answers
// to nucleo-h563zi.local, not to the ubiqos.local every card-less board shares.
#define UBIQOS_DEFAULT_HOSTNAME "nucleo-h563zi"

// Pins are numbered port by port, sixteen to a port: PA0 is 0, PB0 is 16, PD8
// is 56. Nine ports, A to I, whether or not this package bonds out every pin.
#define UBIQOS_PIN(port, n) ((uint32_t)((port) - 'A') * 16u + (n))
#define UBIQOS_PIN_COUNT    144u

// The kernel's own output goes to the same USART as the shell -- USART3, the
// ST-LINK's virtual serial port -- because this board has one serial line to
// the computer and not two. The port's console driver interleaves them a line
// at a time.
#define UBIQOS_HAS_DIAG_UART 0

// The key store opens itself: it is sealed under a passphrase everybody knows,
// see UBIQOS_KEYS_DEFAULT_PASS in kernel/fsserver.c, and tried with it at boot.
// A bench board with no card has nowhere to keep a key file and nobody to type
// at it, and a store that stays shut is a store nothing can use. What that
// costs is plain: the store keeps its secrets from nobody who can read the
// flash, and the ST-LINK on this board reads it. Give the store a passphrase
// of its own and it stays shut until `key unlock`, as on any other board.
#define UBIQOS_KEYS_OPEN_BY_DEFAULT 1

// --- CLOCK AND CONSOLE ------------------------------------------------------
// No crystal is fitted for HSE. The ST-LINK drives its 8 MHz MCO into OSC_IN,
// which makes HSE an external clock in bypass mode. 240 MHz is what Zephyr runs
// this board at, under the chip's 250.
#define UBIQOS_H5_HSE_HZ        8000000u
#define UBIQOS_H5_HSE_BYPASS    1
#define UBIQOS_H5_SYSCLK_HZ     240000000u

// The ST-LINK's virtual serial port: USART3 on PD8 (TX) and PD9 (RX), AF7.
#define UBIQOS_H5_CONSOLE_PORT  'D'
#define UBIQOS_H5_CONSOLE_TX    8u
#define UBIQOS_H5_CONSOLE_RX    9u
#define UBIQOS_H5_CONSOLE_AF    7u

// --- FLASH AND RAM ----------------------------------------------------------
// 2 MB: the kernel in the first 256 kB -- it uses 123 -- the system's modules
// from there across the bank boundary, the application after them, and the
// key store in the last sectors, 8 kB each on this chip. The modules once had
// half of the second bank and had filled 95 % of it, while the kernel sat in a
// megabyte of its own. port/stm32h5/stm32h563zi.ld must say the same.
#define UBIQOS_BOARD_FLASH_MODULE_BASE 0x08040000u
#define UBIQOS_BOARD_FLASH_APP_BASE    0x08180000u
#define UBIQOS_BOARD_FLASH_END         0x081F0000u
#define UBIQOS_BOARD_FLASH_KEYS_BASE   0x081F0000u
#define UBIQOS_BOARD_FLASH_KEYS_SIZE   0x4000u

// The top 64 kB of the 640 kB of SRAM, for the data of single-instance
// programs whose code stays in flash; the linker script keeps it out.
#define UBIQOS_BOARD_FIXED_DATA_BASE   0x20090000u
#define UBIQOS_BOARD_FIXED_DATA_SIZE   0x10000u

#define UBIQOS_BOARD_FIXED_PINS                                              \
    { UBIQOS_PIN('D', 8), "console" }, { UBIQOS_PIN('D', 9), "console" },     \
    /* The Ethernet PHY's RMII and management lines, fixed by the board. */   \
    { UBIQOS_PIN('A', 1), "ethernet" }, { UBIQOS_PIN('A', 2), "ethernet" },   \
    { UBIQOS_PIN('A', 7), "ethernet" }, { UBIQOS_PIN('B', 15), "ethernet" },  \
    { UBIQOS_PIN('C', 1), "ethernet" }, { UBIQOS_PIN('C', 4), "ethernet" },   \
    { UBIQOS_PIN('C', 5), "ethernet" }, { UBIQOS_PIN('G', 11), "ethernet" },  \
    { UBIQOS_PIN('G', 13), "ethernet" },                                      \
    /* SWD and the ST-LINK's clock into OSC_IN. */                            \
    { UBIQOS_PIN('A', 13), "swd" }, { UBIQOS_PIN('A', 14), "swd" },           \
    { UBIQOS_PIN('H', 0), "hse" },

#endif
