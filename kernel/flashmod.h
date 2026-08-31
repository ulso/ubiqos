#ifndef MYRTOS_FLASHMOD_H
#define MYRTOS_FLASHMOD_H

#include "../common/modules.h"   // myrtos_module_header_t

#include <stdint.h>

// The module region in flash. The kernel image ends just past 24 kB; starting a
// megabyte in leaves it ample room to grow. The rest -- close to 15 MB -- is
// modules.
#define MYRTOS_FLASH_MODULE_BASE 0x10100000u
#define MYRTOS_FLASH_END         0x11000000u

// Scan the flash region for module headers and register what is found as
// resident modules. They run where they lie and are never copied -- exactly
// what OS-9 did with ROM modules, and the reason a module system needs no
// filesystem to find code.
uint32_t myrtos_flash_scan(void);

// The image is its own directory: a module is found where it lies, by walking
// the sync words. Nothing has to be registered in advance, so the size of the
// module directory does not bound how many modules the system may have.
const myrtos_module_header_t *myrtos_flash_nth(uint32_t index, char *name_out);
const myrtos_module_header_t *myrtos_flash_lookup(const char *name);

#endif
