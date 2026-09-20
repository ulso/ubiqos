#ifndef UBIQOS_CRYPTO_H
#define UBIQOS_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>

// SHA-256, HMAC, PBKDF2 and ChaCha20-Poly1305 -- what the key store needs to
// be sealed with a passphrase. See kernel/crypto.c for why they are here and
// not from a library.

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint32_t n;
    uint8_t  buf[64];
} ubiqos_sha256_t;

void ubiqos_sha256_init(ubiqos_sha256_t *s);
void ubiqos_sha256_update(ubiqos_sha256_t *s, const void *data, uint32_t len);
void ubiqos_sha256_final(ubiqos_sha256_t *s, uint8_t out[32]);
void ubiqos_sha256(const void *data, uint32_t len, uint8_t out[32]);

void ubiqos_hmac_sha256(const void *key, uint32_t klen, const void *msg, uint32_t mlen,
                        uint8_t out[32]);

// One 32-byte block of PBKDF2-HMAC-SHA256, which is a key.
void ubiqos_pbkdf2_sha256(const void *pass, uint32_t plen, const void *salt, uint32_t slen,
                          uint32_t iterations, uint8_t out[32]);

// ChaCha20-Poly1305 with no additional data. The tag is what makes a wrong
// passphrase and a tampered store the same answer.
void ubiqos_seal(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t *plain, uint8_t *cipher, uint32_t len, uint8_t tag[16]);
bool ubiqos_unseal(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t *cipher, uint8_t *plain, uint32_t len, const uint8_t tag[16]);

#endif
