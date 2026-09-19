// The layout of the tlsroots data module: the root certificates a TLS client
// trusts, one DER certificate after another, behind a small header that says
// what it is. Written by tools/make_roots.py; read by lib/tls/ubiqos_tls.c
// through ubiqos_data_link.

#ifndef UBIQOS_TLS_ROOTS_H
#define UBIQOS_TLS_ROOTS_H

#include <stdint.h>

#define UBIQOS_TLS_ROOTS_MODULE "tlsroots"
#define UBIQOS_TLS_ROOTS_MAGIC  "UBQROOTS"     // eight characters, no NUL kept

typedef struct {
    char     magic[8];
    uint32_t count;      // certificates
    uint32_t length;     // bytes of DER that follow
    uint8_t  der[];
} ubiqos_tls_roots_t;

#endif
