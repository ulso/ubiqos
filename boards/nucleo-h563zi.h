// ST's NUCLEO-H563ZI: an STM32H563ZI, Cortex-M33 at 240 MHz, 2 MB of flash and
// 640 kB of SRAM, with an ST-LINK V3 on the same board.
//
// The contract a board header owes is written out in boards/fruit-jam.h. This
// one says less, because less exists yet: a console and nothing else.
#ifndef UBIQOS_BOARD_NUCLEO_H563ZI_H
#define UBIQOS_BOARD_NUCLEO_H563ZI_H

#define UBIQOS_BOARD_NAME   "ST NUCLEO-H563ZI"

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
