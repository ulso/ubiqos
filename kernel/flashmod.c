#include "flashmod.h"
#include "moddir.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
void myrtos_print_hex(uint32_t v);
bool verify_myrtos_header(myrtos_module_header_t *header);

// Namnet ligger i modulen, men moddir vill ha elva tecken i 8.3-form. En
// modul i flash har inget filnamn, så namnet härleds ur modulens egen sträng.
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

    while (p + sizeof(myrtos_module_header_t) < MYRTOS_FLASH_END) {
        myrtos_module_header_t *m = (myrtos_module_header_t*)p;

        // Oskriven flash läser 0xFFFFFFFF, så synkordet sållar bort tomrum
        // billigt innan checksumman räknas.
        if (m->sync_code != MYRTOS_SYNC_CODE) {
            p += 4;
            continue;
        }
        // Storleken måste vara rimlig innan den används till något.
        if (!m->module_size || p + m->module_size > MYRTOS_FLASH_END) {
            p += 4;
            continue;
        }
        if (!verify_myrtos_header(m)) {
            p += 4;
            continue;
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

        // Hoppa förbi hela modulen; nästa kan börja direkt efter, fyrbytejusterat.
        p += (m->module_size + 3u) & ~3u;
    }

    if (!found) myrtos_print("  none found\n");
    return found;
}
