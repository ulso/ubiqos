// A UbiqOS module from what the assembler made: the header, the code, the
// name, and the table of words the loader must add its address to.
//
// This is make_module.py's job for C, done here in a few lines because an
// assembled program has none of what makes that script long: no ELF, no
// sections, no thread-local data. The code starts right after the 56-byte
// header, so an offset in the code plus 56 is an offset in the module, and
// that is what the relocation table and the relocated words hold.
//
// Built on the host too, so the constants are written out here; on the board
// they are checked against the ABI header, which is where they live.
#include "modwrite.h"

#include <stdlib.h>
#include <string.h>

#define HEADER          56u
#define SYNC            0x0509000Bu
#define ABI_VERSION     10u
#define ARCH_ARM32      2u
#define TYPE_PROGRAM    1u
#define ATTR_REENTRANT  0x01u
#define ATTR_PRIVATE    0x04u
#define DEFAULT_STACK   4096u

#ifdef UBIQOS_MODULE
#include "../../common/ubiqos_abi.h"
_Static_assert(ABI_VERSION == UBIQOS_ABI_VERSION, "Blixt writes modules for another ABI");
_Static_assert(SYNC == UBIQOS_SYNC_CODE, "the sync word moved");
_Static_assert(HEADER == sizeof(ubiqos_module_header_t), "the header changed size");
_Static_assert(ARCH_ARM32 == UBIQOS_ARCH_ARM32, "the machine number moved");
_Static_assert(ATTR_PRIVATE == UBIQOS_ATTR_PRIVATE, "the private bit moved");
#endif

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t *mod_build(const char *name, const asm_result_t *r, uint32_t *size_out)
{
    uint32_t code = (r->size + 3) & ~3u;
    uint32_t nlen = (uint32_t)strlen(name) + 1;
    uint32_t name_room = (nlen + 3) & ~3u;
    uint32_t name_off = HEADER + code;
    uint32_t reloc_off = name_off + name_room;
    uint32_t size = reloc_off + 8 * r->nrelocs;
    uint8_t *m = calloc(1, size);
    if (!m) return 0;

    memcpy(m + HEADER, r->code, r->size);
    // Each relocated word holds an offset in the code; in the module it is an
    // offset from the header, as the loader expects.
    for (uint32_t i = 0; i < r->nrelocs; i++) {
        uint8_t *w = m + HEADER + r->relocs[i];
        uint32_t target = get32(w) + HEADER;
        put32(w, target);
        put32(m + reloc_off + 8 * i, HEADER + r->relocs[i]);   // kind 0, an absolute word
        put32(m + reloc_off + 8 * i + 4, target);
    }
    memcpy(m + name_off, name, nlen);

    // Private, so that every run gets a copy it may write into: a program's
    // data is in among its code. Re-entrant, because each copy is its own.
    uint32_t type_lang = (TYPE_PROGRAM << 8) | (ARCH_ARM32 << 4) | 1u;
    uint32_t attr_rev = ((ATTR_REENTRANT | ATTR_PRIVATE) << 8) | ABI_VERSION;
    uint32_t mem = r->stack ? (r->stack + 15) & ~15u : DEFAULT_STACK;
    uint32_t f[14] = {
        SYNC, size, name_off, (attr_rev << 16) | type_lang,
        HEADER + r->entry, mem,
        0, 0, 0,                 // no thread-local data
        0,                       // revision 0, and nothing reserved
        reloc_off, r->nrelocs,
        0,                       // no .bss
        0,
    };
    uint32_t sum = 0;
    for (int i = 0; i < 13; i++) sum += f[i];
    f[13] = ~sum;
    for (int i = 0; i < 14; i++) put32(m + 4 * i, f[i]);
    *size_out = size;
    return m;
}
