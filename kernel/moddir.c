#include "moddir.h"
#include "tlsf.h"

extern tlsf_pool_t myrtos_mem_pool;
void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);
bool verify_myrtos_header(myrtos_module_header_t *header);

static myrtos_module_entry_t modules[MYRTOS_MAX_MODULES];
static uint32_t module_count;

static bool name_eq(const char *a, const char *b) {
    for (int i = 0; i < 11; i++) if (a[i] != b[i]) return false;
    return true;
}

static void name_copy(char *dst, const char *src) {
    for (int i = 0; i < 11; i++) dst[i] = src[i];
    dst[11] = 0;
}

void myrtos_moddir_init(void) {
    module_count = 0;
}

// A name may exist once. Which copy wins is the revision, as in OS-9: the
// highest is kept and the rest refused. That is how a system was patched there
// -- a newer module in the spare EPROM socket beat the one soldered down, with
// nothing else in the machine changed.
//
// Until this existed, two modules of the same name both got entries with their
// own link counts, and which one ran depended on the order they were found in.
static myrtos_module_entry_t *entry_named(const char *name) {
    for (uint32_t i = 0; i < module_count; i++) {
        bool same = true;
        for (int j = 0; j < 11; j++) if (modules[i].name[j] != name[j]) same = false;
        if (same) return &modules[i];
    }
    return 0;
}

// True when the newcomer should be registered. An entry that is beaten is
// released here if nothing is using it; if it is in use it stays, because a
// process is running that code right now.
static bool supersedes(const myrtos_module_header_t *fresh, const char *name) {
    myrtos_module_entry_t *old = entry_named(name);
    if (!old) return true;

    if (fresh->revision <= old->header->revision) {
        myrtos_print("  ");
        myrtos_print(name);
        myrtos_print(": revision ");
        myrtos_print_u32(fresh->revision);
        myrtos_print(" does not beat ");
        myrtos_print_u32(old->header->revision);
        myrtos_print(", ignored\n");
        return false;
    }
    if (old->links) {
        myrtos_print("  ");
        myrtos_print(name);
        myrtos_print(": newer revision found but the old one is in use\n");
        return false;
    }

    myrtos_print("  ");
    myrtos_print(name);
    myrtos_print(": revision ");
    myrtos_print_u32(fresh->revision);
    myrtos_print(" replaces ");
    myrtos_print_u32(old->header->revision);
    myrtos_print("\n");

    if (old->owned) myrtos_tlsf_free(myrtos_mem_pool, old->owned);
    *old = modules[--module_count];       // close the gap
    return true;
}

static myrtos_module_entry_t *alloc_entry(void) {
    if (module_count >= MYRTOS_MAX_MODULES) {
        myrtos_print("  module directory full\n");
        return 0;
    }
    return &modules[module_count++];
}

bool myrtos_moddir_add_resident(const myrtos_module_header_t *header, const char *name) {
    if (!verify_myrtos_header((myrtos_module_header_t*)header)) return false;
    if (!supersedes(header, name)) return false;
    myrtos_module_entry_t *e = alloc_entry();
    if (!e) return false;
    e->header = header;
    e->links = 0;
    e->owned = 0;
    name_copy(e->name, name);
    return true;
}

bool myrtos_moddir_add_copy(const uint8_t *src, uint32_t len, const char *name) {
    if (!verify_myrtos_header((myrtos_module_header_t*)src)) return false;
    if (!supersedes((const myrtos_module_header_t*)src, name)) return false;

    void *space = myrtos_tlsf_malloc(myrtos_mem_pool, len);
    if (!space) { myrtos_print("  no heap for module\n"); return false; }
    uint8_t *d = (uint8_t*)space;
    for (uint32_t i = 0; i < len; i++) d[i] = src[i];

    myrtos_module_entry_t *e = alloc_entry();
    if (!e) { myrtos_tlsf_free(myrtos_mem_pool, space); return false; }
    e->header = (const myrtos_module_header_t*)space;
    e->links = 0;
    e->owned = space;
    name_copy(e->name, name);
    return true;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

const char *myrtos_moddir_match(const char *user_name) {
    for (uint32_t i = 0; i < module_count; i++) {
        const char *stored = modules[i].name;
        int k = 0;
        while (k < 8 && user_name[k] && lower(stored[k]) == lower(user_name[k])) k++;
        // A match when the user's name has run out and the rest of the module
        // name is padding.
        if (user_name[k]) continue;
        bool rest_blank = true;
        for (int j = k; j < 8; j++) if (stored[j] != ' ') rest_blank = false;
        if (rest_blank) return modules[i].name;
    }
    return 0;
}

const myrtos_module_header_t *myrtos_moddir_link(const char *name) {
    for (uint32_t i = 0; i < module_count; i++) {
        if (!name_eq(modules[i].name, name)) continue;
        modules[i].links++;
        return modules[i].header;
    }
    return 0;
}

void myrtos_moddir_unlink(const myrtos_module_header_t *header) {
    for (uint32_t i = 0; i < module_count; i++) {
        if (modules[i].header != header) continue;
        if (modules[i].links) modules[i].links--;
// The copy stays even at zero links. OS-9 did the same until the memory was
// needed: the next start of the same utility is then immediate.
        return;
    }
}

uint32_t myrtos_moddir_count(void) { return module_count; }

const myrtos_module_entry_t *myrtos_moddir_entry(uint32_t index) {
    return index < module_count ? &modules[index] : 0;
}
