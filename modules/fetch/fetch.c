#include <string.h>
#include "../../common/ubiqos_abi.h"
#include "ubiqos_tls.h"

// fetch -- ask a web server for something and print what comes back.
//
//   fetch https://example.com/
//   fetch http://192.168.68.1/status
//   fetch example.com /            (the old form: host, path, port)
//
// It began as the proof of ubiqos_sock_connect, the first outbound connection
// this machine could make, and https is the reason it grew: with TLS the
// board can talk to the internet's APIs, which answer nothing else. The
// server's certificate is checked against the built-in roots and the host
// name -- see lib/tls -- and a connection that fails that is not made.
//
// Still not an HTTP client: no redirects, no chunked decoding. It prints the
// headers and the body as they arrive and stops when the server closes.

UBIQOS_MEM_SIZE(32768);                  // mbedTLS's big-number work is on the stack
// Records, certificates, contexts. 77.5 kB at most, measured against
// example.com, 1.1.1.1, google.com and github.com; the rest is for a server
// with a longer chain. One piece, so on a board without PSRAM it is SRAM.
uint32_t ubiqos_heap_bytes = 128u * 1024u;

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }
static void err(const char *s) { ubiqos_write_str(UBIQOS_STDERR, s); }

#define SPIN_MS      10
#define CONNECT_WAIT 500      // x SPIN_MS = 5 s, longer than any DNS lookup here
#define IDLE_MS      5000     // nothing arriving for this long ends it

static bool starts(const char *s, const char *pre) {
    while (*pre) { if (*s++ != *pre++) return false; }
    return true;
}

static uint16_t to_port(const char *p, const char *end) {
    uint32_t v = 0;
    for (; p < end && *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
    return (v && v < 65536u) ? (uint16_t)v : 0;
}

// GET, HTTP/1.0 so that the server closes when it is done, and a User-Agent
// because some servers refuse a request without one.
static uint32_t build_request(char *out, uint32_t cap, const char *host, const char *path) {
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nUser-Agent: UbiqOS-fetch\r\nAccept: */*\r\n"
                            "Connection: close\r\n\r\n" };
    uint32_t n = 0;
    for (uint32_t k = 0; k < sizeof parts / sizeof parts[0]; k++)
        for (const char *c = parts[k]; *c; c++) {
            if (n >= cap) return 0;
            out[n++] = *c;
        }
    return n;
}

// Where what arrives goes. By default all of it to stdout, headers and body.
// With -o FILE only the body, into FILE: the headers are read past up to the
// blank line that ends them, and the status line is said on stderr. That is
// what getting a file onto a board without a card needs -- the ESP32-C6's
// firmware onto iLabs' Challenger+, over the USB cable, for espflash to write.
static int32_t out_fd = -1;
static uint32_t head_state;        // how much of "\r\n\r\n" has been seen
static bool in_body;
static uint32_t body_bytes;
static char status[64];
static uint32_t status_len;
static bool status_done;

static void emit(const uint8_t *buf, uint32_t n) {
    if (out_fd < 0) { ubiqos_write(UBIQOS_STDOUT, buf, n); return; }
    uint32_t i = 0;
    while (!in_body && i < n) {
        const char c = (char)buf[i++];
        if (!status_done) {
            if (c == '\r' || c == '\n') status_done = true;
            else if (status_len < sizeof status - 1) status[status_len++] = c;
        }
        static const char end[] = "\r\n\r\n";
        head_state = c == end[head_state] ? head_state + 1 : (c == '\r' ? 1u : 0u);
        if (head_state == 4) in_body = true;
    }
    if (i < n) {
        ubiqos_write(out_fd, buf + i, n - i);
        body_bytes += n - i;
    }
}

static void fetch_tls(const char *host, uint16_t port, const char *req, uint32_t len) {
    char why[200];
    ubiqos_tls_t *t = ubiqos_tls_open(host, port, why, sizeof why);
    if (!t) { err("fetch: "); err(why); err("\r\n"); return; }
    if (ubiqos_tls_write(t, req, len) < 0) {
        err("fetch: the request could not be sent\r\n");
        ubiqos_tls_close(t);
        return;
    }
    static uint8_t buf[1024];
    for (;;) {
        const int32_t n = ubiqos_tls_read(t, buf, sizeof buf, IDLE_MS);
        if (n > 0) { emit(buf, (uint32_t)n); continue; }
        if (n == -2) err("\r\nfetch: nothing more arrived\r\n");
        if (n == -1) err("\r\nfetch: the connection failed\r\n");
        break;
    }
    err("\r\nfetch: TLS 1.2, ");
    err(ubiqos_tls_cipher(t));
    err(", certificate checked\r\n");
    ubiqos_tls_close(t);
}

static void fetch_plain(const char *host, uint16_t port, const char *req, uint32_t len) {
    int32_t sock = ubiqos_sock_connect(UBIQOS_NET_LWIP, host, port);
    if (sock < 0) { err("fetch: the stack would not start a connection\r\n"); return; }

    // SYN_SENT covers the name lookup as well as the handshake, so one wait
    // serves both and the caller never has to know which it is in.
    int32_t st = 0;
    for (uint32_t i = 0; i < CONNECT_WAIT; i++) {
        st = ubiqos_sock_state(sock);
        if (st != UBIQOS_TCP_SYN_SENT) break;
        ubiqos_sleep(SPIN_MS);
    }
    if (st != UBIQOS_TCP_ESTABLISHED) {
        err(st == UBIQOS_TCP_SYN_SENT ? "fetch: gave up waiting\r\n"
                                      : "fetch: could not connect\r\n");
        ubiqos_sock_close(sock);
        return;
    }

    uint32_t done = 0;
    while (done < len) {
        int32_t n = ubiqos_sock_send(sock, (const uint8_t *)req + done, len - done);
        if (n < 0) { err("fetch: the send failed\r\n"); ubiqos_sock_close(sock); return; }
        if (n == 0) { ubiqos_sleep(SPIN_MS); continue; }   // no room; ask again
        done += (uint32_t)n;
    }

    // Until the server closes, which is what HTTP/1.0 asks it to do.
    uint8_t buf[256];
    uint32_t quiet = 0;
    for (;;) {
        int32_t n = ubiqos_sock_recv(sock, buf, sizeof buf);
        if (n < 0) break;                      // the peer has gone: a clean end
        if (n == 0) {
            if ((quiet += SPIN_MS) > IDLE_MS) { err("\r\nfetch: nothing more arrived\r\n"); break; }
            ubiqos_sleep(SPIN_MS);
            continue;
        }
        quiet = 0;
        emit(buf, (uint32_t)n);
    }
    ubiqos_sock_close(sock);
}

// main and not module_main: a NEWLIB module's module_main is the C library's,
// which sets up stdio and the heap and then calls this.
int main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: fetch [-o FILE] URL\n"
            "       fetch HOST [PATH] [PORT]\n\n"
            "An HTTP GET, printed as it arrives: headers, then body -- or with -o,\n"
            "the body alone written to FILE and the status line said. A URL may be\n"
            "http:// or https://. For https the server's certificate is checked\n"
            "against the roots built in -- and any in /sd/certs.pem -- and against\n"
            "the host name, and the wall clock must be set, which NTP does.\n"))
        return 0;
    const char *out_name = 0;
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'o' && !argv[1][2]) {
        out_name = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) { say("usage: fetch [-o FILE] URL, or fetch HOST [PATH] [PORT]\r\n"); return 1; }

    // Host and path are copied out of the URL: the host needs its own NUL.
    static char host[128];
    const char *path = "/";
    uint16_t port = 80;
    bool tls = false;

    const char *u = argv[1];
    if (starts(u, "https://") || starts(u, "http://")) {
        tls = starts(u, "https://");
        port = tls ? 443 : 80;
        u += tls ? 8 : 7;
        const char *slash = strchr(u, '/');
        const char *end = slash ? slash : u + strlen(u);
        const char *colon = memchr(u, ':', (size_t)(end - u));
        const char *hend = colon ? colon : end;
        if (hend == u || (uint32_t)(hend - u) >= sizeof host) { say("fetch: that is not a host\r\n"); return 1; }
        memcpy(host, u, (size_t)(hend - u));
        host[hend - u] = 0;
        if (colon && !(port = to_port(colon + 1, end))) { say("fetch: that is not a port\r\n"); return 1; }
        if (slash) path = slash;
    } else {
        if (strlen(u) >= sizeof host) { say("fetch: that is not a host\r\n"); return 1; }
        strcpy(host, u);
        if (argc > 2) path = argv[2];
        if (argc > 3 && !(port = to_port(argv[3], argv[3] + strlen(argv[3])))) {
            say("fetch: that is not a port\r\n"); return 1;
        }
    }

    static char req[512];
    const uint32_t len = build_request(req, sizeof req, host, path);
    if (!len) { say("fetch: that host and path are too long\r\n"); return 1; }

    if (out_name) {
        out_fd = ubiqos_open_flags(out_name, UBIQOS_O_WRONLY | UBIQOS_O_CREAT | UBIQOS_O_TRUNC);
        if (out_fd < 0) { err("fetch: cannot write "); err(out_name); err("\r\n"); return 1; }
    }
    if (tls) fetch_tls(host, port, req, len);
    else     fetch_plain(host, port, req, len);
    if (out_fd >= 0) {
        ubiqos_close(out_fd);
        status[status_len] = 0;
        err("fetch: "); err(status_len ? status : "no status line"); err("; ");
        char num[12]; uint32_t v = body_bytes; int k = 11; num[k] = 0;
        do { num[--k] = (char)('0' + v % 10u); v /= 10u; } while (v);
        err(&num[k]); err(" bytes to "); err(out_name); err("\r\n");
    }
    return 0;
}
