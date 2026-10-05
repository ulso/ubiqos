// ST's NUCLEO-H503RB: an STM32H503RB, Cortex-M33 at 250 MHz, 128 kB of flash
// and 32 kB of SRAM, with an ST-LINK V3 on the same board.
//
// The small one. Everything here is the NUCLEO-H563ZI's port with less of it:
// no Ethernet, a sixteenth of the flash and a twentieth of the SRAM. What it is
// for is to find out what UbiqOS needs at the least.
//
// The contract a board header owes is written out in boards/fruit-jam.h.
#ifndef UBIQOS_BOARD_NUCLEO_H503RB_H
#define UBIQOS_BOARD_NUCLEO_H503RB_H

#define UBIQOS_BOARD_NAME   "ST NUCLEO-H503RB"
#define UBIQOS_DEFAULT_HOSTNAME "nucleo-h503rb"

// Pins port by port, sixteen to a port, as on the H563: PA0 is 0, PH0 is 112.
// The package has ports A to D and H.
#define UBIQOS_PIN(port, n) ((uint32_t)((port) - 'A') * 16u + (n))
#define UBIQOS_PIN_COUNT    128u

// One serial line to the computer, shared by the kernel and the shell.
#define UBIQOS_HAS_DIAG_UART 0

// --- CLOCK AND CONSOLE ------------------------------------------------------
// A 24 MHz crystal is fitted for HSE, so HSE is the oscillator and not a
// bypassed input. 250 MHz, the chip's limit, is what ST's own templates for
// this board run at.
#define UBIQOS_H5_HSE_HZ        24000000u
#define UBIQOS_H5_HSE_BYPASS    0
#define UBIQOS_H5_SYSCLK_HZ     250000000u

// The ST-LINK's virtual serial port is USART3 here too, but on PA4 (TX) and
// PA3 (RX) with AF13 -- ST's BSP for the Nucleo-64 H5 boards, COM1.
#define UBIQOS_H5_CONSOLE_PORT  'A'
#define UBIQOS_H5_CONSOLE_TX    4u
#define UBIQOS_H5_CONSOLE_RX    3u
#define UBIQOS_H5_CONSOLE_AF    13u

// --- FLASH AND RAM ----------------------------------------------------------
// Two banks of 64 kB, 8 kB sectors. The kernel has the first bank; the modules
// the second, less its last two sectors, which are the key store. There is no
// room for an application region: it starts where it ends.
// port/stm32/h5/stm32h503rb.ld must say the same.
#define UBIQOS_BOARD_FLASH_MODULE_BASE 0x08010000u
#define UBIQOS_BOARD_FLASH_APP_BASE    0x0801C000u
#define UBIQOS_BOARD_FLASH_END         0x0801C000u
#define UBIQOS_BOARD_FLASH_KEYS_BASE   0x0801C000u
#define UBIQOS_BOARD_FLASH_KEYS_SIZE   0x4000u

// No fixed data area: 32 kB has none to spare, and nothing that needs one --
// sshd, fetch -- would fit here anyway.
#define UBIQOS_BOARD_FIXED_DATA_BASE   0u
#define UBIQOS_BOARD_FIXED_DATA_SIZE   0u

// The kernel's tables, cut to what 32 kB can carry: see kernel/sizes.h. Eight
// processes are the kernel, the file server, the console and five more.
#define UBIQOS_BOARD_PROCESSES        8
#define UBIQOS_BOARD_PATHS            8
#define UBIQOS_BOARD_OPEN_FILES       8
#define UBIQOS_BOARD_DMESG_SIZE       2048
#define UBIQOS_BOARD_IRQ_STACK_BYTES  2048
#define UBIQOS_BOARD_CONSOLE_TX_SIZE  512u

#define UBIQOS_BOARD_FIXED_PINS                                              \
    { UBIQOS_PIN('A', 4), "console" }, { UBIQOS_PIN('A', 3), "console" },     \
    /* SWD, and the crystal across OSC_IN and OSC_OUT. */                     \
    { UBIQOS_PIN('A', 13), "swd" }, { UBIQOS_PIN('A', 14), "swd" },           \
    { UBIQOS_PIN('H', 0), "hse" }, { UBIQOS_PIN('H', 1), "hse" },

#endif
