#ifndef UBIQOS_KEYSTORE_H
#define UBIQOS_KEYSTORE_H

#include <stdint.h>
#include <stdbool.h>

// The key store: named secrets in flash, for things that must not sit on the
// card in clear text -- the WiFi password, an API key.
//
// A value goes in and does not come out. There is no call that returns one to
// a process: the kernel keeps them for its own use, the way it already keeps
// the password out of /sd/config.txt. What a program may ask for is the list
// of names, how long each value is, and a fingerprint to compare with the one
// its owner has -- never the bytes.
//
// Nothing here is encrypted yet. This is the storage; lock and unlock come
// next, and the layout leaves room for them: the header carries a format
// number, and a sealed store will be the same records behind a nonce and a
// tag.

#define UBIQOS_KEY_NAME_MAX   24     // including the terminator
#define UBIQOS_KEY_VALUE_MAX  96     // a 512-bit key in hex is 128; this is
                                     // enough for the tokens in use and keeps
                                     // a slot to 128 bytes
#define UBIQOS_KEY_SLOTS      30

void     ubiqos_keys_init(void);           // read what is in flash, once
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
