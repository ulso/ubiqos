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
#define UBIQOS_FLASH_MODULE_BASE 0x10100000u
#define UBIQOS_FLASH_APP_BASE    0x10800000u
#define UBIQOS_FLASH_END         0x11000000u

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
