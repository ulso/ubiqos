#ifndef UBIQOS_FLASHMOD_H
#define UBIQOS_FLASHMOD_H

#include "../common/modules.h"   // ubiqos_module_header_t

#include <stdint.h>

// Two module regions in flash, because there can be two repositories.
//
// The system image is UbiqOS's own: the kernel ends just past 24 kB, so
// starting a megabyte in leaves it ample room to grow, and the seven megabytes
// after that are the shell, the commands and the descriptors.
//
// The application image is for modules built somewhere else against the SDK. It
// exists so that an application can be shipped and updated without rebuilding
// what it runs on, and without its source ever being in this tree: two images,
// two owners, two UF2 files that can be written independently -- or combined
// into one, since every UF2 block carries its own address.
//
// A region ends where the next begins, which is what keeps an oversized system
// image from quietly swallowing the application's half.
//
// The RP2350's addresses come first in this file on purpose: CMakeLists.txt
// reads the module base out of it with a pattern and takes the first match.
#if !UBIQOS_CHIP_STM32H5
#define UBIQOS_FLASH_MODULE_BASE 0x10100000u
#define UBIQOS_FLASH_APP_BASE    0x10800000u
#define UBIQOS_FLASH_END         0x11000000u
#else
// On the STM32H5 each board says where its regions are, because the family
// runs from 128 kB of flash to 2 MB and a layout for one part is wrong for every
// other. The board header's UBIQOS_BOARD_FLASH_* -- see boards/nucleo-h563zi.h.
#include "board.h"
#define UBIQOS_FLASH_MODULE_BASE UBIQOS_BOARD_FLASH_MODULE_BASE
#define UBIQOS_FLASH_APP_BASE    UBIQOS_BOARD_FLASH_APP_BASE
#define UBIQOS_FLASH_END         UBIQOS_BOARD_FLASH_END
#endif

// The key store: two 4 kB sectors, written alternately so that losing power in
// the middle of a write leaves the older copy whole. It is the one thing in
// flash that must survive a system update, so it sits past the application
// region rather than inside anything a UF2 writes.
//
// NOT in the last sector, deliberately. A UF2 built here carries an
// RP2350-E10 block addressed at 0x10ffff00, and the boot ROM writes it -- so
// the last sector is erased every time a system is copied onto the board, and
// anything kept there would go with it.
#if !UBIQOS_CHIP_STM32H5
#define UBIQOS_FLASH_KEYS_BASE   0x10FE0000u
#define UBIQOS_FLASH_KEYS_SIZE   0x2000u
#else
#define UBIQOS_FLASH_KEYS_BASE   UBIQOS_BOARD_FLASH_KEYS_BASE
#define UBIQOS_FLASH_KEYS_SIZE   UBIQOS_BOARD_FLASH_KEYS_SIZE
#endif

// The fixed data area: RAM set aside at a known address for the writable data
// of the single-instance programs make_flash_image.py places there, so that
// their code can stay in flash -- see UBIQOS_ATTR_SPLIT. None on the RP2350,
// whose PSRAM makes a copy cost nothing worth the address. On the STM32H5 the
// board says, and its linker script keeps the area out of everything else.
#if !UBIQOS_CHIP_STM32H5
#define UBIQOS_FIXED_DATA_BASE   0u
#define UBIQOS_FIXED_DATA_SIZE   0u
#else
#define UBIQOS_FIXED_DATA_BASE   UBIQOS_BOARD_FIXED_DATA_BASE
#define UBIQOS_FIXED_DATA_SIZE   UBIQOS_BOARD_FIXED_DATA_SIZE
#endif

// Scan the flash region for module headers and register what is found as
// resident modules. They run where they lie and are never copied -- exactly
// what OS-9 did with ROM modules, and the reason a module system needs no
// filesystem to find code.
uint32_t ubiqos_flash_scan(void);

// The image is its own directory: a module is found where it lies, by walking
// the sync words. Nothing has to be registered in advance, so the size of the
// module directory does not bound how many modules the system may have.
const ubiqos_module_header_t *ubiqos_flash_nth(uint32_t index, char *name_out);
const ubiqos_module_header_t *ubiqos_flash_lookup(const char *name);

#endif
