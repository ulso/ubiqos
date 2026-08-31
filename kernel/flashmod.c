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

// One step through the image, from *p, which is advanced past the module.
// Returns NULL at the end -- and the end is the first word that is not a
// module, not the end of the region.
//
// Searching on was worse than slow. Loading an image smaller than the one
// before it leaves the old tail in flash, and picotool writes only as many
// bytes as the file has -- so the scan found the modules at the end of the
// previous image as well, and the directory listed echo, lsmod, free and both
// descriptors twice.
static const myrtos_module_header_t *flash_step(uintptr_t *p) {
    if (*p + sizeof(myrtos_module_header_t) >= MYRTOS_FLASH_END) return 0;
    myrtos_module_header_t *m = (myrtos_module_header_t*)*p;

    // Unwritten flash reads as 0xFFFFFFFF, and make_flash_image.py writes a
    // terminator, so either way this is where the image ends.
    if (m->sync_code != MYRTOS_SYNC_CODE) return 0;
    if (!m->module_size || *p + m->module_size > MYRTOS_FLASH_END) return 0;
    if (!verify_myrtos_header(m)) return 0;

    *p += (m->module_size + 3u) & ~3u;   // the next may start right after
    return m;
}

// The image is the directory. There is no need to copy it into another one:
// OS-9 read modules straight out of ROM this way, and the whole point of the
// sync word is that a module can be found where it lies.
const myrtos_module_header_t *myrtos_flash_nth(uint32_t index, char *name_out) {
    uintptr_t p = MYRTOS_FLASH_MODULE_BASE;
    for (;;) {
        const myrtos_module_header_t *m = flash_step(&p);
        if (!m) return 0;
        if (index-- == 0) {
            if (name_out) name_from_module(m, name_out);
            return m;
        }
    }
}

// By the eleven-character directory name, padded, as the directory stores it.
const myrtos_module_header_t *myrtos_flash_lookup(const char *name) {
    char n[12];
    for (uint32_t i = 0; ; i++) {
        const myrtos_module_header_t *m = myrtos_flash_nth(i, n);
        if (!m) return 0;
        bool same = true;
        for (int j = 0; j < 11; j++) if (n[j] != name[j]) same = false;
        if (same) return m;
    }
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

        // Only the descriptors are registered. They are what the devices are
        // made of and the I/O manager wants them before anything opens
        // anything, so they cannot wait to be asked for. A program is left
        // where it lies and found by name when somebody runs it -- which is
        // what the sync word is for, and what keeps a directory of thirty-two
        // entries from bounding how many modules the system may have.
        if ((m->type_lang >> 8) == MYRTOS_TYPE_DATA) {
            if (myrtos_moddir_add_resident(m, name)) {
                myrtos_print("  descriptor ");
                myrtos_print(name);
                myrtos_print("\n");
            }
        }
        found++;

// Skip past the whole module; the next may start right after, 4-byte aligned.
        p += (m->module_size + 3u) & ~3u;
    }

    if (!found) myrtos_print("  none found\n");
    else {
        myrtos_print("  ");
        myrtos_print_u32(found);
        myrtos_print(" modules in flash, looked up where they lie\n");
    }
    return found;
}
