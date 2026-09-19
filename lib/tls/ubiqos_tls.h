// A TLS client connection over UbiqOS's own sockets, for NEWLIB modules.
//
//   ubiqos_tls_t *t = ubiqos_tls_open("example.com", 443, err, sizeof err);
//   ubiqos_tls_write(t, request, len);
//   while ((n = ubiqos_tls_read(t, buf, sizeof buf, 5000)) > 0) ...
//   ubiqos_tls_close(t);
//
// The server's certificate is checked against the roots in the tlsroots data
// module (a newer revision on the card replaces the one in flash), plus any in
// /sd/certs.pem, and against the host name. A connection that fails that
// check is not opened at all: there is no way to ask for one anyway, because
// the one time that matters is the time somebody would.
//
// Link it with ubiqos_module_use_tls(name) in CMake; the module must be NEWLIB.

#ifndef UBIQOS_TLS_H
#define UBIQOS_TLS_H

#include <stddef.h>
#include <stdint.h>

typedef struct ubiqos_tls ubiqos_tls_t;

// Resolve, connect and complete the handshake, or say why not in err.
ubiqos_tls_t *ubiqos_tls_open(const char *host, uint16_t port, char *err, size_t errcap);

// All of it, or -1.
int32_t ubiqos_tls_write(ubiqos_tls_t *t, const void *buf, uint32_t len);

// Bytes read; 0 when the server has closed; -1 on an error; -2 when nothing
// arrived within timeout_ms.
int32_t ubiqos_tls_read(ubiqos_tls_t *t, void *buf, uint32_t len, uint32_t timeout_ms);

// The cipher suite in use, for a program that wants to say so.
const char *ubiqos_tls_cipher(ubiqos_tls_t *t);

// Tells the server, closes the socket, frees everything. NULL is allowed.
void ubiqos_tls_close(ubiqos_tls_t *t);

#endif
