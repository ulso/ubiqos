#ifndef MYRTOS_MODDIR_H
#define MYRTOS_MODDIR_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/modules.h"

// The module directory, in the OS-9 sense. A module exists in memory as ONE
// copy however many processes run it; each process has only its own data area.
// The link count decides when the copy may go, exactly as F$Link and F$UnLink.
//
// This rests on the module having no writable data: the build checks that .data
// and .bss are empty, so the code can be shared without two processes treading
// on each other.

// Eight was enough while the system had a shell and a couple of demos. Adding
// ls, cat, cp and rm filled it, and the descriptors -- which are registered last
// because they come last in the flash image -- were the ones turned away. The
// console then had no USB device to open and the shell fell back to the
// write-only UART, which looks exactly like a kernel that failed to boot.
//
// It happened again at thirty-two, and in the same way: adding a thirty-third
// module pushed condesc out, so there was no "con" device, no shell on the
// screen and a keyboard that did nothing. The kernel said "module directory
// full" and the line scrolled past.
//
// Twice was enough to ask why a module in flash needed an entry here at all.
// It does not: the image is its own directory, which is the whole point of the
// sync word, and a program is now found where it lies when somebody asks to run
// it. What this holds is the descriptors, whatever came off the card, and the
// flash modules that are running right now -- so thirty-two is roomier than
// sixty-four was, and the number of modules the system may have is bounded by
// flash rather than by this.
//
// The descriptors still come first in the resident image, and still should:
// they are registered eagerly, and the thing a system cannot boot without
// should not be what a full directory turns away.
#define MYRTOS_MAX_MODULES 32

typedef struct {
    const myrtos_module_header_t *header;
    uint32_t links;             // how many processes are running it
    void    *owned;             // heap memory to give back, NULL if resident
    char     name[12];
    bool     transient;         // adopted from flash for as long as it is in use
} myrtos_module_entry_t;

void  myrtos_moddir_init(void);

// Register a module already readable in memory -- in flash, or in a buffer that
// will not be freed. No copying takes place.
bool  myrtos_moddir_add_resident(const myrtos_module_header_t *header, const char *name);

// Copy a module onto the heap once and register it.
bool  myrtos_moddir_add_copy(const uint8_t *src, uint32_t len, const char *name);

// Look up and bump the link count. Returns NULL if the module does not exist.
const myrtos_module_header_t *myrtos_moddir_link(const char *name);

// Look up by a user-typed name: case-insensitive, without padding and without
// extension. "lsmod", "LSMOD" and "Lsmod" all find the same module.
const char *myrtos_moddir_match(const char *user_name);
void  myrtos_moddir_unlink(const myrtos_module_header_t *header);

uint32_t myrtos_moddir_count(void);
const myrtos_module_entry_t *myrtos_moddir_entry(uint32_t index);

#endif
