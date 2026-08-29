#include "flashmod.h"
#include "moddir.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);
bool verify_myrtos_header(myrtos_module_header_t *header);

// The name lives in the module, but moddir wants eleven characters in 8.3
// form. A module in flash has no filename, so the name is derived from the
// module's own string.
static void name_from_module(const myrtos_module_header_t *m, char *out) {
    const char *src = (const char*)m + m->name_offset;
    int i = 0;
    for (; i < 8 && src[i]; i++) out[i] = src[i];
    for (; i < 8; i++) out[i] = ' ';
    out[8] = 'M'; out[9] = 'O'; out[10] = 'D'; out[11] = 0;
}

uint32_t myrtos_flash_scan(void) {
    uint32_t found = 0;
    uintptr_t p = MYRTOS_FLASH_MODULE_BASE;

    myrtos_print("Scanning flash for resident modules from 0x");
    myrtos_print_hex(MYRTOS_FLASH_MODULE_BASE);
    myrtos_print("\n");

    // The image is contiguous, so it ends at the first word that is not a
    // module and the scan stops there rather than searching the whole region.
    //
    // Searching on was worse than slow. Loading an image smaller than the one
    // before it leaves the old tail in flash, and picotool writes only as many
    // bytes as the file has -- so the scan found the modules at the end of the
    // previous image as well, and the directory listed echo, lsmod, free and
    // both descriptors twice.
    while (p + sizeof(myrtos_module_header_t) < MYRTOS_FLASH_END) {
        myrtos_module_header_t *m = (myrtos_module_header_t*)p;

        // Unwritten flash reads as 0xFFFFFFFF, and make_flash_image.py writes a
        // terminator, so either way this is where the image ends.
        if (m->sync_code != MYRTOS_SYNC_CODE) break;
        if (!m->module_size || p + m->module_size > MYRTOS_FLASH_END) {
            myrtos_print("  implausible module size, stopping\n");
            break;
        }
        if (!verify_myrtos_header(m)) {
            myrtos_print("  bad header, stopping\n");
            break;
        }

        char name[12];
        name_from_module(m, name);
        if (myrtos_moddir_add_resident(m, name)) {
            myrtos_print("  resident ");
            myrtos_print(name);
            myrtos_print(" at 0x");
            myrtos_print_hex((uint32_t)p);
            myrtos_print(", ");
            myrtos_print_u32(m->module_size);
            myrtos_print(" bytes\n");
            found++;
        }

// Skip past the whole module; the next may start right after, 4-byte aligned.
        p += (m->module_size + 3u) & ~3u;
    }

    if (!found) myrtos_print("  none found\n");
    return found;
}
