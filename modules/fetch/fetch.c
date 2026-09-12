#include "../../common/myrtos_abi.h"

// fetch -- ask an HTTP server for something and print what comes back.
//
//   fetch example.com /
//   fetch 192.168.68.1 / 80
//
// This exists to prove myrtos_sock_connect, which is the first outbound
// connection this machine has ever been able to make. Everything the network
// did before answered somebody else: httpd listens, ping is ICMP, browse is
// multicast. None of them opens a connection.
//
// Plain HTTP and nothing else. No TLS, no redirects, no chunked decoding, no
// Content-Length: it prints the bytes as they arrive and stops when the server
// closes. That is enough to say the connection works, and deliberately not a
// line more -- an HTTP client that looks finished invites being used as one.

static void say(const char *s) { myrtos_write_str(MYRTOS_STDOUT, s); }

// The stack cannot wait, so this process does the waiting -- the same shape as
// ping, and the right way round: it has nothing else to do.
#define SPIN_MS      10
#define CONNECT_WAIT 500      // x SPIN_MS = 5 s, longer than any DNS lookup here
#define IDLE_WAIT    300      // x SPIN_MS = 3 s with nothing arriving

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
                    "fetch HOST [PATH] [PORT] -- an HTTP GET, printed as it arrives"))
        return;
    if (argc < 2) { say("usage: fetch HOST [PATH] [PORT]\r\n"); return; }

    const char *host = argv[1];
    const char *path = (argc > 2) ? argv[2] : "/";
    uint16_t    port = 80;
    if (argc > 3) {
        uint32_t v = 0;
        for (const char *p = argv[3]; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
        if (v && v < 65536u) port = (uint16_t)v;
    }

    int32_t sock = myrtos_sock_connect(MYRTOS_NET_LWIP, host, port);
    if (sock < 0) { say("fetch: the stack would not start a connection\r\n"); return; }

    // SYN_SENT covers the name lookup as well as the handshake, so one wait
    // serves both and the caller never has to know which it is in.
    int32_t st = 0;
    for (uint32_t i = 0; i < CONNECT_WAIT; i++) {
        st = myrtos_sock_state(sock);
        if (st != MYRTOS_TCP_SYN_SENT) break;
        myrtos_sleep(SPIN_MS);
    }
    if (st != MYRTOS_TCP_ESTABLISHED) {
        say(st == MYRTOS_TCP_SYN_SENT ? "fetch: gave up waiting\r\n"
                                      : "fetch: could not connect\r\n");
        myrtos_sock_close(sock);
        return;
    }

    // Built here rather than in a myrtos_line_t: that holds 96 bytes, and a
    // host and a path together can want more than that. Truncating a request
    // silently would produce a puzzling reply rather than an error.
    char req[256];
    uint32_t want = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\n\r\n" };
    for (uint32_t k = 0; k < sizeof parts / sizeof parts[0]; k++) {
        for (const char *c = parts[k]; *c; c++) {
            if (want >= sizeof req) { say("fetch: that host and path are too long\r\n");
                                      myrtos_sock_close(sock); return; }
            req[want++] = *c;
        }
    }

    uint32_t done = 0;
    while (done < want) {
        int32_t n = myrtos_sock_send(sock, (const uint8_t *)req + done, want - done);
        if (n < 0) { say("fetch: the send failed\r\n"); myrtos_sock_close(sock); return; }
        if (n == 0) { myrtos_sleep(SPIN_MS); continue; }   // no room; ask again
        done += (uint32_t)n;
    }

    // Until the server closes, which is what Connection: close asks it to do.
    uint8_t buf[256];
    uint32_t quiet = 0;
    for (;;) {
        int32_t n = myrtos_sock_recv(sock, buf, sizeof buf - 1);
        if (n < 0) break;                      // the peer has gone: a clean end
        if (n == 0) {
            if (++quiet > IDLE_WAIT) { say("\r\nfetch: nothing more arrived\r\n"); break; }
            myrtos_sleep(SPIN_MS);
            continue;
        }
        quiet = 0;
        buf[n] = 0;
        myrtos_write(MYRTOS_STDOUT, buf, (uint32_t)n);
    }
    myrtos_sock_close(sock);
}
