#ifndef UBIQOS_KEYSTORE_H
#define UBIQOS_KEYSTORE_H

#include <stdint.h>
#include <stdbool.h>

#include "../common/ubiqos_abi.h"

// The key store: named secrets in flash, for things that must not sit on the
// card in clear text -- the WiFi password, an API key.
//
// A value goes in and does not come out. There is no call that returns one to
// a process: the kernel keeps them for its own use, the way it already keeps
// the password out of /sd/config.txt. What a program may ask for is the list
// of names, how long each value is, and a fingerprint to compare with the one
// its owner has -- never the bytes.
//
// The store is sealed with ChaCha20-Poly1305, under a key from PBKDF2 over the
// passphrase and the chip's id. Locked, there is nothing to list: the names
// are inside the ciphertext with the values. Unlocked, the records are in
// SRAM and go when the power does.

// The name and value limits are the ABI's; the number of slots is what fits
// in a sector once sealed.
#define UBIQOS_KEY_SLOTS      28

void     ubiqos_keys_init(void);           // read what is in flash, once
uint32_t ubiqos_keys_state(void);          // UBIQOS_KEYS_EMPTY/LOCKED/OPEN
int32_t  ubiqos_keys_unlock(const uint8_t *pass, uint32_t plen);  // 1 = a new store
void     ubiqos_keys_lock(void);
int32_t  ubiqos_keys_destroy(void);        // both copies erased; no way back
uint32_t ubiqos_keys_count(void);
bool     ubiqos_keys_nth(uint32_t index, char *name_out, uint32_t *len_out);
bool     ubiqos_keys_fingerprint(const char *name, uint32_t *out);
int32_t  ubiqos_keys_set(const char *name, const uint8_t *value, uint32_t len);
int32_t  ubiqos_keys_remove(const char *name);

// For the kernel itself, and only the kernel: the bytes. The pointer is into
// flash and stays valid until the store is written again.
const uint8_t *ubiqos_keys_value(const char *name, uint32_t *len_out);

// Core 1 calls this from its own loop so that flash can be written: it parks
// in RAM while core 0 erases, because a fetch from flash during an erase does
// not return. Both live in RAM for the same reason.
void ubiqos_keys_park_here(void);
extern volatile bool ubiqos_keys_core1_running;

#endif
