#include "../../common/myrtos_abi.h"

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

static void say(const char *s) { myrtos_write_str(MYRTOS_STDOUT, s); }

static bool is(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static void addr_str(myrtos_line_t *l, uint32_t a) {
    for (int i = 0; i < 4; i++) {
        myrtos_line_u32(l, (a >> (i * 8)) & 0xffu);   // little-endian, as lwIP keeps it
        if (i < 3) myrtos_line_str(l, ".");
    }
}

// Microseconds as milliseconds with one decimal, because a ping over USB is
// two milliseconds and over the air is eighty: a whole number would round the
// interesting one to nothing.
static void ms_str(myrtos_line_t *l, uint32_t us) {
    myrtos_line_u32(l, us / 1000u);
    myrtos_line_str(l, ".");
    myrtos_line_u32(l, (us % 1000u) / 100u);
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: ping HOST [count]\n\nHOST is an address or a name. A name "
            "ending in .local is asked\nfor by multicast, so no DNS server is "
            "needed.\n\nCtrl-C stops it.\n")) return;

    if (argc < 2) { say("usage: ping HOST [count]\r\n"); return; }

    uint32_t want = 4;
    if (argc > 2) {
        want = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) want = want * 10 + (uint32_t)(*p - '0');
        if (!want) want = 4;
    }

    uint32_t sent = 0, got = 0, total_us = 0, worst = 0, best = 0xffffffffu;

    for (uint32_t n = 0; n < want; n++) {
        if (n) myrtos_sleep(1000);                    // one a second, as ping does

        if (myrtos_ping(argv[1]) < 0) {
            say("ping: the network stack would not take it -- is lwIP up?\r\n");
            return;
        }
        sent++;

        uint32_t st[3] = { 0, 0, 0 };
        for (int waited = 0; waited < 300; waited++) {
            myrtos_ping_state(st);
            if (st[0] != MYRTOS_PING_RESOLVING && st[0] != MYRTOS_PING_WAITING) break;
            myrtos_sleep(10);
        }

        myrtos_line_t l;
        myrtos_line_reset(&l);
        switch (st[0]) {
        case MYRTOS_PING_REPLIED:
            got++;
            total_us += st[2];
            if (st[2] > worst) worst = st[2];
            if (st[2] < best) best = st[2];
            myrtos_line_str(&l, "reply from ");
            addr_str(&l, st[1]);
            myrtos_line_str(&l, "  ");
            ms_str(&l, st[2]);
            myrtos_line_str(&l, " ms\r\n");
            break;
        case MYRTOS_PING_NONAME:
            myrtos_line_str(&l, "ping: nobody answers to that name\r\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            return;                                   // no point asking again
        case MYRTOS_PING_UNREACHABLE:
            myrtos_line_str(&l, "ping: the stack could not send it\r\n");
            myrtos_line_flush(MYRTOS_STDOUT, &l);
            return;
        default:
            // WITH the address, because "no reply" and "no reply from the
            // address I picked out of three" are different failures, and only
            // the second one names the thing to look at.
            myrtos_line_str(&l, "no reply from ");
            addr_str(&l, st[1]);
            myrtos_line_str(&l, "\r\n");
            break;
        }
        myrtos_line_flush(MYRTOS_STDOUT, &l);
    }

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "\r\n");
    myrtos_line_u32(&l, sent);
    myrtos_line_str(&l, " sent, ");
    myrtos_line_u32(&l, got);
    myrtos_line_str(&l, " back");
    if (got) {
        myrtos_line_str(&l, ",  best ");
        ms_str(&l, best);
        myrtos_line_str(&l, "  average ");
        ms_str(&l, total_us / got);
        myrtos_line_str(&l, "  worst ");
        ms_str(&l, worst);
        myrtos_line_str(&l, " ms");
    }
    myrtos_line_str(&l, "\r\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}
