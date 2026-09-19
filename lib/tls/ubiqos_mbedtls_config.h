// mbedTLS, as a TLS 1.2 client and nothing else.
//
// This replaces mbedtls_config.h entirely (MBEDTLS_CONFIG_FILE), so what is not
// named here is not built. The choice follows what servers on the internet
// actually offer a client in 2026: ECDHE for the key, certificates signed with
// ECDSA or RSA, and AES-GCM or ChaCha20-Poly1305 for the traffic.
//
// TLS 1.3 is not here yet. In mbedTLS 3.6 it needs the PSA crypto core, which
// is another large piece; servers still offer 1.2 beside 1.3, and 1.3 can come
// when one stops.

#ifndef UBIQOS_MBEDTLS_CONFIG_H
#define UBIQOS_MBEDTLS_CONFIG_H

// --- the machine ------------------------------------------------------------

// No MBEDTLS_HAVE_ASM. Its bignum inner loops are inline assembly that picks
// registers for itself, and an Arm module has r9 reserved (-ffixed-r9) for its
// data pointer: a loop that borrowed it would corrupt every global after it.
// Plain C is slower, and a handshake is still well under a second or two.

// Time: the certificate dates are compared with the wall clock, which SNTP
// sets, and the millisecond clock is the kernel's tick. Both are supplied by
// lib/tls/ubiqos_tls.c -- newlib's time() here counts from boot.
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_TIME_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT

// Entropy: no /dev/urandom here. The RP2350's TRNG is added as a source by
// ubiqos_tls.c.
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// --- hashing and ciphers ----------------------------------------------------

#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C            // still in some chains' older signatures
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C          // and the entropy pool's own hash
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C

// --- public keys ------------------------------------------------------------

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

// --- certificates -----------------------------------------------------------

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_BASE64_C          // PEM, for roots read from the card
#define MBEDTLS_PEM_PARSE_C

// --- TLS --------------------------------------------------------------------

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_ALPN

// A server may send a full 16 kB record, and almost none honour a request for
// smaller ones, so the incoming buffer is the full size. It lives in PSRAM,
// through newlib's malloc. What this end sends is small.
#define MBEDTLS_SSL_IN_CONTENT_LEN   16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096

// Error strings, so that a failed handshake says why.
#define MBEDTLS_ERROR_C

#endif
