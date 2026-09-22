#include <string.h>

#include "keystore.h"
#include "crypto.h"
#include "flashmod.h"
#include "tlsf.h"
#include "critical.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"
#include "hardware/regs/addressmap.h"
#include "pico/unique_id.h"

extern tlsf_pool_t ubiqos_mem_pool;
void ubiqos_random_bytes(uint8_t *out, uint32_t len);   // kernel/syscalls.c, the TRNG
void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);

// --- what is in flash -------------------------------------------------------
//
// Two sectors, one of them the live copy. Which one is decided by the sequence
// number: the higher wins, and a write goes to the other one, so the older copy
// is whole until the new one is complete and checked. That is the whole of the
// power-cut story -- there is no moment when neither is readable.

#define KEYS_MAGIC   0x59454b55u     // "UKEY", little endian
#define KEYS_FORMAT  2u              // 1 was the same records in the clear
// PBKDF2 iterations: what about two seconds of this chip buys, measured with
// the hardware SHA-256 (roughly 12500 iterations a second). It is stored with
// each copy, so raising it later leaves older stores openable.
//
// Two seconds here is nothing like two seconds for somebody who has taken the
// flash away: a desktop does this hundreds of times faster and a graphics card
// faster still. The iterations buy a factor, not safety. What makes a store
// hard to open is the length of the passphrase.
#define KEYS_ROUNDS  25000u

typedef struct {
    char     name[UBIQOS_KEY_NAME_MAX];
    uint16_t len;                    // 0 for an empty slot
    uint16_t reserved;
    uint8_t  value[UBIQOS_KEY_VALUE_MAX];
} key_slot_t;

typedef struct {
    key_slot_t slot[UBIQOS_KEY_SLOTS];
} key_plain_t;

// What is in flash: the records sealed, and what is needed to open them again
// -- everything except the passphrase.
typedef struct {
    uint32_t magic;
    uint32_t format;
    uint32_t seq;                    // the higher of the two copies is the live one
    uint32_t rounds;                 // PBKDF2 iterations this copy was sealed with
    uint8_t  salt[16];
    uint8_t  nonce[12];
    uint8_t  tag[16];
    uint8_t  sealed[sizeof(key_plain_t)];
} key_store_t;

_Static_assert(sizeof(key_store_t) <= FLASH_SECTOR_SIZE, "the store must fit a sector");

static const key_store_t *live;      // in flash, or NULL when there is none

// Unlocked means these exist: the derived key and the records in the clear,
// both in SRAM, both gone when the power goes.
static uint8_t     unlocked_key[32];
static key_plain_t *plain;

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static const key_store_t *copy_at(uint32_t which)
{
    return (const key_store_t *)(uintptr_t)(UBIQOS_FLASH_KEYS_BASE + which * FLASH_SECTOR_SIZE);
}

static bool copy_is_good(const key_store_t *s)
{
    return s->magic == KEYS_MAGIC && s->format == KEYS_FORMAT && s->rounds >= 1000u;
}

void ubiqos_keys_init(void)
{
    const key_store_t *a = copy_at(0), *b = copy_at(1);
    const bool ga = copy_is_good(a), gb = copy_is_good(b);
    live = (ga && gb) ? (a->seq >= b->seq ? a : b) : (ga ? a : (gb ? b : 0));
    ubiqos_print(live ? "Keys: a sealed store; 'key unlock' opens it\n"
                      : "Keys: none stored\n");
}

uint32_t ubiqos_keys_state(void)
{
    if (plain) return UBIQOS_KEYS_OPEN;
    return live ? UBIQOS_KEYS_LOCKED : UBIQOS_KEYS_EMPTY;
}

// The passphrase is not the whole of it: the chip's own id goes into the salt,
// so the same passphrase on two boards gives two different keys and a stolen
// store cannot be attacked alongside anybody else's. The id is a serial
// number, not a secret -- anyone holding the board can read it -- which is
// exactly why it is salt and not key.
static void derive(const key_store_t *st, const uint8_t *pass, uint32_t plen, uint8_t out[32])
{
    uint8_t salt[16 + PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
    memcpy(salt, st->salt, 16);
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    memcpy(salt + 16, id.id, PICO_UNIQUE_BOARD_ID_SIZE_BYTES);
    ubiqos_pbkdf2_sha256(pass, plen, salt, sizeof salt, st->rounds, out);
    memset(salt, 0, sizeof salt);
    memset(&id, 0, sizeof id);
}

void ubiqos_keys_lock(void)
{
    if (plain) {
        memset(plain, 0, sizeof *plain);
        ubiqos_tlsf_free(ubiqos_mem_pool, plain);
        plain = 0;
    }
    memset(unlocked_key, 0, sizeof unlocked_key);
}

static bool name_eq(const char *a, const char *b)
{
    for (uint32_t i = 0; i < UBIQOS_KEY_NAME_MAX; i++) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return false;
}

// Everything below needs the store open: the names are sealed with the values.
static const key_slot_t *find(const char *name)
{
    if (!plain || !name || !name[0]) return 0;
    for (uint32_t i = 0; i < UBIQOS_KEY_SLOTS; i++)
        if (plain->slot[i].len && name_eq(plain->slot[i].name, name)) return &plain->slot[i];
    return 0;
}

uint32_t ubiqos_keys_count(void)
{
    if (!plain) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < UBIQOS_KEY_SLOTS; i++) if (plain->slot[i].len) n++;
    return n;
}

bool ubiqos_keys_nth(uint32_t index, char *name_out, uint32_t *len_out)
{
    if (!plain) return false;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < UBIQOS_KEY_SLOTS; i++) {
        if (!plain->slot[i].len) continue;
        if (seen++ != index) continue;
        for (uint32_t k = 0; k < UBIQOS_KEY_NAME_MAX; k++) name_out[k] = plain->slot[i].name[k];
        if (len_out) *len_out = plain->slot[i].len;
        return true;
    }
    return false;
}

// Enough of the value to recognise it by, and not enough to be it: a CRC of
// the bytes. Whoever has the key in their password manager can work out the
// same four bytes; whoever has only these cannot work back to a secret.
bool ubiqos_keys_fingerprint(const char *name, uint32_t *out)
{
    const key_slot_t *s = find(name);
    if (!s) return false;
    if (out) *out = crc32(s->value, s->len);
    return true;
}

// Is this the value? Answered here, because answering it anywhere else means
// handing over the value to be compared -- which is the one thing this store
// does not do. The comparison looks at every byte whatever the first one says,
// so that the time it takes cannot be counted into a guess.
bool ubiqos_keys_match(const char *name, const uint8_t *value, uint32_t len)
{
    const key_slot_t *s = find(name);
    if (!s) return false;
    uint8_t diff = (uint8_t)((s->len ^ len) | ((s->len ^ len) >> 8));
    const uint32_t n = s->len < len ? s->len : len;
    for (uint32_t i = 0; i < n; i++) diff |= (uint8_t)(s->value[i] ^ value[i]);
    return s->len == len && !diff;
}

// A key OF this board, for a purpose named by the caller -- not a key IN the
// store. It is HMAC over the store's own key, which never leaves the kernel,
// so the answer is stable for as long as the passphrase is, unguessable to
// anybody without it, and different for every label and every board.
//
// This exists because a program can need a secret the kernel cannot use on its
// behalf. sshd has to sign with its host key, and signing is elliptic-curve
// arithmetic that does not fit in this kernel -- so it derives one here rather
// than storing one it would then have to read back. Nothing stored is
// revealed, and the promise that a value never comes out is untouched.
//
// Change the passphrase and every derived key changes with it. For a host key
// that means clients will notice; that is the trade, and it is written down.
bool ubiqos_keys_derive(const char *label, uint8_t out[32])
{
    if (!plain) return false;
    char msg[UBIQOS_KEY_NAME_MAX + 8];
    uint32_t n = 0;
    for (const char *p = "derive:"; *p; p++) msg[n++] = *p;
    for (uint32_t i = 0; label[i] && n < sizeof msg; i++) msg[n++] = label[i];
    ubiqos_hmac_sha256(unlocked_key, sizeof unlocked_key, msg, n, out);
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
// core 1, which runs kernel code too and parks in a loop of its own, in RAM,
// while core 0 writes.

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

// A NULL image erases and writes nothing back, which is how a store is
// destroyed.
static void __not_in_flash_func(do_write)(uint32_t offset, const uint8_t *data)
{
    const uint32_t st = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    if (data) flash_range_program(offset, data, FLASH_SECTOR_SIZE);
    restore_interrupts(st);
}

// Seal what is in SRAM and put it in the copy that is not live, so that the
// live one stays whole until this one is complete. A fresh nonce every time,
// because a nonce used twice with one key is the way ChaCha20 is broken.
static int32_t seal_and_write(void)
{
    if (!plain || !live) return -1;

    key_store_t *img = ubiqos_tlsf_malloc(ubiqos_mem_pool, sizeof *img);
    if (!img) return -1;
    memcpy(img, live, sizeof *img);
    img->seq = live->seq + 1;
    ubiqos_random_bytes(img->nonce, sizeof img->nonce);
    ubiqos_seal(unlocked_key, img->nonce, (const uint8_t *)plain, img->sealed,
                sizeof(key_plain_t), img->tag);

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
    memset(img, 0, sizeof *img);
    ubiqos_tlsf_free(ubiqos_mem_pool, img);
    return rc;
}

int32_t ubiqos_keys_set(const char *name, const uint8_t *value, uint32_t len)
{
    if (!plain) return -5;                       // locked
    if (!name || !name[0] || !len || len > UBIQOS_KEY_VALUE_MAX) return -1;
    uint32_t n = 0;
    while (name[n]) n++;
    if (n >= UBIQOS_KEY_NAME_MAX) return -1;

    key_slot_t *slot = 0, *spare = 0;
    for (uint32_t i = 0; i < UBIQOS_KEY_SLOTS; i++) {
        if (plain->slot[i].len && name_eq(plain->slot[i].name, name)) { slot = &plain->slot[i]; break; }
        if (!plain->slot[i].len && !spare) spare = &plain->slot[i];
    }
    if (!slot) slot = spare;
    if (!slot) return -2;                        // full

    key_slot_t keep;
    memcpy(&keep, slot, sizeof keep);            // to put back if the write fails
    memset(slot, 0, sizeof *slot);
    uint32_t k = 0;
    while (k < UBIQOS_KEY_NAME_MAX - 1 && name[k]) { slot->name[k] = name[k]; k++; }
    slot->name[k] = 0;
    memcpy(slot->value, value, len);
    slot->len = (uint16_t)len;

    const int32_t rc = seal_and_write();
    if (rc != 0) memcpy(slot, &keep, sizeof keep);
    memset(&keep, 0, sizeof keep);
    return rc;
}

int32_t ubiqos_keys_remove(const char *name)
{
    if (!plain) return -5;
    key_slot_t *slot = (key_slot_t *)find(name);
    if (!slot) return -1;

    key_slot_t keep;
    memcpy(&keep, slot, sizeof keep);
    memset(slot, 0, sizeof *slot);
    const int32_t rc = seal_and_write();
    if (rc != 0) memcpy(slot, &keep, sizeof keep);
    memset(&keep, 0, sizeof keep);
    return rc;
}

// Open the store, or make one. Both take the same second of PBKDF2, which is
// the point of it: a passphrase guessed at a thousand a second on a desktop
// is guessed at one a second against this.
// Forget the whole store: both copies erased, and with them every key and the
// passphrase that opened them. There is no way back, which is the point --
// a board being passed on should not carry anybody's secrets.
int32_t ubiqos_keys_destroy(void)
{
    ubiqos_keys_lock();
    if (!park_core1()) return -3;
    do_write((UBIQOS_FLASH_KEYS_BASE - XIP_BASE), 0);
    do_write((UBIQOS_FLASH_KEYS_BASE - XIP_BASE) + FLASH_SECTOR_SIZE, 0);
    release_core1();
    live = 0;
    return 0;
}

int32_t ubiqos_keys_unlock(const uint8_t *pass, uint32_t plen)
{
    if (!pass || !plen) return -1;
    if (plain) return 0;                         // already open

    key_plain_t *fresh = ubiqos_tlsf_malloc(ubiqos_mem_pool, sizeof *fresh);
    if (!fresh) return -1;

    if (!live) {
        // No store yet: this passphrase becomes the one, and an empty store is
        // written so that the next unlock has something to check against.
        key_store_t *img = ubiqos_tlsf_malloc(ubiqos_mem_pool, sizeof *img);
        if (!img) { ubiqos_tlsf_free(ubiqos_mem_pool, fresh); return -1; }
        memset(img, 0, sizeof *img);
        img->magic  = KEYS_MAGIC;
        img->format = KEYS_FORMAT;
        img->seq    = 1;
        img->rounds = KEYS_ROUNDS;
        ubiqos_random_bytes(img->salt, sizeof img->salt);
        ubiqos_random_bytes(img->nonce, sizeof img->nonce);
        memset(fresh, 0, sizeof *fresh);
        derive(img, pass, plen, unlocked_key);
        ubiqos_seal(unlocked_key, img->nonce, (const uint8_t *)fresh, img->sealed,
                    sizeof *fresh, img->tag);

        const uint32_t offset = UBIQOS_FLASH_KEYS_BASE - XIP_BASE;
        int32_t rc = 0;
        if (!park_core1()) rc = -3;
        else {
            do_write(offset, (const uint8_t *)img);
            release_core1();
            const key_store_t *w = copy_at(0);
            if (copy_is_good(w) && w->seq == 1) live = w;
            else rc = -4;
        }
        memset(img, 0, sizeof *img);
        ubiqos_tlsf_free(ubiqos_mem_pool, img);
        if (rc != 0) {
            memset(unlocked_key, 0, sizeof unlocked_key);
            ubiqos_tlsf_free(ubiqos_mem_pool, fresh);
            return rc;
        }
        plain = fresh;
        return 1;                                // made a new one
    }

    uint8_t key[32];
    derive(live, pass, plen, key);
    const bool ok = ubiqos_unseal(key, live->nonce, live->sealed, (uint8_t *)fresh,
                                  sizeof *fresh, live->tag);
    if (!ok) {
        memset(key, 0, sizeof key);
        memset(fresh, 0, sizeof *fresh);
        ubiqos_tlsf_free(ubiqos_mem_pool, fresh);
        return -6;                               // the passphrase, or a changed store
    }
    memcpy(unlocked_key, key, sizeof unlocked_key);
    memset(key, 0, sizeof key);
    plain = fresh;
    return 0;
}
