#include "../../common/ubiqos_abi.h"

// udp -- send a datagram, or listen for them.
//
//   udp send 192.168.7.2 9000 hello there
//   udp send fe80::1%cable 9000 hello  (a link-local address names its link)
//   udp listen 9000
//   udp echo 9000
//
// The first program to use datagram sockets, and so the test of them: `listen`
// prints each datagram with who sent it, `echo` sends each one back where it
// came from -- which, for a link-local IPv6 sender, means out of the interface
// it came in on, carried in the address's zone.

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

static bool is(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static uint32_t to_u32(const char *s) {
    uint32_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10u + (uint32_t)(*s - '0');
    return v;
}

static void hex16(ubiqos_line_t *l, uint32_t v) {
    char t[5];
    int n = 0;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const uint32_t d = (v >> shift) & 0xfu;
        if (!n && !d && shift) continue;              // no leading zeros
        t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    t[n] = 0;
    ubiqos_line_str(l, t);
}

// As RFC 5952 writes an IPv6 address: lower case, no leading zeros, and the
// longest run of two or more zero groups as "::".
static void put_addr(ubiqos_line_t *l, const ubiqos_sockaddr_t *a) {
    if (a->family == UBIQOS_AF_INET) {
        for (int i = 0; i < 4; i++) {
            ubiqos_line_u32(l, a->addr[i]);
            if (i < 3) ubiqos_line_str(l, ".");
        }
        return;
    }
    uint32_t g[8];
    for (int i = 0; i < 8; i++) g[i] = ((uint32_t)a->addr[2 * i] << 8) | a->addr[2 * i + 1];
    int best = -1, bestlen = 1;
    for (int i = 0; i < 8; ) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > bestlen) { best = i; bestlen = j - i; }
        i = j;
    }
    for (int i = 0; i < 8; i++) {
        if (i == best) { ubiqos_line_str(l, "::"); i += bestlen - 1; continue; }
        if (i && i != best + bestlen) ubiqos_line_str(l, ":");
        hex16(l, g[i]);
    }
    if (a->zone) { ubiqos_line_str(l, "%"); ubiqos_line_u32(l, a->zone); }
}

static int32_t open_or_say(uint16_t port) {
    const int32_t s = ubiqos_udp_open(UBIQOS_NET_LWIP, port);
    if (s < 0) say("udp: the network stack would not give a socket -- is lwIP up?\r\n");
    return s;
}

static void do_send(const char *host, uint16_t port, int argc, char **argv) {
    ubiqos_sockaddr_t to;
    int32_t rc;
    for (int waited = 0; (rc = ubiqos_net_addr(UBIQOS_NET_LWIP, host, port, &to)) == 1; waited++) {
        if (waited >= 500) break;                     // five seconds of asking
        ubiqos_sleep(10);
    }
    if (rc != 0) { say("udp: nobody answers to that name\r\n"); return; }

    uint8_t text[256];
    uint32_t n = 0;
    for (int i = 0; i < argc; i++) {
        for (const char *p = argv[i]; *p && n < sizeof text - 1; p++) text[n++] = (uint8_t)*p;
        if (i < argc - 1 && n < sizeof text - 1) text[n++] = ' ';
    }

    const int32_t s = open_or_say(0);
    if (s < 0) return;
    int32_t sent = 0;
    for (int tries = 0; (sent = ubiqos_udp_sendto(s, &to, text, n)) == 0 && n && tries < 100; tries++)
        ubiqos_sleep(10);
    ubiqos_sock_close(s);

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    if (sent < 0 || (n && !sent)) {
        ubiqos_line_str(&l, "udp: it would not go to ");
    } else {
        ubiqos_line_u32(&l, (uint32_t)sent);
        ubiqos_line_str(&l, " bytes to ");
    }
    put_addr(&l, &to);
    ubiqos_line_str(&l, " port ");
    ubiqos_line_u32(&l, port);
    ubiqos_line_str(&l, "\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

static void do_listen(uint16_t port, bool echo) {
    const int32_t s = open_or_say(port);
    if (s < 0) return;

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, echo ? "udp: sending back what comes to port " : "udp: listening on port ");
    ubiqos_line_u32(&l, port);
    ubiqos_line_str(&l, ", IPv4 and IPv6; Ctrl-C stops\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);

    // On the stack, not static: a module's static data comes out of the same
    // pool as its stack, on top of the 4 kB stack it gets anyway, and on the
    // Feather F405 that pool has about 9 kB left once sshd and httpd run.
    uint8_t buf[1500];
    for (;;) {
        ubiqos_sockaddr_t from;
        from.port = 0;
        const int32_t n = ubiqos_udp_recvfrom(s, &from, buf, sizeof buf);
        if (n < 0) { say("udp: the socket has gone\r\n"); return; }
        if (!n && !from.port) { ubiqos_sleep(10); continue; }

        if (echo) {
            for (int tries = 0; ubiqos_udp_sendto(s, &from, buf, (uint32_t)n) == 0 && tries < 100; tries++)
                ubiqos_sleep(10);
        }

        ubiqos_line_reset(&l);
        put_addr(&l, &from);
        ubiqos_line_str(&l, " port ");
        ubiqos_line_u32(&l, from.port);
        ubiqos_line_str(&l, ", ");
        ubiqos_line_u32(&l, (uint32_t)n);
        ubiqos_line_str(&l, " bytes: ");
        // The text as far as it is printable, so a datagram of binary does not
        // put the terminal in a state of its own.
        char t[2] = { 0, 0 };
        // Thirty-two of it: a line is 96 characters, and an IPv6 address with
        // its port and count has taken half of them already.
        for (int32_t i = 0; i < n && i < 32; i++) {
            t[0] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
            ubiqos_line_str(&l, t);
        }
        if (n > 32) ubiqos_line_str(&l, "...");
        ubiqos_line_str(&l, "\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: udp send HOST PORT TEXT...\n"
            "       udp listen PORT\n"
            "       udp echo PORT\n\n"
            "send     one datagram with TEXT in it, to an address or a name\n"
            "listen   print each datagram that comes to PORT, and who sent it\n"
            "echo     the same, and send each one back where it came from\n\n"
            "Over IPv4 and, where the board has it, IPv6. Ctrl-C stops listening.\n")) return;

    if (argc >= 5 && is(argv[1], "send")) {
        const uint32_t port = to_u32(argv[3]);
        if (!port || port > 65535u) { say("udp: no such port\r\n"); return; }
        do_send(argv[2], (uint16_t)port, argc - 4, argv + 4);
        return;
    }
    if (argc == 3 && (is(argv[1], "listen") || is(argv[1], "echo"))) {
        const uint32_t port = to_u32(argv[2]);
        if (!port || port > 65535u) { say("udp: no such port\r\n"); return; }
        do_listen((uint16_t)port, is(argv[1], "echo"));
        return;
    }
    say("usage: udp send HOST PORT TEXT... | udp listen PORT | udp echo PORT\r\n");
}
