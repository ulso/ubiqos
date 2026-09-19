#include "flashmod.h"
#include "moddir.h"

void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
void ubiqos_print_hex(uint32_t v);
bool verify_ubiqos_header(ubiqos_module_header_t *header);

// The name lives in the module, but moddir wants eleven characters in 8.3
// form. A module in flash has no filename, so the name is derived from the
// module's own string.
// The module's own name, copied. It used to be padded to eight and given the
// extension "MOD" -- so a module called cxxdemo became "cxxdemo MOD" inside the
// kernel, and the boot log said "termdescMOD" because that is what it was. The
// module never carried an extension; this manufactured one, to match the 8.3
// name the same module would have had coming off a FAT card. Neither end needs
// it any more.
static void name_from_module(const ubiqos_module_header_t *m, char *out) {
    const char *src = (const char*)m + m->name_offset;
    int i = 0;
    for (; i < UBIQOS_NAME_LEN - 1 && src[i]; i++) out[i] = src[i];
    out[i] = 0;
}

// One step through the image, from *p, which is advanced past the module.
// Returns NULL at the end -- and the end is the first word that is not a
// module, not the end of the region.
//
// Searching on was worse than slow. Loading an image smaller than the one
// before it leaves the old tail in flash, and picotool writes only as many
// bytes as the file has -- so the scan found the modules at the end of the
// previous image as well, and the directory listed echo, lsmod, free and both
// descriptors twice.
static const ubiqos_module_header_t *flash_step(uintptr_t *p, uintptr_t end) {
    if (*p + sizeof(ubiqos_module_header_t) >= end) return 0;
    ubiqos_module_header_t *m = (ubiqos_module_header_t*)*p;

    // Unwritten flash reads as 0xFFFFFFFF, and make_flash_image.py writes a
    // terminator, so either way this is where the image ends.
    if (m->sync_code != UBIQOS_SYNC_CODE) return 0;
    if (!m->module_size || *p + m->module_size > end) return 0;
    if (!verify_ubiqos_header(m)) return 0;

    *p += (m->module_size + 3u) & ~3u;   // the next may start right after
    return m;
}

// The image is the directory. There is no need to copy it into another one:
// OS-9 read modules straight out of ROM this way, and the whole point of the
// sync word is that a module can be found where it lies.
// The regions, in the order they are searched, so the system's own modules are
// found before an application's. That order is the tie-break for a name in
// both, and having the system win is the safer way round: an application cannot
// shadow the shell by naming a module after it.
#define UBIQOS_FLASH_REGIONS 2u

static uintptr_t region_base(uint32_t i)
{
    return i == 0 ? UBIQOS_FLASH_MODULE_BASE : UBIQOS_FLASH_APP_BASE;
}

static uintptr_t region_end(uint32_t i)
{
    return i == 0 ? UBIQOS_FLASH_APP_BASE : UBIQOS_FLASH_END;
}

const ubiqos_module_header_t *ubiqos_flash_nth(uint32_t index, char *name_out) {
    for (uint32_t r = 0; r < UBIQOS_FLASH_REGIONS; r++) {
        uintptr_t p = region_base(r);
        const uintptr_t end = region_end(r);
        for (;;) {
            const ubiqos_module_header_t *m = flash_step(&p, end);
            if (!m) break;                  // this region's end, not the last
            if (index-- == 0) {
                if (name_out) name_from_module(m, name_out);
                return m;
            }
        }
    }
    return 0;
}

// By the eleven-character directory name, padded, as the directory stores it.
const ubiqos_module_header_t *ubiqos_flash_lookup(const char *name) {
    char n[UBIQOS_NAME_LEN];
    for (uint32_t i = 0; ; i++) {
        const ubiqos_module_header_t *m = ubiqos_flash_nth(i, n);
        if (!m) return 0;
        const char *a = n, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return m;
    }
}

// One region, walked and registered. Split out of the scan below when there
// came to be two: the walk is identical and only the bounds differ, and a
// second copy of it would have been a second place to forget the terminator.
static uint32_t scan_region(uintptr_t p, uintptr_t stop)
{
    uint32_t found = 0;

    // The image is contiguous, so it ends at the first word that is not a
    // module and the scan stops there rather than searching the whole region.
    //
    // Searching on was worse than slow. Loading an image smaller than the one
    // before it leaves the old tail in flash, and picotool writes only as many
    // bytes as the file has -- so the scan found the modules at the end of the
    // previous image as well, and the directory listed echo, lsmod, free and
    // both descriptors twice.
    while (p + sizeof(ubiqos_module_header_t) < stop) {
        ubiqos_module_header_t *m = (ubiqos_module_header_t*)p;

        // Unwritten flash reads as 0xFFFFFFFF, and make_flash_image.py writes a
        // terminator, so either way this is where the image ends.
        if (m->sync_code != UBIQOS_SYNC_CODE) break;
        if (!m->module_size || p + m->module_size > stop) {
            ubiqos_print("  implausible module size, stopping\n");
            break;
        }
        if (!verify_ubiqos_header(m)) {
            ubiqos_print("  bad header, stopping\n");
            break;
        }

        char name[UBIQOS_NAME_LEN];
        name_from_module(m, name);

        // Only the descriptors are registered. They are what the devices are
        // made of and the I/O manager wants them before anything opens
        // anything, so they cannot wait to be asked for. A program is left
        // where it lies and found by name when somebody runs it -- which is
        // what the sync word is for, and what keeps a directory of thirty-two
        // entries from bounding how many modules the system may have.
        // A plain data module is not a descriptor and waits to be asked for
        // like a program, rather than taking one of the directory's slots.
        if ((m->type_lang >> 8) == UBIQOS_TYPE_DATA
                && !((m->attr_rev >> 8) & UBIQOS_ATTR_PLAIN)) {
            if (ubiqos_moddir_add_resident(m, name)) {
                ubiqos_print("  descriptor ");
                ubiqos_print(name);
                ubiqos_print("\n");
            }
        }
        found++;

        // Skip past the whole module; the next may start right after, 4-byte
        // aligned.
        p += (m->module_size + 3u) & ~3u;
    }
    return found;
}

// Both regions, and the second is allowed to be empty: a board with no
// application in it is the ordinary case, and an unwritten region reads as
// 0xFFFFFFFF, which is not a sync word. So nothing is said about it unless
// something is there.
uint32_t ubiqos_flash_scan(void) {
    ubiqos_print("Scanning flash for resident modules from 0x");
    ubiqos_print_hex(UBIQOS_FLASH_MODULE_BASE);
    ubiqos_print("\n");

    uint32_t found = scan_region(UBIQOS_FLASH_MODULE_BASE, UBIQOS_FLASH_APP_BASE);

    if (!found) ubiqos_print("  none found\n");
    else {
        ubiqos_print("  ");
        ubiqos_print_u32(found);
        ubiqos_print(" modules in flash, looked up where they lie\n");
    }

    const uint32_t app = scan_region(UBIQOS_FLASH_APP_BASE, UBIQOS_FLASH_END);
    if (app) {
        ubiqos_print("  ");
        ubiqos_print_u32(app);
        ubiqos_print(" more from the application image at 0x");
        ubiqos_print_hex(UBIQOS_FLASH_APP_BASE);
        ubiqos_print("\n");
    }
    return found + app;
}
