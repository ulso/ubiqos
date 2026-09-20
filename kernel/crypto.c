// The little cryptography the kernel needs, and nothing more: SHA-256,
// HMAC-SHA256, PBKDF2 and ChaCha20-Poly1305. It is here because the key store
// is sealed with a passphrase, and the kernel runs from SRAM where mbedTLS
// does not fit -- lib/tls is for modules, which have PSRAM.
//
// SHA-256 in software rather than through the RP2350's accelerator. The
// accelerator wants a DMA channel and a hardware lock, and DMA channels on the
// 4.3B are already contested; software SHA-256 is about 1.5 microseconds a
// block here, which makes a hundred thousand PBKDF2 iterations well under a
// second. The other reason is testing: this file compiles on a desktop
// unchanged, and it was checked against the published test vectors there
// before it went anywhere near the board.
//
// ChaCha20-Poly1305 is RFC 8439. It is the AEAD to write by hand: additions,
// rotations and one multiply-accumulate, no tables, nothing that leaks through
// a cache -- and it authenticates, so a wrong passphrase and a tampered store
// are the same answer: no.

#include <string.h>
#include "crypto.h"

// On the board the RP2350's own SHA-256 does the work: the software version
// below is thirty times slower, which turned a one-second unlock into half a
// minute. The software one stays because it is what compiles on a desktop,
// where this file is checked against the published test vectors -- and the two
// must agree, which the board proves every time it opens a store sealed by the
// other.
//
// One user at a time: there is no lock here because the key store is the only
// thing in the kernel that hashes, and it does so from one thread.
#ifdef UBIQOS_SHA256_HW
#include "hardware/sha256.h"
#endif

// --- SHA-256 ----------------------------------------------------------------

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }


#ifdef UBIQOS_SHA256_HW
// The hardware takes whole blocks and starts from the standard initial value,
// which is all this file needs: every hash here is a short message from its
// own beginning.
static void sha256_block(ubiqos_sha256_t *s, const uint8_t *p)
{
    (void)s;
    for (int i = 0; i < 16; i++) {
        const uint32_t w = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
                           ((uint32_t)p[4*i+2] << 8) | (uint32_t)p[4*i+3];
        sha256_wait_ready_blocking();
        *(volatile uint32_t *)sha256_get_write_addr() = w;
    }
}
#else
static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

static void sha256_block(ubiqos_sha256_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
               ((uint32_t)p[4*i+2] << 8) | (uint32_t)p[4*i+3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
        const uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
#endif

void ubiqos_sha256_init(ubiqos_sha256_t *s)
{
#ifdef UBIQOS_SHA256_HW
    sha256_set_bswap(false);            // the words are assembled big-endian here
    sha256_set_dma_size(4);             // and written a word at a time
    sha256_start();
#endif
    static const uint32_t iv[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u,
    };
    for (int i = 0; i < 8; i++) s->h[i] = iv[i];
    s->len = 0;
    s->n = 0;
}

void ubiqos_sha256_update(ubiqos_sha256_t *s, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    s->len += len;
    while (len) {
        const uint32_t room = 64 - s->n;
        const uint32_t take = len < room ? len : room;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 64) { sha256_block(s, s->buf); s->n = 0; }
    }
}

void ubiqos_sha256_final(ubiqos_sha256_t *s, uint8_t out[32])
{
    const uint64_t bits = (uint64_t)s->len * 8u;
    static const uint8_t pad = 0x80;
    ubiqos_sha256_update(s, &pad, 1);
    static const uint8_t zero = 0;
    while (s->n != 56) ubiqos_sha256_update(s, &zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - 8*i));
    ubiqos_sha256_update(s, tail, 8);
#ifdef UBIQOS_SHA256_HW
    sha256_wait_valid_blocking();
    sha256_result_t res;
    sha256_get_result(&res, SHA256_BIG_ENDIAN);
    memcpy(out, res.bytes, 32);
    memset(&res, 0, sizeof res);
#else
    for (int i = 0; i < 8; i++) {
        out[4*i]   = (uint8_t)(s->h[i] >> 24);
        out[4*i+1] = (uint8_t)(s->h[i] >> 16);
        out[4*i+2] = (uint8_t)(s->h[i] >> 8);
        out[4*i+3] = (uint8_t)s->h[i];
    }
#endif
}

void ubiqos_sha256(const void *data, uint32_t len, uint8_t out[32])
{
    ubiqos_sha256_t s;
    ubiqos_sha256_init(&s);
    ubiqos_sha256_update(&s, data, len);
    ubiqos_sha256_final(&s, out);
}

// --- HMAC and PBKDF2 --------------------------------------------------------

static void hmac(const uint8_t *key, uint32_t klen, const uint8_t *msg, uint32_t mlen,
                 const uint8_t *msg2, uint32_t mlen2, uint8_t out[32])
{
    uint8_t k[64], pad[64];
    memset(k, 0, sizeof k);
    if (klen > 64) ubiqos_sha256(key, klen, k);
    else           memcpy(k, key, klen);

    ubiqos_sha256_t s;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    ubiqos_sha256_init(&s);
    ubiqos_sha256_update(&s, pad, 64);
    ubiqos_sha256_update(&s, msg, mlen);
    if (mlen2) ubiqos_sha256_update(&s, msg2, mlen2);
    uint8_t inner[32];
    ubiqos_sha256_final(&s, inner);

    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    ubiqos_sha256_init(&s);
    ubiqos_sha256_update(&s, pad, 64);
    ubiqos_sha256_update(&s, inner, 32);
    ubiqos_sha256_final(&s, out);

    memset(k, 0, sizeof k);
    memset(pad, 0, sizeof pad);
    memset(inner, 0, sizeof inner);
}

void ubiqos_hmac_sha256(const void *key, uint32_t klen, const void *msg, uint32_t mlen,
                        uint8_t out[32])
{
    hmac(key, klen, msg, mlen, 0, 0, out);
}

// One 32-byte block is all this is asked for, so there is no block counter
// loop: PBKDF2's first block with the index 1 appended to the salt.
void ubiqos_pbkdf2_sha256(const void *pass, uint32_t plen, const void *salt, uint32_t slen,
                          uint32_t iterations, uint8_t out[32])
{
    static const uint8_t one[4] = { 0, 0, 0, 1 };
    uint8_t u[32], acc[32];
    hmac(pass, plen, salt, slen, one, sizeof one, u);
    memcpy(acc, u, 32);
    for (uint32_t i = 1; i < iterations; i++) {
        hmac(pass, plen, u, 32, 0, 0, u);
        for (int k = 0; k < 32; k++) acc[k] ^= u[k];
    }
    memcpy(out, acc, 32);
    memset(u, 0, sizeof u);
    memset(acc, 0, sizeof acc);
}

// --- ChaCha20 ---------------------------------------------------------------

#define QR(a,b,c,d) ( a += b, d ^= a, d = ror(d,16), \
                      c += d, b ^= c, b = ror(b,20), \
                      a += b, d ^= a, d = ror(d,24), \
                      c += d, b ^= c, b = ror(b,25) )

static void chacha_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                         uint8_t out[64])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++)
        s[4+i] = (uint32_t)key[4*i] | ((uint32_t)key[4*i+1] << 8) |
                 ((uint32_t)key[4*i+2] << 16) | ((uint32_t)key[4*i+3] << 24);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13+i] = (uint32_t)nonce[4*i] | ((uint32_t)nonce[4*i+1] << 8) |
                  ((uint32_t)nonce[4*i+2] << 16) | ((uint32_t)nonce[4*i+3] << 24);
    memcpy(x, s, sizeof x);
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[ 8], x[12]);
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        const uint32_t v = x[i] + s[i];
        out[4*i]   = (uint8_t)v;
        out[4*i+1] = (uint8_t)(v >> 8);
        out[4*i+2] = (uint8_t)(v >> 16);
        out[4*i+3] = (uint8_t)(v >> 24);
    }
}

static void chacha_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                       const uint8_t *in, uint8_t *out, uint32_t len)
{
    uint8_t block[64];
    while (len) {
        chacha_block(key, counter++, nonce, block);
        const uint32_t take = len < 64 ? len : 64;
        for (uint32_t i = 0; i < take; i++) out[i] = in[i] ^ block[i];
        in += take; out += take; len -= take;
    }
    memset(block, 0, sizeof block);
}

// --- Poly1305 ---------------------------------------------------------------
//
// The 130-bit accumulator in five 26-bit limbs, which is what makes it fit in
// 32-bit arithmetic without a wide multiply.

typedef struct {
    uint32_t r[5], h[5], pad[4];
} poly_t;

static void poly_init(poly_t *p, const uint8_t key[32])
{
    const uint32_t t0 = (uint32_t)key[0] | ((uint32_t)key[1]<<8) | ((uint32_t)key[2]<<16) | ((uint32_t)key[3]<<24);
    const uint32_t t1 = (uint32_t)key[4] | ((uint32_t)key[5]<<8) | ((uint32_t)key[6]<<16) | ((uint32_t)key[7]<<24);
    const uint32_t t2 = (uint32_t)key[8] | ((uint32_t)key[9]<<8) | ((uint32_t)key[10]<<16) | ((uint32_t)key[11]<<24);
    const uint32_t t3 = (uint32_t)key[12] | ((uint32_t)key[13]<<8) | ((uint32_t)key[14]<<16) | ((uint32_t)key[15]<<24);
    p->r[0] = t0 & 0x3ffffffu;
    p->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03u;
    p->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffu;
    p->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffu;
    p->r[4] = (t3 >> 8) & 0x00fffffu;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++)
        p->pad[i] = (uint32_t)key[16+4*i] | ((uint32_t)key[17+4*i]<<8) |
                    ((uint32_t)key[18+4*i]<<16) | ((uint32_t)key[19+4*i]<<24);
}

static void poly_blocks(poly_t *p, const uint8_t *m, uint32_t bytes, uint32_t final)
{
    const uint32_t hibit = final ? 0 : (1u << 24);
    const uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];

    while (bytes >= 16) {
        const uint32_t t0 = (uint32_t)m[0] | ((uint32_t)m[1]<<8) | ((uint32_t)m[2]<<16) | ((uint32_t)m[3]<<24);
        const uint32_t t1 = (uint32_t)m[4] | ((uint32_t)m[5]<<8) | ((uint32_t)m[6]<<16) | ((uint32_t)m[7]<<24);
        const uint32_t t2 = (uint32_t)m[8] | ((uint32_t)m[9]<<8) | ((uint32_t)m[10]<<16) | ((uint32_t)m[11]<<24);
        const uint32_t t3 = (uint32_t)m[12] | ((uint32_t)m[13]<<8) | ((uint32_t)m[14]<<16) | ((uint32_t)m[15]<<24);

        h0 += t0 & 0x3ffffffu;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x3ffffffu;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffffu;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffffu;
        h4 += (t3 >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu;
        h1 += c;

        m += 16; bytes -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_finish(poly_t *p, const uint8_t *m, uint32_t left, uint8_t tag[16])
{
    if (left) {
        uint8_t block[16];
        memset(block, 0, sizeof block);
        memcpy(block, m, left);
        block[left] = 1;
        poly_blocks(p, block, 16, 1);
        memset(block, 0, sizeof block);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    uint32_t c = h1 >> 26; h1 &= 0x3ffffffu;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffffu;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffffu;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffffu;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffffu;
    h1 += c;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffffu;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffffu;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffffu;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffffu;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1;            // all ones when g is the answer
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    uint64_t f = (uint64_t)((h0) | (h1 << 26)) + (uint64_t)p->pad[0];
    uint32_t w0 = (uint32_t)f;
    f = (uint64_t)((h1 >> 6) | (h2 << 20)) + (uint64_t)p->pad[1] + (f >> 32);
    uint32_t w1 = (uint32_t)f;
    f = (uint64_t)((h2 >> 12) | (h3 << 14)) + (uint64_t)p->pad[2] + (f >> 32);
    uint32_t w2 = (uint32_t)f;
    f = (uint64_t)((h3 >> 18) | (h4 << 8)) + (uint64_t)p->pad[3] + (f >> 32);
    uint32_t w3 = (uint32_t)f;

    const uint32_t w[4] = { w0, w1, w2, w3 };
    for (int i = 0; i < 4; i++) {
        tag[4*i]   = (uint8_t)w[i];
        tag[4*i+1] = (uint8_t)(w[i] >> 8);
        tag[4*i+2] = (uint8_t)(w[i] >> 16);
        tag[4*i+3] = (uint8_t)(w[i] >> 24);
    }
    memset(p, 0, sizeof *p);
}

// --- ChaCha20-Poly1305, RFC 8439 -------------------------------------------
//
// No additional data: everything this seals is the whole message.

static void tag_of(const uint8_t key[32], const uint8_t nonce[12],
                   const uint8_t *cipher, uint32_t len, uint8_t tag[16])
{
    uint8_t otk[64];
    chacha_block(key, 0, nonce, otk);          // counter 0 makes the one-time key
    poly_t p;
    poly_init(&p, otk);

    // No additional data, so the message is the ciphertext, zero-padded to a
    // whole block, and then the lengths -- both counted as full blocks, which
    // is what RFC 8439 says and what the "final" flag here means.
    const uint32_t full = len & ~15u;
    poly_blocks(&p, cipher, full, 0);
    if (len & 15u) {
        uint8_t block[16];
        memset(block, 0, sizeof block);
        memcpy(block, cipher + full, len & 15u);
        poly_blocks(&p, block, 16, 0);
        memset(block, 0, sizeof block);
    }
    uint8_t lengths[16];
    memset(lengths, 0, sizeof lengths);        // additional data: none
    for (int i = 0; i < 8; i++) lengths[8+i] = (uint8_t)((uint64_t)len >> (8*i));
    poly_blocks(&p, lengths, 16, 0);

    poly_finish(&p, 0, 0, tag);
    memset(otk, 0, sizeof otk);
}

void ubiqos_seal(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t *plain, uint8_t *cipher, uint32_t len, uint8_t tag[16])
{
    chacha_xor(key, 1, nonce, plain, cipher, len);
    tag_of(key, nonce, cipher, len, tag);
}

bool ubiqos_unseal(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t *cipher, uint8_t *plain, uint32_t len, const uint8_t tag[16])
{
    uint8_t want[16];
    tag_of(key, nonce, cipher, len, want);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ tag[i]);
    memset(want, 0, sizeof want);
    if (diff) return false;                    // wrong key, or somebody edited it
    chacha_xor(key, 1, nonce, cipher, plain, len);
    return true;
}
