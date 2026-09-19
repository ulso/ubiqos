#include "moddir.h"
#include "flashmod.h"
#include "tlsf.h"
#include "hardware/sync.h"

extern tlsf_pool_t ubiqos_mem_pool;
void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
bool verify_ubiqos_header(ubiqos_module_header_t *header);

static ubiqos_module_entry_t modules[UBIQOS_MAX_MODULES];
static uint32_t module_count;

static bool name_eq(const char *a, const char *b) {
    for (int i = 0; i < 11; i++) if (a[i] != b[i]) return false;
    return true;
}

// Eleven bytes, stopping at a NUL and padding with spaces.
//
// It used to copy eleven bytes flat, which is right for a name that comes out
// of a module header -- a fixed field, padded already -- and wrong for a C
// string. When modules began being loaded by name from the card, the name came
// from the shell's line buffer, and `dhello from the card` registered a module
// called "dhello\0from the ca": the copy ran straight past the terminator and
// took the arguments with it. name_eq compares all eleven, so nothing ever
// matched it again -- the module loaded, sat in the directory, and could not be
// found or run.
//
// Padding with spaces and not with zeroes, which was the second half of the
// Terminated, not padded. It used to be space-filled to eleven -- eight for the
// name and three for a FAT extension -- and matching then had to decide that a
// shorter name fitted by checking the remainder was blank. That equality had to
// be remembered at every comparison, and was forgotten at least twice: a
// zero-filled field printed correctly in lsmod and matched nothing at all.
//
// A name is now whatever it is, up to UBIQOS_NAME_LEN - 1, and two names are
// equal when they are the same string.
static void name_copy(char *dst, const char *src) {
    int i = 0;
    while (i < UBIQOS_NAME_LEN - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void ubiqos_moddir_init(void) {
    module_count = 0;
}

// A name may exist once. Which copy wins is the revision, as in OS-9: the
// highest is kept and the rest refused. That is how a system was patched there
// -- a newer module in the spare EPROM socket beat the one soldered down, with
// nothing else in the machine changed.
//
// Until this existed, two modules of the same name both got entries with their
// own link counts, and which one ran depended on the order they were found in.
static ubiqos_module_entry_t *entry_named(const char *name) {
    for (uint32_t i = 0; i < module_count; i++) {
        const char *a = modules[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return &modules[i];
    }
    return 0;
}

// True when the newcomer should be registered. An entry that is beaten is
// released here if nothing is using it; if it is in use it stays, because a
// process is running that code right now.
static bool supersedes(const ubiqos_module_header_t *fresh, const char *name,
                       bool from_card) {
    ubiqos_module_entry_t *old = entry_named(name);
    if (!old) {
        // Nothing in the directory -- but flash may still hold one, adopted
        // only while something uses it. A copy from the card must beat THAT,
        // or an older module on the card would quietly replace a newer system.
        // Only a copy from the card: the boot scan registers the flash modules
        // themselves, and one compared with itself would refuse itself.
        if (!from_card) return true;
        const ubiqos_module_header_t *f = ubiqos_flash_lookup(name);
        if (!f || fresh->revision > f->revision) return true;
        ubiqos_print("  ");
        ubiqos_print(name);
        ubiqos_print(": revision ");
        ubiqos_print_u32(fresh->revision);
        ubiqos_print(" on the card does not beat ");
        ubiqos_print_u32(f->revision);
        ubiqos_print(" in flash, ignored\n");
        return false;
    }

    // The same revision the directory already holds from the card: asked for
    // again rather than offered anew, and not worth a line on the console.
    if (old->owned && fresh->revision == old->header->revision) return false;

    if (fresh->revision <= old->header->revision) {
        ubiqos_print("  ");
        ubiqos_print(name);
        ubiqos_print(": revision ");
        ubiqos_print_u32(fresh->revision);
        ubiqos_print(" does not beat ");
        ubiqos_print_u32(old->header->revision);
        ubiqos_print(", ignored\n");
        return false;
    }
    if (old->links) {
        ubiqos_print("  ");
        ubiqos_print(name);
        ubiqos_print(": newer revision found but the old one is in use\n");
        return false;
    }

    ubiqos_print("  ");
    ubiqos_print(name);
    ubiqos_print(": revision ");
    ubiqos_print_u32(fresh->revision);
    ubiqos_print(" replaces ");
    ubiqos_print_u32(old->header->revision);
    ubiqos_print("\n");

    extern tlsf_pool_t ubiqos_pool_of_address(void *p);
    if (old->owned) ubiqos_tlsf_free(ubiqos_pool_of_address(old->owned), old->owned);
    // Closing the gap overwrites an entry a reader may be standing on, so the
    // move and the count go together or a walk sees half of each.
    uint32_t st = save_and_disable_interrupts();
    *old = modules[--module_count];       // close the gap
    restore_interrupts(st);
    return true;
}

// A slot past the end, which no reader can see: every walk of the directory
// stops at module_count. The caller fills it and then calls commit_entry, and
// only that makes it exist.
//
// The order matters now in a way it did not when every module was registered
// from main before the scheduler started. The filesystem server registers what
// it finds on the card from a kernel thread, with interrupts on, so a system
// call can land in the middle of it -- and a directory that counted the slot
// before it was filled would hand that caller an entry holding whatever the
// last module to occupy it left behind. A stale header pointer is a jump into
// nothing, arriving whenever the timer happens to fall between two lines.
static ubiqos_module_entry_t *alloc_entry(void) {
    if (module_count >= UBIQOS_MAX_MODULES) {
        ubiqos_print("  module directory full\n");
        return 0;
    }
    return &modules[module_count];
}

// Publishing is the one store that must not be reordered before the fills, and
// the critical section is what stops both the compiler and the timer.
static void commit_entry(void) {
    uint32_t st = save_and_disable_interrupts();
    module_count++;
    restore_interrupts(st);
}

bool ubiqos_moddir_add_resident(const ubiqos_module_header_t *header, const char *name) {
    if (!verify_ubiqos_header((ubiqos_module_header_t*)header)) return false;
    if (!supersedes(header, name, false)) return false;
    ubiqos_module_entry_t *e = alloc_entry();
    if (!e) return false;
    e->header = header;
    e->links = 0;
    e->owned = 0;
    e->transient = false;
    name_copy(e->name, name);
    commit_entry();
    return true;
}

// Take an image the caller has already allocated, rather than copying one.
//
// This used to be ubiqos_moddir_add_copy, which took a pointer to a module
// sitting in a staging buffer and made its own copy in the right pool. That
// suited a scan that read every module through one fixed 32 kB buffer. Now that
// a module is read only when something runs it, the reader knows its size
// before it reads and can put it straight where it belongs -- so what arrives
// here is the module, not a view of it, and the directory adopts the
// allocation. `owned` says so, and is what frees it again.
//
// On refusal the caller still owns the image and frees it; saying so here
// rather than freeing it means one owner at a time and no double free.
bool ubiqos_moddir_add_image(uint8_t *image, uint32_t len, const char *name) {
    (void)len;
    if (!verify_ubiqos_header((ubiqos_module_header_t*)image)) return false;
    if (!supersedes((const ubiqos_module_header_t*)image, name, true)) return false;

    ubiqos_module_entry_t *e = alloc_entry();
    if (!e) return false;
    e->header = (const ubiqos_module_header_t*)image;
    e->links = 0;
    e->owned = image;
    e->transient = false;      // `owned` is what decides its fate, not this
    name_copy(e->name, name);
    commit_entry();
    return true;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Case-insensitive, and equal means the same string. What a user types is
// matched this way because a card gives names in whatever case it stored them.
static bool name_eq_ci(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return !*a && !*b;
}

static ubiqos_module_entry_t *adopt_from_flash(const char *name);

const char *ubiqos_moddir_match(const char *user_name) {
    for (uint32_t i = 0; i < module_count; i++) {
        if (name_eq_ci(modules[i].name, user_name)) return modules[i].name;
    }

    // Not registered, so try the image. Adopting it here rather than returning
    // a pointer into a scratch buffer keeps the promise this function has
    // always made: the name it returns stays put, and link can be called with
    // it. Every caller does exactly that.
    char name[UBIQOS_NAME_LEN];
    for (uint32_t i = 0; ubiqos_flash_nth(i, name); i++) {
        if (!name_eq_ci(name, user_name)) continue;
        ubiqos_module_entry_t *e = adopt_from_flash(name);
        return e ? e->name : 0;
    }
    return 0;
}

// Give a flash module an entry, for as long as something is running it. The
// entry is the bookkeeping -- a link count, and somewhere for a newer revision
// off the card to be compared against -- not a copy of anything: the header
// still points into flash and the code still runs where it lies.
static ubiqos_module_entry_t *adopt_from_flash(const char *name) {
    const ubiqos_module_header_t *m = ubiqos_flash_lookup(name);
    if (!m) return 0;
    ubiqos_module_entry_t *e = alloc_entry();
    if (!e) return 0;
    e->header = m;
    e->links = 0;
    e->owned = 0;
    e->transient = true;
    name_copy(e->name, name);
    commit_entry();
    return e;
}

const ubiqos_module_header_t *ubiqos_moddir_link(const char *name) {
    for (uint32_t i = 0; i < module_count; i++) {
        if (!name_eq(modules[i].name, name)) continue;
        uint32_t st = save_and_disable_interrupts();
        modules[i].links++;
        restore_interrupts(st);
        return modules[i].header;
    }
    ubiqos_module_entry_t *e = adopt_from_flash(name);
    if (!e) return 0;
    e->links++;
    return e->header;
}

void ubiqos_moddir_unlink(const ubiqos_module_header_t *header) {
    for (uint32_t i = 0; i < module_count; i++) {
        if (modules[i].header != header) continue;
        uint32_t st = save_and_disable_interrupts();
        if (modules[i].links) modules[i].links--;
        // At zero links the entry goes, and if it owns an image that goes too.
        //
        // This used to keep the copy -- OS-9 kept a loaded module until the
        // memory was needed, so the next start of the same utility was
        // immediate. Ulf's call on 2 Sep 2026 was to drop it: nothing should
        // sit in RAM because it was run once. Since nothing is read off the
        // card until it is run either, a module now occupies memory for exactly
        // as long as something is using it and not one moment longer.
        //
        // An adopted flash entry has no image to free -- the module is in flash
        // either way and the entry existed only to count the links -- but it
        // goes for the same reason: keeping them would fill the directory with
        // everything ever run.
        if (!modules[i].links) {
            if (modules[i].owned) {
                extern tlsf_pool_t ubiqos_pool_of_address(void *p);
                ubiqos_tlsf_free(ubiqos_pool_of_address(modules[i].owned),
                                 modules[i].owned);
            }
            modules[i] = modules[--module_count];
        }
        restore_interrupts(st);
        return;
    }
}

// How many modules there are, which is not how many have entries. What is not
// registered is in flash, and flash is a directory -- so the count is both,
// with anything registered from the card not counted twice.
uint32_t ubiqos_moddir_count(void) {
    uint32_t n = module_count;
    char name[UBIQOS_NAME_LEN];
    for (uint32_t i = 0; ubiqos_flash_nth(i, name); i++)
        if (!entry_named(name)) n++;
    return n;
}

const ubiqos_module_entry_t *ubiqos_moddir_entry(uint32_t index) {
    if (index < module_count) return &modules[index];

    // Past the registered ones, the image itself. Made up on the spot, because
    // there is nothing to point at: the entry is what a registered module has,
    // and these have not needed one. The caller reads it and is done with it
    // before asking for the next, which is why one of them is enough.
    static ubiqos_module_entry_t made_up;
    uint32_t want = index - module_count;
    char name[UBIQOS_NAME_LEN];
    for (uint32_t i = 0; ; i++) {
        const ubiqos_module_header_t *m = ubiqos_flash_nth(i, name);
        if (!m) return 0;
        if (entry_named(name)) continue;
        if (want--) continue;
        made_up.header = m;
        made_up.links = 0;
        made_up.owned = 0;
        made_up.transient = false;
        name_copy(made_up.name, name);
        return &made_up;
    }
}

// --- LIBRARY MODULES ------------------------------------------------------
// Link a module the kernel means to call rather than run.
//
// The same work as starting a process, minus the process: the module is found,
// relocated into the pool it belongs in -- which for anything not real-time is
// PSRAM, so its code is not in SRAM at all -- and then simply not started.
// exec_offset points at a table instead of at an entry point.
//
// Everything is checked before a single pointer is trusted. A module of the
// wrong type would be entered as though its first instruction were a pointer;
// a table from an older build would be read with today's field order. Both are
// the kind of mistake that shows up as a jump into the middle of something.
// Find a module of the wanted type and give back the address its table sits at.
// Shared by libraries and drivers, which differ only in the type byte and in
// what the caller does with the pointer afterwards.
static const void *link_table(const char *name, uint32_t want_type,
                              const char *what, void **owned_out)
{
    if (owned_out) *owned_out = 0;

    const ubiqos_module_header_t *h = ubiqos_moddir_link(name);
    if (!h) return 0;

    if ((h->type_lang >> 8) != want_type) {
        ubiqos_print(what);
        ubiqos_print(": ");
        ubiqos_print(name);
        ubiqos_print(" is not one\n");
        ubiqos_moddir_unlink(h);
        return 0;
    }

    // Copied for the same reasons a program is: writable data, addresses to
    // fix, or a .bss to zero. A library with none of them runs where it lies.
    const uint8_t *base = (const uint8_t*)h;
    bool needs_copy = ((h->attr_rev >> 8) & UBIQOS_ATTR_PRIVATE) != 0;
    if (needs_copy) {
        extern uint8_t *ubiqos_module_relocated_copy(const ubiqos_module_header_t *m,
                                                     void **owned_out);
        void *owned = 0;
        base = ubiqos_module_relocated_copy(h, &owned);
        if (!base) {
            ubiqos_print("lib: no room to relocate ");
            ubiqos_print(name);
            ubiqos_print("\n");
            ubiqos_moddir_unlink(h);
            return 0;
        }
        if (owned_out) *owned_out = owned;
    }

    return base + h->exec_offset;
}

const ubiqos_lib_table_t *ubiqos_lib_link(const char *name, void **owned_out)
{
    const ubiqos_lib_table_t *t =
        (const ubiqos_lib_table_t*)link_table(name, UBIQOS_TYPE_LIBRARY, "lib", owned_out);
    if (!t) return 0;
    if (t->abi != UBIQOS_LIB_ABI) {
        ubiqos_print("lib: ");
        ubiqos_print(name);
        ubiqos_print(" speaks another interface\n");
        return 0;
    }
    return t;
}

// The same for a device driver. The I/O manager asks for one by the name a
// descriptor gave, after looking through the drivers built into the kernel and
// not finding it -- so a descriptor for a device nobody compiled in still works,
// which is the whole point of the descriptors being files.
const ubiqos_driver_module_t *ubiqos_driver_link(const char *name, void **owned_out)
{
    const ubiqos_driver_module_t *d =
        (const ubiqos_driver_module_t*)link_table(name, UBIQOS_TYPE_DRIVER, "drv", owned_out);
    if (!d) return 0;
    if (d->abi != UBIQOS_DRIVER_ABI) {
        ubiqos_print("drv: ");
        ubiqos_print(name);
        ubiqos_print(" speaks another interface\n");
        return 0;
    }
    return d;
}
