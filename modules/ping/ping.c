#include "../../common/ubiqos_abi.h"

// ping -- is it there, and how far away.
//
//   ping 192.168.68.1
//   ping fruit-jam.local
//
// A name ending in .local is asked for by multicast, so anything on the network
// that answers to a name can be reached by it without a DNS server anywhere.
// The board has been answering such questions since the 9th; asking them is
// newer.
//
// The stack cannot wait -- it lives in the USB task, which drives the console
// and the network on the same turn -- so a ping is started and then polled, and
// the waiting happens here. That is the right way round: this process has
// nothing else to do.

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

static bool is(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static void addr_str(ubiqos_line_t *l, uint32_t a) {
    for (int i = 0; i < 4; i++) {
        ubiqos_line_u32(l, (a >> (i * 8)) & 0xffu);   // little-endian, as lwIP keeps it
        if (i < 3) ubiqos_line_str(l, ".");
    }
}

// Microseconds as milliseconds with one decimal, because a ping over USB is
// two milliseconds and over the air is eighty: a whole number would round the
// interesting one to nothing.
static void ms_str(ubiqos_line_t *l, uint32_t us) {
    ubiqos_line_u32(l, us / 1000u);
    ubiqos_line_str(l, ".");
    ubiqos_line_u32(l, (us % 1000u) / 100u);
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: ping HOST [count]\n\n"
            "HOST is an address or a name.\n\n"
            "  a name ending in .local, or with no dot at all, is asked for by\n"
            "  multicast on every interface, so no server is needed\n"
            "  anything else is asked of the DNS servers the router gave us\n\n"
            "Traffic goes out over the WiFi when it has an address and a router,\n"
            "and over the USB link otherwise. Ctrl-C stops it.\n")) return;

    if (argc < 2) { say("usage: ping HOST [count]\r\n"); return; }

    uint32_t want = 4;
    if (argc > 2) {
        want = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) want = want * 10 + (uint32_t)(*p - '0');
        if (!want) want = 4;
    }

    uint32_t sent = 0, got = 0, total_us = 0, worst = 0, best = 0xffffffffu;

    for (uint32_t n = 0; n < want; n++) {
        if (n) ubiqos_sleep(1000);                    // one a second, as ping does

        if (ubiqos_ping(argv[1]) < 0) {
            say("ping: the network stack would not take it -- is lwIP up?\r\n");
            return;
        }
        sent++;

        uint32_t st[3] = { 0, 0, 0 };
        for (int waited = 0; waited < 300; waited++) {
            ubiqos_ping_state(st);
            if (st[0] != UBIQOS_PING_RESOLVING && st[0] != UBIQOS_PING_WAITING) break;
            ubiqos_sleep(10);
        }

        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        switch (st[0]) {
        case UBIQOS_PING_REPLIED:
            got++;
            total_us += st[2];
            if (st[2] > worst) worst = st[2];
            if (st[2] < best) best = st[2];
            ubiqos_line_str(&l, "reply from ");
            addr_str(&l, st[1]);
            ubiqos_line_str(&l, "  ");
            ms_str(&l, st[2]);
            ubiqos_line_str(&l, " ms\r\n");
            break;
        case UBIQOS_PING_NONAME:
            ubiqos_line_str(&l, "ping: nobody answers to that name\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            return;                                   // no point asking again
        case UBIQOS_PING_UNREACHABLE:
            // There is no route, which on this machine means neither network
            // has an address: the WiFi has not joined and the USB link has no
            // host on the other end. Saying "could not send" invites a look at
            // the sending; this says what to look at instead.
            ubiqos_line_str(&l, "ping: no route -- has the WiFi joined, and is "
                                "the USB cable in?\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &l);
            return;
        default:
            // WITH the address, because "no reply" and "no reply from the
            // address I picked out of three" are different failures, and only
            // the second one names the thing to look at.
            ubiqos_line_str(&l, "no reply from ");
            addr_str(&l, st[1]);
            ubiqos_line_str(&l, "\r\n");
            break;
        }
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "\r\n");
    ubiqos_line_u32(&l, sent);
    ubiqos_line_str(&l, " sent, ");
    ubiqos_line_u32(&l, got);
    ubiqos_line_str(&l, " back");
    if (got) {
        ubiqos_line_str(&l, ",  best ");
        ms_str(&l, best);
        ubiqos_line_str(&l, "  average ");
        ms_str(&l, total_us / got);
        ubiqos_line_str(&l, "  worst ");
        ms_str(&l, worst);
        ubiqos_line_str(&l, " ms");
    }
    ubiqos_line_str(&l, "\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}
