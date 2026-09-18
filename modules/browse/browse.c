#include "../../common/ubiqos_abi.h"

// browse -- what is on the network.
//
//   browse              the kinds of service anybody offers
//   browse _http._tcp   who offers that one
//
// This is zeroconf from the asking side. The board has answered such questions
// since the 9th -- that is how the Mac finds fruit-jam.local -- and asking them
// is what lets it find everything else without a server anywhere.
//
// The question goes out on EVERY interface that is up, which matters on a
// machine with two: a Raspberry Pi on the air and a Mac on the USB cable are
// on different networks, and a browse that picked one would be a browse that
// found half the house.

static void say(const char *s) { ubiqos_write_str(UBIQOS_STDOUT, s); }

// "_services._dns-sd._udp" is the standing question "what kinds of thing are
// there", and every responder answers it with the services it offers. It is
// where a browse with nothing in mind begins.
#define SERVICE_TYPES "_services._dns-sd._udp.local"

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: browse [SERVICE]\n\nAsks the network what is on it. With no "
            "argument it lists the kinds of\nservice anybody offers; with one, "
            "such as _http._tcp, it lists who\noffers that.\n\nThe question goes "
            "out on every interface that is up.\n")) return;

    char service[64];
    uint32_t n = 0;
    if (argc > 1) {
        for (const char *p = argv[1]; *p && n < sizeof(service) - 7; p++) service[n++] = *p;
        // ".local" is not something anybody should have to type.
        const char *tail = ".local";
        bool has = n > 6;
        if (has) for (int i = 0; i < 6; i++) if (service[n - 6 + i] != tail[i]) { has = false; break; }
        if (!has) for (int i = 0; i < 6; i++) service[n++] = tail[i];
        service[n] = 0;
    } else {
        const char *d = SERVICE_TYPES;
        while (*d) service[n++] = *d++;
        service[n] = 0;
    }

    say("asking");
    if (ubiqos_browse(service) < 0) {
        say("\r\nbrowse: the network stack would not take it -- is lwIP up?\r\n");
        return;
    }

    for (int waited = 0; waited < 40; waited++) {
        if (ubiqos_browse_done() != 0) break;
        say(".");
        ubiqos_sleep(100);
    }
    say("\r\n\r\n");

    uint32_t shown = 0;
    for (uint32_t i = 0; i < UBIQOS_MDNS_MAX; i++) {
        char name[64];
        if (ubiqos_browse_name(i, name, sizeof(name)) <= 0) break;
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "  ");
        ubiqos_line_str(&l, name);
        ubiqos_line_str(&l, "\r\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
        shown++;
    }

    if (!shown) {
        say("nothing answered\r\n");
        return;
    }

    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, "\r\n");
    ubiqos_line_u32(&l, shown);
    ubiqos_line_str(&l, argc > 1 ? " answering\r\n" : " kinds of service\r\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}
