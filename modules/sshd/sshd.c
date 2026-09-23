// The curve's own constants have to be readable to be printed, and mbedTLS
// hides them behind MBEDTLS_PRIVATE. This is the sanctioned way in, and it is
// here for the self-test and nothing else.
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include <string.h>
#include <stdio.h>

#include "../../common/ubiqos_abi.h"

#include "mbedtls/bignum.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"

// sshd -- a shell over SSH.
//
//   sshd [port]     answer on 22, or where you say
//
// netcon gave the board a shell over TCP and said out loud what it was: no
// encryption, no password, and therefore the USB cable only. This is the same
// shell with the other half done. What it carries is the same pipe pair into
// the ordinary `sh`; what is new is everything between the socket and that.
//
// WHAT IT IMPLEMENTS, and no more:
//
//   kex           curve25519-sha256
//   host key      ecdsa-sha2-nistp256
//   cipher        aes256-gcm@openssh.com, both directions
//   auth          password, checked against the key store
//   channel       one session, one shell, with a pty the client asks for
//
// One of each, because a second choice is a second thing to get wrong and
// every OpenSSH client made this decade offers all five. A client that offers
// none of them is told which is missing rather than left to guess.
//
// NOT Ed25519, which everybody's keys are: mbedTLS has no Edwards curves at
// all, so neither the host key nor a client key can be one. The host key is
// ECDSA P-256, which every client accepts.
//
// WHAT THIS IS NOT: an audited SSH server. It is a few hundred lines written
// against the RFCs by two people who wanted a prompt over the air. The
// cryptography underneath is mbedTLS and the randomness is the chip's own
// generator, but the protocol around them is ours, and a protocol is where SSH
// servers have historically gone wrong. Put it on your own network. Do not put
// it on the internet.

UBIQOS_MEM_SIZE(49152);
uint32_t ubiqos_heap_bytes = 192u * 1024u;   // mbedTLS's contexts, in PSRAM

#define DEFAULT_PORT 22
#define NET_WAIT_S   30
#define PKT_MAX      4096           // what we will read; the channel asks for less
// The channel's window is exactly the room there is to hold typed input on its
// way to the shell. It used to be thirty-two kilobytes, given straight back
// after every packet -- "the board is never the reason a session waits" -- which
// meant the client could always send, the input had to be pushed into the shell
// whether it had room or not, and a busy shell stopped sshd dead. Now the client
// may send no more than fits here, and the window reopens only as the shell
// takes it.
#define CHAN_WINDOW  2048
#define CHAN_PACKET  1024

#define KEY_HOST     "ssh.hostkey"
#define KEY_PASSWORD "ssh.password"

// SSH message numbers, of which this speaks a dozen.
#define MSG_DISCONNECT      1
#define MSG_IGNORE          2
#define MSG_UNIMPLEMENTED   3
#define MSG_DEBUG           4
#define MSG_SERVICE_REQUEST 5
#define MSG_SERVICE_ACCEPT  6
#define MSG_KEXINIT         20
#define MSG_NEWKEYS         21
#define MSG_KEX_ECDH_INIT   30
#define MSG_KEX_ECDH_REPLY  31
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_BANNER  53
#define MSG_GLOBAL_REQUEST   80
#define MSG_CHANNEL_OPEN     90
#define MSG_CHANNEL_OPEN_CONFIRMATION 91
#define MSG_CHANNEL_OPEN_FAILURE 92
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA     94
#define MSG_CHANNEL_EOF      96
#define MSG_CHANNEL_CLOSE    97
#define MSG_CHANNEL_REQUEST  98
#define MSG_CHANNEL_SUCCESS  99
#define MSG_CHANNEL_FAILURE  100

static const char VERSION[] = "SSH-2.0-UbiqOS";

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

// `sshd -d` prints the numbers the exchange is built from, so that a client
// which disagrees can be compared with rather than guessed at. It prints an
// ephemeral secret, which is why it is not the default: the handshake it
// belongs to is the one being debugged and no other.
static bool debugging;

static void say_hex(const char *what, const uint8_t *p, uint32_t n) {
    if (!debugging) return;
    static const char hex[] = "0123456789abcdef";
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, what);
    ubiqos_line_str(&l, " ");
    for (uint32_t i = 0; i < n; i++) {
        const char two[3] = { hex[p[i] >> 4], hex[p[i] & 15], 0 };
        ubiqos_line_str(&l, two);
        if ((i % 32) == 31 || i + 1 == n) {
            ubiqos_line_str(&l, "\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "     ");
        }
    }
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

static void say_num(const char *what, int32_t n) {
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, what);
    ubiqos_line_str(&l, " ");
    if (n < 0) { ubiqos_line_str(&l, "-"); n = -n; }
    ubiqos_line_u32(&l, (uint32_t)n);
    ubiqos_line_str(&l, "\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

// --- WRITING AND READING THE WIRE'S TYPES -----------------------------------
//
// SSH has four: a byte, a uint32 big-endian, a string with its length in front
// (which is also how opaque blobs travel), and an mpint -- a big-endian number
// whose top bit must not be mistaken for a sign, so a zero byte goes in front
// when it would be.

typedef struct { uint8_t *b; uint32_t cap, n; bool over; } wr_t;

static void w_raw(wr_t *w, const void *p, uint32_t n) {
    if (w->n + n > w->cap) { w->over = true; return; }
    memcpy(w->b + w->n, p, n);
    w->n += n;
}
static void w_byte(wr_t *w, uint8_t v) { w_raw(w, &v, 1); }
static void w_u32(wr_t *w, uint32_t v) {
    const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    w_raw(w, b, 4);
}
static void w_strn(wr_t *w, const void *p, uint32_t n) { w_u32(w, n); w_raw(w, p, n); }
static void w_str(wr_t *w, const char *s) { w_strn(w, s, (uint32_t)strlen(s)); }

static void w_mpint(wr_t *w, const uint8_t *be, uint32_t n) {
    uint32_t i = 0;
    while (i < n && !be[i]) i++;                 // no leading zeroes
    if (i == n) { w_u32(w, 0); return; }         // the number is zero
    const bool sign = (be[i] & 0x80u) != 0;      // would read as negative
    w_u32(w, (n - i) + (sign ? 1u : 0u));
    if (sign) w_byte(w, 0);
    w_raw(w, be + i, n - i);
}

typedef struct { const uint8_t *b; uint32_t n, at; bool over; } rd_t;

static uint8_t r_byte(rd_t *r) {
    if (r->at + 1 > r->n) { r->over = true; return 0; }
    return r->b[r->at++];
}
static uint32_t r_u32(rd_t *r) {
    if (r->at + 4 > r->n) { r->over = true; r->at = r->n; return 0; }
    const uint8_t *p = r->b + r->at;
    r->at += 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
// The string stays where it is; nothing is copied.
static const uint8_t *r_str(rd_t *r, uint32_t *len) {
    const uint32_t n = r_u32(r);
    if (r->over || r->at + n > r->n) { r->over = true; *len = 0; return r->b; }
    const uint8_t *p = r->b + r->at;
    r->at += n;
    *len = n;
    return p;
}
static bool r_is(const uint8_t *p, uint32_t n, const char *s) {
    return n == strlen(s) && memcmp(p, s, n) == 0;
}
// Is this name one of the comma-separated ones the client listed?
static bool listed(const uint8_t *list, uint32_t n, const char *want) {
    const uint32_t w = (uint32_t)strlen(want);
    uint32_t i = 0;
    while (i < n) {
        uint32_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j - i == w && memcmp(list + i, want, w) == 0) return true;
        i = j + 1;
    }
    return false;
}

// --- THE CONNECTION ---------------------------------------------------------

static int32_t sock;
static uint32_t seq_in, seq_out;
static bool encrypted;
static mbedtls_gcm_context gcm_in, gcm_out;
static uint8_t iv_in[12], iv_out[12];

static uint8_t rxbuf[PKT_MAX];        // one packet as it arrives
static uint8_t txbuf[PKT_MAX];        // and one being built
static uint8_t payload[PKT_MAX];      // the packet's contents, unwrapped

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;

static int trng_source(void *data, unsigned char *out, size_t len, size_t *olen) {
    (void)data;
    uint8_t raw[64];
    size_t n = 0;
    while (n < len) {
        const int32_t got = ubiqos_random_trng(raw, sizeof raw);
        if (got < 8) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        for (int32_t i = 0; i + 8 <= got && n < len; i += 8) {
            uint8_t b = 0;
            for (int k = 0; k < 8; k++) b ^= raw[i + k];
            out[n++] = b;                     // eight bytes folded into one
        }
    }
    *olen = n;
    return 0;
}

// mbedTLS makes keys with a random number generator and nothing else, and the
// host key must be the SAME key every time this starts. So the derived seed is
// turned into a generator: SHA-256 of the seed and a counter, which is as
// deterministic as the seed and looks like nothing in particular. It is used
// for the host key and never for anything that must be unpredictable.
typedef struct { uint8_t seed[32]; uint32_t counter; } det_t;

static int det_random(void *ctx, unsigned char *out, size_t len) {
    det_t *d = (det_t *)ctx;
    while (len) {
        uint8_t block[32];
        const uint8_t c[4] = { (uint8_t)(d->counter >> 24), (uint8_t)(d->counter >> 16),
                               (uint8_t)(d->counter >> 8), (uint8_t)d->counter };
        mbedtls_sha256_context h;
        mbedtls_sha256_init(&h);
        mbedtls_sha256_starts(&h, 0);
        mbedtls_sha256_update(&h, d->seed, sizeof d->seed);
        mbedtls_sha256_update(&h, c, sizeof c);
        mbedtls_sha256_finish(&h, block);
        mbedtls_sha256_free(&h);
        d->counter++;
        const size_t take = len < sizeof block ? len : sizeof block;
        memcpy(out, block, take);
        out += take;
        len -= take;
    }
    return 0;
}

// Every socket read goes through here: sockets do not wait, so waiting is done
// by asking again. Returns false if the peer went away or the patience ran out.
static bool peer_gone;

static bool read_exact(uint8_t *out, uint32_t n, uint32_t ms) {
    uint32_t got = 0;
    uint32_t waited = 0;
    while (got < n) {
        const int32_t r = ubiqos_sock_recv(sock, out + got, n - got);
        if (r < 0) { peer_gone = true; return false; }
        if (r == 0) {
            if (waited >= ms) return false;
            ubiqos_sleep(5);
            waited += 5;
            continue;
        }
        got += (uint32_t)r;
        waited = 0;
    }
    return true;
}

static bool write_all(const uint8_t *p, uint32_t n) {
    uint32_t sent = 0;
    while (sent < n) {
        const int32_t r = ubiqos_sock_send(sock, p + sent, n - sent);
        if (r < 0) return false;
        if (r == 0) { ubiqos_sleep(5); continue; }
        sent += (uint32_t)r;
    }
    return true;
}

// The nonce is the derived IV with its last eight bytes counting packets. It
// must never repeat under one key, which is the whole reason it is here and
// not folded into the send.
static void bump(uint8_t iv[12]) {
    for (int i = 11; i >= 4; i--) if (++iv[i]) break;
}

// One packet out: length, padding length, the payload, random padding. Plain
// until NEWKEYS, and after it the length is the only thing still in the clear
// -- GCM authenticates it as additional data rather than hiding it.
static bool send_packet(const uint8_t *pl, uint32_t n) {
    const uint32_t block = encrypted ? 16u : 8u;
    uint32_t pad = block - ((encrypted ? (1 + n) : (4 + 1 + n)) % block);
    if (pad < 4) pad += block;
    const uint32_t len = 1 + n + pad;
    if (4 + len + 16 > PKT_MAX) return false;

    txbuf[0] = (uint8_t)(len >> 24); txbuf[1] = (uint8_t)(len >> 16);
    txbuf[2] = (uint8_t)(len >> 8);  txbuf[3] = (uint8_t)len;
    txbuf[4] = (uint8_t)pad;
    memcpy(txbuf + 5, pl, n);
    if (mbedtls_ctr_drbg_random(&drbg, txbuf + 5 + n, pad) != 0) return false;

    seq_out++;
    if (!encrypted) return write_all(txbuf, 4 + len);

    uint8_t out[PKT_MAX], tag[16];
    if (mbedtls_gcm_crypt_and_tag(&gcm_out, MBEDTLS_GCM_ENCRYPT, len,
                                  iv_out, sizeof iv_out, txbuf, 4,
                                  txbuf + 4, out, 16, tag) != 0) return false;
    bump(iv_out);
    memcpy(txbuf + 4, out, len);
    memcpy(txbuf + 4 + len, tag, 16);
    return write_all(txbuf, 4 + len + 16);
}

// And one in. The payload is left in `payload`, its length in *out_len.
static bool recv_packet(uint32_t *out_len, uint32_t ms) {
    uint8_t head[4];
    if (!read_exact(head, 4, ms)) return false;
    const uint32_t len = ((uint32_t)head[0] << 24) | ((uint32_t)head[1] << 16)
                       | ((uint32_t)head[2] << 8) | head[3];
    if (len < 8 || len > PKT_MAX - 32) return false;

    if (!encrypted) {
        if (!read_exact(rxbuf, len, 5000)) return false;
        const uint32_t pad = rxbuf[0];
        if (pad + 1 > len) return false;
        *out_len = len - pad - 1;
        memcpy(payload, rxbuf + 1, *out_len);
        seq_in++;
        return true;
    }

    uint8_t tag[16];
    if (!read_exact(rxbuf, len, 5000)) return false;
    if (!read_exact(tag, 16, 5000)) return false;
    uint8_t plain[PKT_MAX];
    if (mbedtls_gcm_auth_decrypt(&gcm_in, len, iv_in, sizeof iv_in, head, 4,
                                 tag, 16, rxbuf, plain) != 0) return false;
    bump(iv_in);
    const uint32_t pad = plain[0];
    if (pad + 1 > len) return false;
    *out_len = len - pad - 1;
    memcpy(payload, plain + 1, *out_len);
    seq_in++;
    return true;
}

static void disconnect(uint32_t reason, const char *why) {
    uint8_t b[256];
    wr_t w = { b, sizeof b, 0, false };
    w_byte(&w, MSG_DISCONNECT);
    w_u32(&w, reason);
    w_str(&w, why);
    w_str(&w, "");
    send_packet(b, w.n);
}

// --- THE HOST KEY -----------------------------------------------------------
//
// It lives in the key store, which means the store must be unlocked before
// this program will run: a host key on the card is a host key anybody holding
// the card can impersonate the board with. The first run makes one and keeps
// it, so the fingerprint a client remembers goes on being right.

static mbedtls_ecp_group host_grp;
static mbedtls_mpi host_d;
static mbedtls_ecp_point host_q;

static bool host_key_load(void) {
    mbedtls_ecp_group_init(&host_grp);
    mbedtls_mpi_init(&host_d);
    mbedtls_ecp_point_init(&host_q);
    if (mbedtls_ecp_group_load(&host_grp, MBEDTLS_ECP_DP_SECP256R1) != 0) return false;

    ubiqos_keyreq_t r;
    memset(&r, 0, sizeof r);
    if (ubiqos_key_op(UBIQOS_KEY_OP_STATE, &r) != (int32_t)UBIQOS_KEYS_OPEN) {
        say("sshd: the key store is locked -- 'key unlock' first.\r\n"
            "      The host key comes from it, so that a stolen card cannot be\r\n"
            "      used to impersonate this board.\r\n");
        return false;
    }

    // The host key is DERIVED, not stored. The store never hands a value back
    // -- that is its whole promise -- so instead the kernel gives thirty-two
    // bytes of HMAC over the key that opens it, under a label of our choosing.
    // Same board and same passphrase, same key, for ever; nothing on the card,
    // nothing in a file, and nothing to read out of the store.
    strcpy(r.name, KEY_HOST);
    if (ubiqos_key_op(UBIQOS_KEY_OP_DERIVE, &r) != 0 || r.len != 32) {
        say("sshd: the key store would not derive a host key\r\n");
        return false;
    }

    // Through mbedTLS's own key generation rather than by hand: it is what
    // knows that a private key is a number below the curve's order, and it
    // rejects and retries until it has one. Given a deterministic generator it
    // lands on the same key every time.
    det_t det;
    memcpy(det.seed, r.value, sizeof det.seed);
    det.counter = 0;
    memset(r.value, 0, sizeof r.value);
    const int rc = mbedtls_ecp_gen_keypair(&host_grp, &host_d, &host_q, det_random, &det);
    memset(&det, 0, sizeof det);
    return rc == 0;
}


// A P-256 signature over the bytes 00..1f, made by OpenSSL on a desktop. If
// the board cannot verify THIS, the fault is in the board's arithmetic and not
// in anything the protocol does with it.
static const uint8_t test_q[65] = {
    0x04, 0x23, 0x3b, 0x04, 0x72, 0x7e, 0xb8, 0x8f, 0x66, 0x9a, 0x93, 0x8e, 0xab, 0x6b, 0x7c, 0xfd,
    0xa3, 0xde, 0x9e, 0x9d, 0x2a, 0x2f, 0x8e, 0xfd, 0x20, 0x6d, 0x41, 0x66, 0xeb, 0x86, 0x53, 0x3f,
    0xdd, 0xff, 0xe3, 0xbb, 0x23, 0xdb, 0xf7, 0x0d, 0xee, 0xf6, 0xe9, 0x27, 0xf1, 0x54, 0xc4, 0x48,
    0xb7, 0x87, 0xbd, 0x39, 0x40, 0x41, 0x15, 0xfa, 0x79, 0x93, 0x3a, 0x26, 0x79, 0xaa, 0xf3, 0x7b,
    0x48,
};
static const uint8_t test_r[32] = {
    0x91, 0xd3, 0x7e, 0x73, 0xc0, 0x97, 0xd1, 0xd0, 0x3f, 0xe4, 0x25, 0x1a, 0xdb, 0x0d, 0xba, 0xd6,
    0x67, 0x73, 0xed, 0x1b, 0x9f, 0x86, 0x1b, 0xaf, 0x39, 0x00, 0x27, 0x78, 0x00, 0x00, 0x4b, 0x4e,
};
static const uint8_t test_s[32] = {
    0xb6, 0xd7, 0xfb, 0xd6, 0xe8, 0xe9, 0xaf, 0x24, 0xb1, 0xca, 0x68, 0xb7, 0xe0, 0xa1, 0x76, 0xc8,
    0x07, 0x8a, 0x7e, 0x02, 0xbc, 0xb3, 0x9a, 0x55, 0x6d, 0xc9, 0x16, 0x30, 0x81, 0x74, 0x3b, 0xb7,
};

// The same arithmetic with a key that is written down here, so that it can be
// run on a board whose key store is locked -- and so that the public half can
// be worked out independently. `sshd -t` does this and stops.
static const uint8_t fixed_d[32] = {
    0x0c, 0x28, 0xfc, 0xa3, 0x86, 0xc7, 0xa2, 0x27, 0x60, 0x0b, 0x2f, 0xe5, 0x0b, 0x7c, 0xae, 0x11,
    0xec, 0x86, 0xd3, 0xbf, 0x1f, 0xbe, 0x47, 0x1b, 0xe8, 0x98, 0x27, 0xe1, 0x9d, 0x72, 0xaa, 0x1d,
};

static void crypto_selftest(void) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    mbedtls_ecp_point_init(&q);

    say_num("load", mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1));
    say_num("read d", mbedtls_mpi_read_binary(&d, fixed_d, sizeof fixed_d));
    uint8_t out[32];
    mbedtls_mpi_write_binary(&d, out, sizeof out);
    say_hex("d back", out, sizeof out);

    say_num("d*G", mbedtls_ecp_mul(&grp, &q, &d, &grp.G, mbedtls_ctr_drbg_random, &drbg));
    uint8_t qb[65];
    size_t qlen = 0;
    mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &qlen, qb, sizeof qb);
    say_hex("Q", qb, (uint32_t)qlen);

    // The hash itself, printed, because a constant filled in by a loop is the
    // kind of thing a relocated module can get wrong without saying so.
    uint8_t h[32];
    for (int i = 0; i < 32; i++) h[i] = (uint8_t)i;
    say_hex("h", h, sizeof h);

    say_num("sign", mbedtls_ecdsa_sign(&grp, &r, &s, &d, h, sizeof h,
                                       mbedtls_ctr_drbg_random, &drbg));
    uint8_t rb[32], sb[32];
    mbedtls_mpi_write_binary(&r, rb, sizeof rb);
    mbedtls_mpi_write_binary(&s, sb, sizeof sb);
    say_hex("r", rb, sizeof rb);
    say_hex("s", sb, sizeof sb);
    say_num("verify our own", mbedtls_ecdsa_verify(&grp, h, sizeof h, &q, &r, &s));

    say_hex("foreign Q", test_q, sizeof test_q);
    say_hex("foreign r", test_r, sizeof test_r);
    mbedtls_ecp_point tq;
    mbedtls_mpi tr, ts;
    mbedtls_ecp_point_init(&tq);
    mbedtls_mpi_init(&tr); mbedtls_mpi_init(&ts);
    say_num("read foreign point", mbedtls_ecp_point_read_binary(&grp, &tq, test_q, sizeof test_q));
    mbedtls_mpi_read_binary(&tr, test_r, sizeof test_r);
    mbedtls_mpi_read_binary(&ts, test_s, sizeof test_s);
    say_num("verify foreign", mbedtls_ecdsa_verify(&grp, h, sizeof h, &tq, &tr, &ts));
}

// Does this key pair agree with itself? Signing and verifying the same fixed
// hash on the board settles what no amount of reading can: whether the public
// half published in K_S belongs to the private half that signs, and whether
// mbedTLS's ECDSA works at all in a module. Printed so that the same numbers
// can be checked against another implementation off the board.
static void host_key_selftest(void) {
    if (!debugging) return;
    uint8_t h[32];
    for (int i = 0; i < 32; i++) h[i] = (uint8_t)i;

    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    const int rc1 = mbedtls_ecdsa_sign(&host_grp, &r, &s, &host_d, h, sizeof h,
                                       mbedtls_ctr_drbg_random, &drbg);
    const int rc2 = mbedtls_ecdsa_verify(&host_grp, h, sizeof h, &host_q, &r, &s);
    say_num("selftest: sign", rc1);
    say_num("selftest: verify (0 is good)", rc2);

    uint8_t rb[32], sb[32], q[65];
    size_t qlen = 0;
    mbedtls_mpi_write_binary(&r, rb, sizeof rb);
    mbedtls_mpi_write_binary(&s, sb, sizeof sb);
    mbedtls_ecp_point_write_binary(&host_grp, &host_q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                   &qlen, q, sizeof q);
    say_hex("selftest Q", q, (uint32_t)qlen);

    // The curve as this build has it. A group whose order is not the real one
    // signs and verifies perfectly well against itself and against nobody else.
    uint8_t num[48];
    if (mbedtls_mpi_write_binary(&host_grp.N, num, 32) == 0) say_hex("selftest N", num, 32);
    if (mbedtls_mpi_write_binary(&host_grp.P, num, 32) == 0) say_hex("selftest P", num, 32);
    if (mbedtls_mpi_write_binary(&host_grp.G.X, num, 32) == 0) say_hex("selftest Gx", num, 32);

    // And a signature from another implementation, verified here.
    {
        mbedtls_ecp_point tq;
        mbedtls_mpi tr, ts;
        mbedtls_ecp_point_init(&tq);
        mbedtls_mpi_init(&tr);
        mbedtls_mpi_init(&ts);
        int rc = mbedtls_ecp_point_read_binary(&host_grp, &tq, test_q, sizeof test_q);
        say_num("selftest: reading a foreign point", rc);
        mbedtls_mpi_read_binary(&tr, test_r, sizeof test_r);
        mbedtls_mpi_read_binary(&ts, test_s, sizeof test_s);
        rc = mbedtls_ecdsa_verify(&host_grp, h, sizeof h, &tq, &tr, &ts);
        say_num("selftest: a foreign signature (0 is good)", rc);
        mbedtls_ecp_point_free(&tq);
        mbedtls_mpi_free(&tr);
        mbedtls_mpi_free(&ts);
    }
    say_hex("selftest r", rb, sizeof rb);
    say_hex("selftest s", sb, sizeof sb);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
}

// K_S, the host key as the protocol spells it.
static uint32_t host_key_blob(uint8_t *out, uint32_t cap) {
    uint8_t q[65];
    size_t qlen = 0;
    if (mbedtls_ecp_point_write_binary(&host_grp, &host_q,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &qlen,
                                       q, sizeof q) != 0) return 0;
    wr_t w = { out, cap, 0, false };
    w_str(&w, "ecdsa-sha2-nistp256");
    w_str(&w, "nistp256");
    w_strn(&w, q, (uint32_t)qlen);
    return w.over ? 0 : w.n;
}

// The signature over the exchange hash -- and the hash is hashed AGAIN first.
//
// "ecdsa-sha2-nistp256" names a signature scheme, and a signature scheme
// includes its hashing: the message is H and ECDSA hashes it before signing.
// mbedTLS's ecdsa_sign takes the digest that has already been made, so it must
// be given SHA-256(H); OpenSSH's verify calls ssh_digest_memory on H and then
// ECDSA_do_verify, which is the same thing said from the other side.
//
// Signing H itself produces a signature that is valid for a message nobody is
// checking. Both ends agree on H, mbedTLS verifies its own work, and every
// other implementation says no -- which is exactly what happened, and cost an
// afternoon, because the test that was meant to settle it hashed once too.
static uint32_t host_sign(const uint8_t h[32], uint8_t *out, uint32_t cap) {
    uint8_t digest[32];
    if (mbedtls_sha256(h, 32, digest, 0) != 0) return 0;

    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    uint32_t n = 0;
    if (mbedtls_ecdsa_sign(&host_grp, &r, &s, &host_d, digest, sizeof digest,
                           mbedtls_ctr_drbg_random, &drbg) == 0) {
        uint8_t rb[32], sb[32], inner[80];
        if (mbedtls_mpi_write_binary(&r, rb, 32) == 0 &&
            mbedtls_mpi_write_binary(&s, sb, 32) == 0) {
            wr_t iw = { inner, sizeof inner, 0, false };
            w_mpint(&iw, rb, 32);
            w_mpint(&iw, sb, 32);
            wr_t w = { out, cap, 0, false };
            w_str(&w, "ecdsa-sha2-nistp256");
            w_strn(&w, inner, iw.n);
            if (!w.over && !iw.over) n = w.n;
        }
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return n;
}

// --- KEY EXCHANGE -----------------------------------------------------------

static uint8_t session_id[32];
static bool have_session_id;

static void kexinit_payload(wr_t *w) {
    uint8_t cookie[16];
    mbedtls_ctr_drbg_random(&drbg, cookie, sizeof cookie);
    w_byte(w, MSG_KEXINIT);
    w_raw(w, cookie, sizeof cookie);
    w_str(w, "curve25519-sha256,curve25519-sha256@libssh.org");
    w_str(w, "ecdsa-sha2-nistp256");
    w_str(w, "aes256-gcm@openssh.com");      // client to server
    w_str(w, "aes256-gcm@openssh.com");      // server to client
    w_str(w, "");                            // the MAC comes with the cipher
    w_str(w, "");
    w_str(w, "none");
    w_str(w, "none");
    w_str(w, "");
    w_str(w, "");
    w_byte(w, 0);                            // no guessed packet follows
    w_u32(w, 0);
}

// What the client offered, checked one list at a time so that a refusal can
// say which one it was.
static bool client_agrees(const uint8_t *p, uint32_t n) {
    rd_t r = { p, n, 0, false };
    r_byte(&r);                              // the message number
    r.at += 16;                              // the cookie
    uint32_t len;
    const uint8_t *kex = r_str(&r, &len);
    if (!listed(kex, len, "curve25519-sha256") &&
        !listed(kex, len, "curve25519-sha256@libssh.org")) {
        say("sshd: the client will not do curve25519-sha256\r\n");
        return false;
    }
    const uint8_t *hk = r_str(&r, &len);
    if (!listed(hk, len, "ecdsa-sha2-nistp256")) {
        say("sshd: the client will not take an ecdsa-sha2-nistp256 host key.\r\n"
            "      Try: ssh -o HostKeyAlgorithms=+ecdsa-sha2-nistp256 ...\r\n");
        return false;
    }
    const uint8_t *c2s = r_str(&r, &len);
    const bool ok_c2s = listed(c2s, len, "aes256-gcm@openssh.com");
    const uint8_t *s2c = r_str(&r, &len);
    const bool ok_s2c = listed(s2c, len, "aes256-gcm@openssh.com");
    if (!ok_c2s || !ok_s2c) {
        say("sshd: the client will not do aes256-gcm@openssh.com\r\n");
        return false;
    }
    return !r.over;
}

static void hash_string(mbedtls_sha256_context *c, const void *p, uint32_t n) {
    const uint8_t b[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    mbedtls_sha256_update(c, b, 4);
    mbedtls_sha256_update(c, (const uint8_t *)p, n);
}

// One of the six keys the exchange makes, as much of it as is asked for.
static bool derive_key(char which, const uint8_t *kmp, uint32_t kmplen,
                       const uint8_t h[32], uint8_t *out, uint32_t need) {
    uint8_t block[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, kmp, kmplen);
    mbedtls_sha256_update(&c, h, 32);
    mbedtls_sha256_update(&c, (const uint8_t *)&which, 1);
    mbedtls_sha256_update(&c, session_id, 32);
    mbedtls_sha256_finish(&c, block);
    mbedtls_sha256_free(&c);

    uint32_t have = need < 32 ? need : 32;
    memcpy(out, block, have);
    while (have < need) {                    // K1, then K2 = H(K || H || K1)...
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(&c, kmp, kmplen);
        mbedtls_sha256_update(&c, h, 32);
        mbedtls_sha256_update(&c, out, have);
        mbedtls_sha256_finish(&c, block);
        mbedtls_sha256_free(&c);
        const uint32_t take = (need - have) < 32 ? (need - have) : 32;
        memcpy(out + have, block, take);
        have += take;
    }
    return true;
}

static bool do_kex(const char *client_version, const uint8_t *ic, uint32_t iclen,
                   const uint8_t *is, uint32_t islen) {
    uint32_t n;
    if (!recv_packet(&n, 10000) || payload[0] != MSG_KEX_ECDH_INIT) {
        say("sshd: no ECDH init\r\n");
        return false;
    }
    rd_t r = { payload, n, 1, false };
    uint32_t qclen;
    const uint8_t *qc = r_str(&r, &qclen);
    if (r.over || qclen != 32) { say("sshd: a curve25519 point is 32 bytes\r\n"); return false; }

    // Our half of the exchange. Curve25519 speaks little-endian and mbedTLS
    // speaks whatever it is told, so the byte order is written out here rather
    // than assumed.
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point qs, qcp, shared;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&qs); mbedtls_ecp_point_init(&qcp);
    mbedtls_ecp_point_init(&shared);
    bool ok = false;
    uint8_t qsb[32], kb[32];
    size_t olen = 0;

    // Curve25519 puts its points on the wire little-endian, and mbedTLS knows
    // that: read_binary and write_binary do the order for this curve, which is
    // why neither is done here. The clamping of the private key is inside
    // gen_keypair for the same reason.
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) goto out;
    if (mbedtls_ecp_gen_keypair(&grp, &d, &qs, mbedtls_ctr_drbg_random, &drbg) != 0) goto out;
    if (mbedtls_ecp_point_write_binary(&grp, &qs, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, qsb, sizeof qsb) != 0 || olen != 32) goto out;
    if (mbedtls_ecp_point_read_binary(&grp, &qcp, qc, 32) != 0) goto out;
    if (mbedtls_ecp_mul(&grp, &shared, &d, &qcp, mbedtls_ctr_drbg_random, &drbg) != 0) goto out;
    if (mbedtls_ecp_point_write_binary(&grp, &shared, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, kb, sizeof kb) != 0 || olen != 32) goto out;

    {
        // K is the thirty-two bytes as they came, read as a big-endian number.
        //
        // Not the coordinate turned round, which is what a first reading of
        // "curve25519 is little-endian" suggests and what this did at first:
        // the exchange then succeeded, the client accepted the host key, and
        // the signature failed to verify, because both sides had hashed a
        // different K. RFC 8731 and OpenSSH both take the raw output of the
        // scalar multiplication and put it in an mpint as it is -- the bytes,
        // not the number they would be in the other order.
        uint8_t kmp[40];
        wr_t kw = { kmp, sizeof kmp, 0, false };
        w_mpint(&kw, kb, 32);

        uint8_t ks[128], sig[128];
        const uint32_t kslen = host_key_blob(ks, sizeof ks);
        if (!kslen) goto out;
        say_hex("Q_C ", qc, 32);
        say_hex("Q_S ", qsb, 32);
        say_hex("K   ", kb, 32);
        say_hex("K_S ", ks, kslen);

        // The exchange hash: everything both sides have said, in order, so
        // that a man in the middle cannot have changed any of it.
        uint8_t h[32];
        mbedtls_sha256_context c;
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        hash_string(&c, client_version, (uint32_t)strlen(client_version));
        hash_string(&c, VERSION, (uint32_t)strlen(VERSION));
        hash_string(&c, ic, iclen);
        hash_string(&c, is, islen);
        hash_string(&c, ks, kslen);
        hash_string(&c, qc, 32);
        hash_string(&c, qsb, 32);
        mbedtls_sha256_update(&c, kmp, kw.n);
        mbedtls_sha256_finish(&c, h);
        mbedtls_sha256_free(&c);

        say_hex("H   ", h, 32);
        if (!have_session_id) { memcpy(session_id, h, 32); have_session_id = true; }

        const uint32_t siglen = host_sign(h, sig, sizeof sig);
        if (!siglen) goto out;

        uint8_t out_pkt[512];
        wr_t w = { out_pkt, sizeof out_pkt, 0, false };
        w_byte(&w, MSG_KEX_ECDH_REPLY);
        w_strn(&w, ks, kslen);
        w_strn(&w, qsb, 32);
        w_strn(&w, sig, siglen);
        if (w.over || !send_packet(out_pkt, w.n)) goto out;

        const uint8_t newkeys = MSG_NEWKEYS;
        if (!send_packet(&newkeys, 1)) goto out;
        if (!recv_packet(&n, 10000)) {
            // Which is not the same as the wrong message, and saying so cost
            // an hour: the buffer still held the LAST packet, so a client that
            // had given up and gone looked like one repeating itself.
            say("sshd: nothing came back where NEWKEYS should have been --\r\n"
                "      the client did not like the key exchange\r\n");
            goto out;
        }
        if (payload[0] != MSG_NEWKEYS) {
            say_num("sshd: expected NEWKEYS, got message", (int32_t)payload[0]);
            if (payload[0] == MSG_DISCONNECT) {
                rd_t dr = { payload, n, 1, false };
                r_u32(&dr);
                uint32_t wl;
                const uint8_t *why = r_str(&dr, &wl);
                char txt[160];
                uint32_t k = 0;
                while (k < wl && k < sizeof txt - 1) { txt[k] = (char)why[k]; k++; }
                txt[k] = 0;
                say("sshd: the client said: ");
                say(txt);
                say("\r\n");
            }
            goto out;
        }

        // Six keys come out of this; an AEAD cipher uses four of them.
        uint8_t iv_c2s[12], iv_s2c[12], key_c2s[32], key_s2c[32];
        derive_key('A', kmp, kw.n, h, iv_c2s, sizeof iv_c2s);
        derive_key('B', kmp, kw.n, h, iv_s2c, sizeof iv_s2c);
        derive_key('C', kmp, kw.n, h, key_c2s, sizeof key_c2s);
        derive_key('D', kmp, kw.n, h, key_s2c, sizeof key_s2c);

        mbedtls_gcm_init(&gcm_in);
        mbedtls_gcm_init(&gcm_out);
        if (mbedtls_gcm_setkey(&gcm_in, MBEDTLS_CIPHER_ID_AES, key_c2s, 256) != 0) goto out;
        if (mbedtls_gcm_setkey(&gcm_out, MBEDTLS_CIPHER_ID_AES, key_s2c, 256) != 0) goto out;
        memcpy(iv_in, iv_c2s, 12);
        memcpy(iv_out, iv_s2c, 12);
        memset(key_c2s, 0, sizeof key_c2s);
        memset(key_s2c, 0, sizeof key_s2c);
        memset(kb, 0, sizeof kb);
        encrypted = true;
        ok = true;
    }

out:
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&qs); mbedtls_ecp_point_free(&qcp);
    mbedtls_ecp_point_free(&shared);
    return ok;
}

// --- WHO IS ASKING ----------------------------------------------------------
//
// The password is the one in the key store under ssh.password, and the kernel
// is what compares it: the candidate goes in, yes or no comes back, and this
// program never holds the real one. Every byte is looked at whatever the first
// one says, so the time it takes says nothing.

static bool password_ok(const uint8_t *p, uint32_t n) {
    ubiqos_keyreq_t r;
    memset(&r, 0, sizeof r);
    strcpy(r.name, KEY_PASSWORD);
    if (n > sizeof r.value) return false;
    memcpy(r.value, p, n);
    r.len = n;
    const int32_t rc = ubiqos_key_op(UBIQOS_KEY_OP_MATCH, &r);
    memset(r.value, 0, sizeof r.value);
    return rc == 0;
}

static bool do_userauth(void) {
    for (;;) {
        uint32_t n;
        if (!recv_packet(&n, 60000)) return false;
        const uint8_t msg = payload[0];

        if (msg == MSG_SERVICE_REQUEST) {
            rd_t r = { payload, n, 1, false };
            uint32_t sl;
            const uint8_t *s = r_str(&r, &sl);
            if (!r_is(s, sl, "ssh-userauth")) { disconnect(7, "only ssh-userauth"); return false; }
            uint8_t b[64];
            wr_t w = { b, sizeof b, 0, false };
            w_byte(&w, MSG_SERVICE_ACCEPT);
            w_str(&w, "ssh-userauth");
            if (!send_packet(b, w.n)) return false;
            continue;
        }

        if (msg != MSG_USERAUTH_REQUEST) continue;   // IGNORE, DEBUG and the like

        rd_t r = { payload, n, 1, false };
        uint32_t ul, svl, ml;
        r_str(&r, &ul);                              // the user name, which we do not use
        r_str(&r, &svl);
        const uint8_t *method = r_str(&r, &ml);

        if (r_is(method, ml, "password")) {
            r_byte(&r);                              // FALSE: not a change of password
            uint32_t pl;
            const uint8_t *pw = r_str(&r, &pl);
            if (!r.over && password_ok(pw, pl)) {
                const uint8_t ok = MSG_USERAUTH_SUCCESS;
                say("sshd: let in\r\n");
                return send_packet(&ok, 1);
            }
            ubiqos_sleep(1000);                      // no free guessing rate
            say("sshd: wrong password\r\n");
        }

        uint8_t b[64];
        wr_t w = { b, sizeof b, 0, false };
        w_byte(&w, MSG_USERAUTH_FAILURE);
        w_str(&w, "password");
        w_byte(&w, 0);
        if (!send_packet(b, w.n)) return false;
    }
}

// --- THE SHELL --------------------------------------------------------------
//
// The same two pipes netcon uses. A process with a pipe on descriptor 0 cannot
// tell it from a serial port, so `sh` needs to know nothing about any of this.

static int32_t pair[2], shell_pid = -1;   // pair[0] is ours, pair[1] the shell's

// The shell gets a TERMINAL, not two pipes: 0, 1 and 2 all name one end of a
// two-way pair, and the other end is this program. That is what the console
// gives a shell on the screen, and programs rely on it -- `more` prints its
// prompt on descriptor 2 and reads the key from descriptor 2, and with a pipe
// there it printed on the board's own screen and waited for the board's own
// keyboard.
static bool start_shell(void) {
    if (ubiqos_pipepair(pair) < 0) return false;

    // Borrowed for the moment of the exec, as `sh` itself does for `>`.
    const int32_t s0 = ubiqos_dup(UBIQOS_STDIN, -1);
    const int32_t s1 = ubiqos_dup(UBIQOS_STDOUT, -1);
    const int32_t s2 = ubiqos_dup(UBIQOS_STDERR, -1);
    ubiqos_dup(pair[1], UBIQOS_STDIN);
    ubiqos_dup(pair[1], UBIQOS_STDOUT);
    ubiqos_dup(pair[1], UBIQOS_STDERR);
    shell_pid = ubiqos_exec("sh", "");
    ubiqos_dup(s0, UBIQOS_STDIN);
    ubiqos_dup(s1, UBIQOS_STDOUT);
    ubiqos_dup(s2, UBIQOS_STDERR);
    ubiqos_close(s0);
    ubiqos_close(s1);
    ubiqos_close(s2);

    // Our copy of the shell's end. It has to go, or closing the connection
    // leaves the shell with a writer that never stops writing.
    ubiqos_close(pair[1]);
    return shell_pid >= 0;
}

static bool shell_alive(void) {
    for (uint32_t s = 0; s < UBIQOS_PS_SLOTS; s++) {
        ubiqos_psinfo_t p;
        if (ubiqos_psinfo(s, &p) != 0) continue;
        if ((int32_t)p.pid == shell_pid) return p.state != UBIQOS_PS_ZOMBIE;
    }
    return false;
}


// A newline on its own moves down but not back: the console's driver turns LF
// into CR LF and a pipe does not, so a shell whose output looks right on the
// screen comes out as a staircase over the network. This is what a pty layer
// does, and it is the whole of what one would be needed for here.
static uint32_t crlf(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap) {
    uint32_t k = 0;
    uint8_t prev = 0;
    for (uint32_t i = 0; i < n && k + 2 <= cap; i++) {
        if (in[i] == '\n' && prev != '\r') out[k++] = '\r';
        out[k++] = in[i];
        prev = in[i];
    }
    return k;
}

static uint32_t chan_id;              // the client's number for it
static uint32_t chan_window;          // how much it will take before saying more
static uint32_t our_window;

// Input on its way to the shell, and the one size report still to be told.
//
// Typed bytes wait here while the shell is busy; the SSH window guarantees they
// fit. A resized window is kept as a size and not as bytes: dragging a window
// sends a report for every step, and only the last one matters, so a burst of
// them becomes one line into the shell -- and only between whole pieces of
// input, never in the middle of an arrow key's escape sequence.
static uint8_t  tosh[CHAN_WINDOW];
static uint32_t tosh_n;
static uint32_t size_rows, size_cols;
static bool     size_pending;

// Ctrl-C bytes that ended a command rather than going to the shell. They were
// sent inside the window like any byte, so the window has to be given them back
// even though the shell never took them.
static uint32_t intr_bytes;
static bool     intr_echo;

static void queue_input(const uint8_t *d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        // Ctrl-C is the kernel's to decide, as it is on a console: a command in
        // front is ended and the key is gone; nothing in front, or a program
        // like Atto that took the key for itself, and it is passed on as input.
        if (d[i] == 3 && ubiqos_interrupt(pair[0]) == 1) {
            intr_bytes++;
            intr_echo = true;
            continue;
        }
        if (tosh_n >= sizeof tosh) break;                  // the window forbids it
        tosh[tosh_n++] = d[i];
    }
}

// How long a key takes to be answered: from the moment typed input is handed
// to the shell to the moment the first byte of whatever it says back leaves.
// Kept always -- it costs two numbers per key -- and printed at the end of a
// session by `sshd -d`, because "it feels slow" is where a fix starts, not
// where it is proved.
static uint32_t lat_fed, lat_n, lat_sum, lat_max, st_out, st_iter;

// Give the shell what it has room for, and never wait for it. Returns false if
// the connection went while saying how much room there is again.
static bool feed_shell(void) {
    uint32_t given = 0;
    while (tosh_n) {
        const int32_t k = ubiqos_write_some(pair[0], tosh, tosh_n);
        if (k <= 0) break;
        memmove(tosh, tosh + k, tosh_n - (uint32_t)k);
        tosh_n -= (uint32_t)k;
        given += (uint32_t)k;
    }
    if (!tosh_n && size_pending) {
        char rep_[32];
        const int k = snprintf(rep_, sizeof rep_, "\x1b[8;%lu;%lut",
                               (unsigned long)size_rows, (unsigned long)size_cols);
        // All of it or none: half a report in the shell's input is worse than
        // a late one, and the next pass will try again.
        if (k > 0 && ubiqos_writable(pair[0]) > 0
            && ubiqos_write_some(pair[0], rep_, (uint32_t)k) == k)
            size_pending = false;
    }
    if (given && !lat_fed) lat_fed = ubiqos_ticks_now();
    given += intr_bytes;
    intr_bytes = 0;
    if (!given) return true;

    // The window reopens by exactly what the shell took.
    uint8_t b[16];
    wr_t w = { b, sizeof b, 0, false };
    w_byte(&w, MSG_CHANNEL_WINDOW_ADJUST);
    w_u32(&w, chan_id);
    w_u32(&w, given);
    return send_packet(b, w.n);
}

static bool send_channel_data(const uint8_t *p, uint32_t n) {
    while (n) {
        uint32_t take = n > CHAN_PACKET ? CHAN_PACKET : n;
        while (chan_window < take) {      // it has not opened its window yet
            uint32_t got;
            if (!recv_packet(&got, 30000)) return false;
            rd_t r = { payload, got, 1, false };
            if (payload[0] == MSG_CHANNEL_WINDOW_ADJUST) {
                r_u32(&r);
                chan_window += r_u32(&r);
            } else if (payload[0] == MSG_CHANNEL_DATA) {
                // Typed while we were waiting for room to answer. Queued, not
                // pushed: pushing into a busy shell is what used to stop sshd.
                r_u32(&r);
                uint32_t dl;
                const uint8_t *d = r_str(&r, &dl);
                if (!r.over && dl) queue_input(d, dl);
            } else if (payload[0] == MSG_CHANNEL_CLOSE || payload[0] == MSG_DISCONNECT) {
                return false;
            }
        }
        uint8_t b[CHAN_PACKET + 32];
        wr_t w = { b, sizeof b, 0, false };
        w_byte(&w, MSG_CHANNEL_DATA);
        w_u32(&w, chan_id);
        w_strn(&w, p, take);
        if (w.over || !send_packet(b, w.n)) return false;
        chan_window -= take;
        p += take;
        n -= take;
    }
    return true;
}

static void channel_eof_and_close(void) {
    uint8_t b[64];
    wr_t w = { b, sizeof b, 0, false };
    w_byte(&w, MSG_CHANNEL_REQUEST);
    w_u32(&w, chan_id);
    w_str(&w, "exit-status");
    w_byte(&w, 0);
    w_u32(&w, 0);
    send_packet(b, w.n);

    w.n = 0; w.over = false;
    w_byte(&w, MSG_CHANNEL_EOF);
    w_u32(&w, chan_id);
    send_packet(b, w.n);

    w.n = 0; w.over = false;
    w_byte(&w, MSG_CHANNEL_CLOSE);
    w_u32(&w, chan_id);
    send_packet(b, w.n);
}

// Everything after the login: open a channel, start a shell, carry bytes.
static void do_session(void) {
    bool running = false;
    uint32_t idle = 0;
    for (;;) {
        bool moved = false;
        st_iter++;

        // Anything the shell has said goes out first, so that a prompt appears
        // without waiting for the next keystroke.
        if (running) {
            uint8_t out[512], wire[1024];
            while (ubiqos_readable(pair[0]) > 0) {
                const int32_t got = ubiqos_read(pair[0], out, sizeof out);
                if (got <= 0) break;
                if (lat_fed) {
                    const uint32_t ms = ubiqos_ticks_now() - lat_fed;
                    lat_sum += ms;
                    lat_n++;
                    if (ms > lat_max) lat_max = ms;
                    lat_fed = 0;
                }
                st_out += (uint32_t)got;
                const uint32_t k = crlf(out, (uint32_t)got, wire, sizeof wire);
                if (!send_channel_data(wire, k)) return;
                moved = true;
            }
            if (intr_echo) {
                // What a terminal shows for the key, as the console does.
                intr_echo = false;
                if (!send_channel_data((const uint8_t *)"^C\r\n", 4)) return;
            }
            // Whether the shell is still there costs a look through every
            // process slot, so it is asked when the loop is idle rather than
            // on every pass -- which is now hundreds of times a second.
            if (!moved && ++idle >= 20) {
                idle = 0;
                if (!shell_alive()) { channel_eof_and_close(); return; }
            }
            if (!feed_shell()) return;        // typed input, as the shell has room
        }

        // And then whatever has arrived, if anything has -- WITHOUT WAITING
        // while a shell is running. It used to wait up to twenty milliseconds
        // here for the client, and that wait sat between every key and its
        // answer, and between every 128 bytes of output and the next: the
        // shell fills its pipe, blocks, and was only drained again once sshd
        // had finished waiting for somebody else. The loop now looks and
        // moves on, and sleeps only when nothing at all happened. Measured
        // from the client, sixty keys each: the echo took 51 ms at the median
        // before and 19 ms after.
        uint32_t n;
        if (!recv_packet(&n, running ? 0 : 60000)) {
            if (peer_gone || !running) return;
            if (!moved) ubiqos_sleep(2);
            continue;
        }
        const uint8_t msg = payload[0];
        rd_t r = { payload, n, 1, false };

        switch (msg) {
        case MSG_CHANNEL_OPEN: {
            uint32_t tl;
            const uint8_t *type = r_str(&r, &tl);
            const uint32_t sender = r_u32(&r);
            chan_window = r_u32(&r);
            r_u32(&r);                        // their maximum packet
            uint8_t b[64];
            wr_t w = { b, sizeof b, 0, false };
            if (!r_is(type, tl, "session")) {
                w_byte(&w, MSG_CHANNEL_OPEN_FAILURE);
                w_u32(&w, sender);
                w_u32(&w, 3);                 // unknown channel type
                w_str(&w, "only a session");
                w_str(&w, "");
                send_packet(b, w.n);
                break;
            }
            chan_id = sender;
            our_window = CHAN_WINDOW;
            w_byte(&w, MSG_CHANNEL_OPEN_CONFIRMATION);
            w_u32(&w, sender);
            w_u32(&w, 0);                     // our number for it
            w_u32(&w, our_window);
            w_u32(&w, CHAN_PACKET);
            if (!send_packet(b, w.n)) return;
            break;
        }
        case MSG_CHANNEL_REQUEST: {
            r_u32(&r);                        // our channel number
            uint32_t tl;
            const uint8_t *type = r_str(&r, &tl);
            const uint8_t want_reply = r_byte(&r);
            bool ok = false;
            if (r_is(type, tl, "pty-req")) ok = true;
            else if (r_is(type, tl, "window-change")) {
                // The window at the other end was resized. There is no signal
                // to send and no terminal driver to tell, so it goes to the
                // shell's side IN BAND, as exactly what a terminal says when it
                // is asked its size: ESC [ 8 ; rows ; cols t. The shell takes
                // its width from it, curses turns it into KEY_RESIZE and Atto
                // redraws, and the prompts that keep secrets skip it like any
                // other escape sequence -- see ubiqos_esc_skip.
                const uint32_t cols = r_u32(&r);
                const uint32_t rows = r_u32(&r);
                if (running && !r.over && cols >= 20 && rows >= 4
                    && cols <= 1000 && rows <= 1000) {
                    // Remembered, not written: the next pass of the loop tells
                    // the shell, once, whatever the size is by then.
                    size_rows = rows;
                    size_cols = cols;
                    size_pending = true;
                }
                ok = true;                    // and no reply: it never wants one
            }
            else if (r_is(type, tl, "shell")) {
                ok = start_shell();
                running = ok;
                if (ok) say("sshd: a shell for the session\r\n");
            }
            if (want_reply) {
                uint8_t b[16];
                wr_t w = { b, sizeof b, 0, false };
                w_byte(&w, ok ? MSG_CHANNEL_SUCCESS : MSG_CHANNEL_FAILURE);
                w_u32(&w, chan_id);
                if (!send_packet(b, w.n)) return;
            }
            break;
        }
        case MSG_CHANNEL_DATA: {
            r_u32(&r);
            uint32_t dl;
            const uint8_t *d = r_str(&r, &dl);
            if (r.over) break;
            if (running && dl) {
                queue_input(d, dl);
                if (!feed_shell()) return;   // now, not on the next pass
            }
            break;
        }
        case MSG_CHANNEL_WINDOW_ADJUST:
            r_u32(&r);
            chan_window += r_u32(&r);
            break;
        case MSG_CHANNEL_EOF:
        case MSG_CHANNEL_CLOSE:
            return;
        case MSG_DISCONNECT:
            return;
        case MSG_GLOBAL_REQUEST: {
            uint32_t tl;
            r_str(&r, &tl);
            if (r_byte(&r)) {                 // it wants an answer, and gets no
                const uint8_t no = MSG_UNIMPLEMENTED;
                uint8_t b[8];
                wr_t w = { b, sizeof b, 0, false };
                w_byte(&w, no);
                w_u32(&w, seq_in - 1);
                send_packet(b, w.n);
            }
            break;
        }
        default:
            break;
        }
    }
}

// --- ONE CONNECTION, FROM HELLO TO GOODBYE ----------------------------------

static void serve(void) {
    seq_in = seq_out = 0;
    encrypted = false;
    have_session_id = false;
    shell_pid = -1;
    tosh_n = 0;
    size_pending = false;
    intr_bytes = 0;
    intr_echo = false;
    // Per connection, every one of them. This one was not, and a client that
    // had hung up took the NEXT session with it: the shell started, its banner
    // went out, and the first idle poll read a flag left over from somebody
    // else and closed a session nobody had left.
    peer_gone = false;

    // The version line. Both sides send theirs without waiting for the other's.
    char hello[64];
    const int hn = snprintf(hello, sizeof hello, "%s\r\n", VERSION);
    if (!write_all((const uint8_t *)hello, (uint32_t)hn)) return;

    char theirs[256];
    uint32_t tn = 0;
    for (;;) {
        uint8_t c;
        if (!read_exact(&c, 1, 10000)) return;
        if (c == '\n') break;
        if (c != '\r' && tn < sizeof theirs - 1) theirs[tn++] = (char)c;
    }
    theirs[tn] = 0;
    if (strncmp(theirs, "SSH-2.0", 7) != 0) { say("sshd: not SSH-2.0\r\n"); return; }

    // KEXINIT both ways. Ours has to be kept: the exchange hash covers both.
    static uint8_t ours[512], client_kexinit[2048];
    wr_t w = { ours, sizeof ours, 0, false };
    kexinit_payload(&w);
    if (w.over || !send_packet(ours, w.n)) return;

    uint32_t n;
    if (!recv_packet(&n, 10000) || payload[0] != MSG_KEXINIT) {
        say("sshd: no KEXINIT from the client\r\n");
        return;
    }
    if (n > sizeof client_kexinit) return;
    memcpy(client_kexinit, payload, n);
    if (!client_agrees(client_kexinit, n)) { disconnect(3, "no algorithm in common"); return; }

    if (!do_kex(theirs, client_kexinit, n, ours, w.n)) {
        say("sshd: the key exchange failed\r\n");
        return;
    }
    say("sshd: keys agreed\r\n");

    if (!do_userauth()) return;
    lat_fed = lat_n = lat_sum = lat_max = st_out = st_iter = 0;
    do_session();
    if (debugging) {
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "sshd: ");
        ubiqos_line_u32(&l, lat_n);
        ubiqos_line_str(&l, " keys answered, average ");
        ubiqos_line_u32(&l, lat_n ? lat_sum / lat_n : 0);
        ubiqos_line_str(&l, " ms, worst ");
        ubiqos_line_u32(&l, lat_max);
        ubiqos_line_str(&l, " ms; ");
        ubiqos_line_u32(&l, st_out);
        ubiqos_line_str(&l, " bytes out in ");
        ubiqos_line_u32(&l, st_iter);
        ubiqos_line_str(&l, " passes\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }

    if (shell_pid >= 0) {
        // Closing our end is how the shell is asked to leave: its input has no
        // writer left, which reads as the end of the file.
        // Everything the session started goes with it -- see ubiqos_hangup.
        // Killing the shell alone left an Atto from a closed window running.
        ubiqos_hangup(pair[0]);
        ubiqos_close(pair[0]);
    }
    if (encrypted) {
        mbedtls_gcm_free(&gcm_in);
        mbedtls_gcm_free(&gcm_out);
    }
}

int main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: sshd [port]\n\n"
            "A shell over SSH: curve25519-sha256, an ecdsa-sha2-nistp256 host\n"
            "key, aes256-gcm, and a password checked against the key store.\n\n"
            "The store must be unlocked: the host key is kept there, and so is\n"
            "the password, under 'ssh.password' -- set it with 'key set\n"
            "ssh.password' before the first connection. The host key is made on\n"
            "the first run and kept, so a client's memory of it stays right.\n\n"
            "Ed25519 is not available -- mbedTLS has no Edwards curves -- so a\n"
            "client that offers only ed25519 host keys is told to allow ecdsa.\n\n"
            "This is not an audited SSH server. Your own network, not the\n"
            "internet.\n"))
        return 0;

    uint32_t port = DEFAULT_PORT;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == 'd' && !argv[i][2]) debugging = true;
    if (argc > 1) {
        uint32_t v = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
        if (v) port = v;
    }

    bool testing = false;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == 't' && !argv[i][2]) { testing = true; debugging = true; }

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_add_source(&entropy, trng_source, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    static const char pers[] = "ubiqos sshd";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, sizeof pers - 1) != 0) {
        say("sshd: no entropy\r\n");
        return 0;
    }

    if (testing) { crypto_selftest(); return 0; }

    if (!host_key_load()) return 1;
    host_key_selftest();

    int32_t server = ubiqos_sock_listen_on(UBIQOS_NET_LWIP, (uint16_t)port);
    for (uint32_t waited = 0; server < 0 && waited < NET_WAIT_S; waited++) {
        ubiqos_sleep(1000);
        server = ubiqos_sock_listen_on(UBIQOS_NET_LWIP, (uint16_t)port);
    }
    if (server < 0) {
        {
            // The stack knows why and has always known: 10 is a full socket
            // table, 20 and up are lwIP's own bind errors -- 28 is the port
            // already being in use. Printing "would not" and stopping there
            // sent two people looking in the wrong place, twice.
            const int32_t why = ubiqos_syscall(SYS_MEMINFO, UBIQOS_MEM_SOCKETS, 102, 0);
            ubiqos_line_t l;
            ubiqos_line_reset(&l);
            ubiqos_line_str(&l, "sshd: the network stack would not take port ");
            ubiqos_line_u32(&l, port);
            ubiqos_line_str(&l, why == 10 ? " -- no free socket" :
                                why == 28 ? " -- something else is on it" : " -- reason ");
            if (why != 10 && why != 28) ubiqos_line_u32(&l, (uint32_t)why);
            ubiqos_line_str(&l, "\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
        }
        return 1;
    }

    say_num("sshd: listening on port", (int32_t)port);

    for (;;) {
        const int32_t c = ubiqos_sock_accept(server);
        if (c < 0) { ubiqos_sleep(100); continue; }
        sock = c;
        say("sshd: a client\r\n");
        serve();
        ubiqos_sock_close(sock);
        say("sshd: gone\r\n");
    }
}
