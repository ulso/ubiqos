#include <string.h>

#include "keystore.h"
#include "flashmod.h"
#include "tlsf.h"
#include "critical.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"
#include "hardware/regs/addressmap.h"

extern tlsf_pool_t ubiqos_mem_pool;
void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);

// --- what is in flash -------------------------------------------------------
//
// Two sectors, one of them the live copy. Which one is decided by the sequence
// number: the higher wins, and a write goes to the other one, so the older copy
// is whole until the new one is complete and checked. That is the whole of the
// power-cut story -- there is no moment when neither is readable.

#define KEYS_MAGIC   0x59454b55u     // "UKEY", little endian
#define KEYS_FORMAT  1u              // 2 will be the sealed one

typedef struct {
    char     name[UBIQOS_KEY_NAME_MAX];
    uint16_t len;                    // 0 for an empty slot
    uint16_t reserved;
    uint8_t  value[UBIQOS_KEY_VALUE_MAX];
} key_slot_t;

typedef struct {
    uint32_t magic;
    uint32_t format;
    uint32_t seq;                    // the higher of the two copies is the live one
    uint32_t crc;                    // over the slots that follow
    key_slot_t slot[UBIQOS_KEY_SLOTS];
} key_store_t;

_Static_assert(sizeof(key_store_t) <= FLASH_SECTOR_SIZE, "the store must fit a sector");

static const key_store_t *live;      // in flash, or NULL when there is none

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static uint32_t store_crc(const key_store_t *s)
{
    return crc32((const uint8_t *)s->slot, sizeof s->slot);
}

static const key_store_t *copy_at(uint32_t which)
{
    return (const key_store_t *)(uintptr_t)(UBIQOS_FLASH_KEYS_BASE + which * FLASH_SECTOR_SIZE);
}

static bool copy_is_good(const key_store_t *s)
{
    return s->magic == KEYS_MAGIC && s->format == KEYS_FORMAT && s->crc == store_crc(s);
}

void ubiqos_keys_init(void)
{
    const key_store_t *a = copy_at(0), *b = copy_at(1);
    const bool ga = copy_is_good(a), gb = copy_is_good(b);
    live = (ga && gb) ? (a->seq >= b->seq ? a : b) : (ga ? a : (gb ? b : 0));

    ubiqos_print("Keys: ");
    if (!live) {
        ubiqos_print("none stored\n");
        return;
    }
    ubiqos_print_u32(ubiqos_keys_count());
    ubiqos_print(" stored\n");
}

static bool name_eq(const char *a, const char *b)
{
    for (uint32_t i = 0; i < UBIQOS_KEY_NAME_MAX; i++) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return false;
}

static const key_slot_t *find(const char *name)
{
    if (!live || !name || !name[0]) return 0;
    for (uint32_t i = 0; i < UBIQOS_KEY_SLOTS; i++)
        if (live->slot[i].len && name_eq(live->slot[i].name, name)) return &live->slot[i];
    return 0;
}

uint32_t ubiqos_keys_count(void)
{
    if (!live) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < UBIQOS_KEY_SLOTS; i++) if (live->slot[i].len) n++;
    return n;
}

bool ubiqos_keys_nth(uint32_t index, char *name_out, uint32_t *len_out)
{
    if (!live) return false;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < UBIQOS_KEY_SLOTS; i++) {
        if (!live->slot[i].len) continue;
        if (seen++ != index) continue;
        for (uint32_t k = 0; k < UBIQOS_KEY_NAME_MAX; k++) name_out[k] = live->slot[i].name[k];
        if (len_out) *len_out = live->slot[i].len;
        return true;
    }
    return false;
}

// Enough of the value to recognise it by, and not enough to be it: a CRC of
// the bytes. Somebody who has the key in their password manager can compare
// the same four bytes; somebody who has only this cannot work backwards to a
// secret of any length.
bool ubiqos_keys_fingerprint(const char *name, uint32_t *out)
{
    const key_slot_t *s = find(name);
    if (!s) return false;
    if (out) *out = crc32(s->value, s->len);
    return true;
}

const uint8_t *ubiqos_keys_value(const char *name, uint32_t *len_out)
{
    const key_slot_t *s = find(name);
    if (!s) return 0;
    if (len_out) *len_out = s->len;
    return s->value;
}

// --- writing ----------------------------------------------------------------
//
// An erase takes tens of milliseconds, and for all of it the flash cannot be
// read -- so no instruction may be fetched from it. The kernel is linked
// copy_to_ram and runs from SRAM, which is most of the answer; the rest is
// core 1, which runs kernel code too but must not be inside the writing
// function's caller when it is asked to wait. It parks in a loop of its own,
// in RAM, and core 0 waits for it to say so.

static volatile bool park_wanted;
static volatile bool parked;

void __not_in_flash_func(ubiqos_keys_park_here)(void)
{
    if (!park_wanted) return;
    parked = true;
    while (park_wanted) tight_loop_contents();
    parked = false;
}

// Set before core 1 is launched. A board whose second core never started has
// nothing to park, and that is not a reason to refuse to store a key.
volatile bool ubiqos_keys_core1_running;

static bool park_core1(void)
{
    if (!ubiqos_keys_core1_running) return true;
    park_wanted = true;
    for (uint32_t spins = 0; spins < 5000000u; spins++)
        if (parked) return true;
    park_wanted = false;
    return false;
}

static void release_core1(void)
{
    park_wanted = false;
    while (parked) tight_loop_contents();
}

static void __not_in_flash_func(do_write)(uint32_t offset, const uint8_t *data)
{
    const uint32_t st = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, data, FLASH_SECTOR_SIZE);
    restore_interrupts(st);
}

// The new store is built in SRAM -- never in PSRAM, which shares the QMI with
// the flash and is no more readable than the flash is during an erase.
static int32_t commit(const key_store_t *from, const char *name,
                      const uint8_t *value, uint32_t len)
{
    key_store_t *img = ubiqos_tlsf_malloc(ubiqos_mem_pool, sizeof *img);
    if (!img) return -1;

    if (from) memcpy(img, from, sizeof *img);
    else      memset(img, 0, sizeof *img);
    img->magic  = KEYS_MAGIC;
    img->format = KEYS_FORMAT;
    img->seq    = from ? from->seq + 1 : 1;

    // The named slot, or the first free one. Setting a name that is there
    // replaces it in place, so a key keeps its slot across changes.
    key_slot_t *slot = 0, *spare = 0;
    for (uint32_t i = 0; i < UBIQOS_KEY_SLOTS; i++) {
        if (img->slot[i].len && name_eq(img->slot[i].name, name)) { slot = &img->slot[i]; break; }
        if (!img->slot[i].len && !spare) spare = &img->slot[i];
    }
    if (!slot) slot = spare;
    if (!slot) { ubiqos_tlsf_free(ubiqos_mem_pool, img); return -2; }   // full

    memset(slot, 0, sizeof *slot);
    if (len) {                                  // len 0 means remove
        uint32_t k = 0;
        while (k < UBIQOS_KEY_NAME_MAX - 1 && name[k]) { slot->name[k] = name[k]; k++; }
        slot->name[k] = 0;
        memcpy(slot->value, value, len);
        slot->len = (uint16_t)len;
    }
    img->crc = store_crc(img);

    // Into the copy that is not live, so that the live one is whole until this
    // one is complete.
    const uint32_t which = (live == copy_at(0)) ? 1u : 0u;
    const uint32_t offset = (UBIQOS_FLASH_KEYS_BASE - XIP_BASE) + which * FLASH_SECTOR_SIZE;

    int32_t rc = 0;
    if (!park_core1()) rc = -3;
    else {
        do_write(offset, (const uint8_t *)img);
        release_core1();
        const key_store_t *fresh = copy_at(which);
        if (copy_is_good(fresh) && fresh->seq == img->seq) live = fresh;
        else rc = -4;
    }
    memset(img, 0, sizeof *img);                // the value was in this buffer
    ubiqos_tlsf_free(ubiqos_mem_pool, img);
    return rc;
}

int32_t ubiqos_keys_set(const char *name, const uint8_t *value, uint32_t len)
{
    if (!name || !name[0] || !len || len > UBIQOS_KEY_VALUE_MAX) return -1;
    uint32_t n = 0;
    while (name[n]) n++;
    if (n >= UBIQOS_KEY_NAME_MAX) return -1;
    return commit(live, name, value, len);
}

int32_t ubiqos_keys_remove(const char *name)
{
    if (!find(name)) return -1;
    return commit(live, name, 0, 0);
}
