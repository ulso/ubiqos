#include "../../common/ubiqos_abi.h"

// wifi -- asks the ESP32-C6 what firmware it is running.
//
// A version string coming back settles three things at once: that the wiring is
// right, that the handshake works, and that the chip really speaks NINA -- which
// is what decides that UbiqOS needs no TCP/IP stack of its own.
static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return !*a && !*b;
}

// What came of it, in the caller's words rather than the chip's number.
static void report(int32_t r) {
    if (r == 0)       ubiqos_write_str(UBIQOS_STDOUT, "connected\r\n");
    else if (r == 4)  ubiqos_write_str(UBIQOS_STDOUT, "wifi: wrong password, or the network refused\r\n");
    else if (r == -2) ubiqos_write_str(UBIQOS_STDOUT, "wifi: still trying after twenty seconds\r\n");
    else              ubiqos_write_str(UBIQOS_STDOUT, "wifi: could not join\r\n");
}

void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: wifi [scan | connect | ip | stats | reset]\n\n  (none)    the coprocessor's firmware version\n  scan      list the networks it can hear\n  connect   join one: 'connect <ssid>' asks for the password, and bare\n            'connect' uses /sd/config.txt and asks for whatever it\n            did not say. The password is typed here and is never an\n            argument\n  ip        the address it was given\n  stats     how the command channel to the chip has been behaving\n  reset     hold the chip in reset and let it come back. The way out\n            when the link is wrong in a way talking cannot fix; it\n            comes back knowing no network.\n")) return;

    ubiqos_line_t line;
    char version[16];

    // "wifi scan" lists what is on the air. It needs no name and no password:
    // a scan is what the chip hears, not what it joins.
    if (argc > 1 && is(argv[1], "ip")) {
        char addr[48];
        if (ubiqos_wifi_address(addr, sizeof(addr)) == 0) {
            ubiqos_write_str(UBIQOS_STDOUT, addr);
            ubiqos_write_str(UBIQOS_STDOUT, "\r\n");
        } else {
            ubiqos_write_str(UBIQOS_STDOUT, "wifi: no address -- not on a network\r\n");
        }
        return;
    }

    // How the command channel has been behaving, which is a different question
    // from whether the chip works. The driver's whole failure mode was a
    // channel one step out of step, and from outside that looks exactly like a
    // dead coprocessor.
    if (argc > 1 && is(argv[1], "stats")) {
        // Zeroed by hand. An initialiser this size makes the compiler emit a
        // memset, and a module links no C library -- the same trap the Zig and
        // neopixel modules hit, in a different disguise.
        uint32_t n[8];
        for (uint32_t i = 0; i < 8; i++) n[i] = 0;
        if (ubiqos_wifi_stats(n) != 0) {
            ubiqos_write_str(UBIQOS_STDERR, "wifi: no coprocessor to ask\r\n");
            return;
        }
        static const char *what[] = { "commands  ", "resyncs   ", "retries   ",
                                      "failures  ", "recv end ok   ", "recv end bad  ",
                                      "  last byte   ", "  its length  " };
        for (uint32_t i = 0; i < 8; i++) {
            ubiqos_line_reset(&line);
            ubiqos_line_str(&line, what[i]);
            ubiqos_line_u32(&line, n[i]);
            ubiqos_line_str(&line, "\r\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &line);
        }
        return;
    }

    // The way back when the link has gone wrong in a way that talking to it
    // cannot fix. It is not a reconnect: the chip comes up knowing nothing, so
    // this says so rather than leaving somebody to wonder why 'ip' went quiet.
    if (argc > 1 && is(argv[1], "reset")) {
        ubiqos_write_str(UBIQOS_STDOUT, "resetting the coprocessor...\r\n");
        if (ubiqos_wifi_reset() != 0) {
            ubiqos_write_str(UBIQOS_STDOUT, "wifi: no coprocessor to reset\r\n");
            return;
        }
        ubiqos_write_str(UBIQOS_STDOUT,
            "wifi: reset. It is not on a network -- 'wifi connect <ssid>' again.\r\n");
        return;
    }

    if (argc > 1 && is(argv[1], "connect")) {
        // Bare `wifi connect` is the card's network. When /sd/config.txt gave
        // both a name and a password the kernel joins with them and this
        // process never sees either -- it cannot, the file is unreadable to it.
        if (argc == 2 && ubiqos_config_get(UBIQOS_CFG_PASSWORD, 0, 0) == 1) {
            char named[34];
            int32_t have = ubiqos_config_get(UBIQOS_CFG_SSID, named, sizeof(named));
            if (have > 0) {
                ubiqos_write_str(UBIQOS_STDOUT, "joining ");
                ubiqos_write_str(UBIQOS_STDOUT, named);
                ubiqos_write_str(UBIQOS_STDOUT, "\r\n");
                report(ubiqos_wifi_join(0));
                return;
            }
        }

        // The name and the secret, back to back, in one buffer that gets wiped
        // before this returns. The secret is never an argument: argv lives in
        // the process's memory and the shell keeps sixteen lines of history.
        char creds[100];
        uint32_t n = 0;

        // The name can come from the card, from the command line, or from
        // whoever is at the keyboard -- in that order, because each is more
        // trouble than the one before it.
        char named[34];
        const char *want = 0;
        if (argc > 2) want = argv[2];
        else if (ubiqos_config_get(UBIQOS_CFG_SSID, named, sizeof(named)) > 0) want = named;

        if (want) {
            for (const char *p = want; *p && n < 33; p++) creds[n++] = *p;
        } else {
            ubiqos_write_str(UBIQOS_STDOUT, "network: ");
            for (;;) {
                uint8_t ch;
                if (ubiqos_read(UBIQOS_STDIN, &ch, 1) <= 0) continue;
                if (ch == '\r' || ch == '\n') break;
                if (ch == 3) { n = 0; break; }
                if (ch == 8 || ch == 127) {
                    if (n) { n--; ubiqos_write_str(UBIQOS_STDOUT, "\b \b"); }
                    continue;
                }
                // Echoed, unlike the password. A network name is on the air for
                // anybody to hear and there is nothing to hide about it -- and
                // a name typed blind is a name typed wrong.
                if (ch >= ' ' && n < 33) {
                    creds[n++] = (char)ch;
                    ubiqos_write(UBIQOS_STDOUT, &ch, 1);
                }
            }
            ubiqos_write_str(UBIQOS_STDOUT, "\r\n");
            if (!n) { ubiqos_write_str(UBIQOS_STDOUT, "wifi: no network named\r\n"); return; }
        }
        creds[n++] = 0;
        uint32_t pass_at = n;

        ubiqos_write_str(UBIQOS_STDOUT, "password: ");
        for (;;) {
            uint8_t ch;
            if (ubiqos_read(UBIQOS_STDIN, &ch, 1) <= 0) continue;
            if (ch == '\r' || ch == '\n') break;
            if (ch == 3) { n = pass_at; break; }          // ctrl-C: forget it
            if (ch == 8 || ch == 127) { if (n > pass_at) n--; continue; }
            // Not echoed, and not drawn. What is typed here should not survive
            // on the screen, in a scrollback, or in anybody's terminal capture.
            if (ch >= ' ' && n < sizeof(creds) - 1) creds[n++] = (char)ch;
        }
        creds[n] = 0;
        ubiqos_write_str(UBIQOS_STDOUT, "\r\n");

        int32_t r = (n > pass_at) ? ubiqos_wifi_join(creds) : -1;

        // Gone from memory before this process is, rather than left lying in
        // the block until something else is given it.
        for (uint32_t i = 0; i < sizeof(creds); i++) creds[i] = 0;

        report(r);
        return;
    }

    if (argc > 1 && is(argv[1], "scan")) {
        char why[48];   // room for the raw bytes the chip answered with
        why[0] = 0;
        int32_t n = ubiqos_wifi_network(-1, why, sizeof(why));
        if (n < 0) {
            ubiqos_write_str(UBIQOS_STDOUT, "wifi: no answer\n");
            return;
        }
        for (int32_t i = 0; i < n; i++) {
            char ssid[34];
            int32_t rssi = ubiqos_wifi_network(i, ssid, sizeof(ssid));
            ubiqos_line_reset(&line);
            ubiqos_line_str(&line, ssid);
            uint32_t k = 0;
            while (ssid[k]) k++;
            while (k++ < 34) ubiqos_line_str(&line, " ");
            ubiqos_line_str(&line, "-");
            ubiqos_line_u32(&line, (uint32_t)(-rssi));
            ubiqos_line_str(&line, " dBm\n");
            ubiqos_line_flush(UBIQOS_STDOUT, &line);
        }
        ubiqos_line_reset(&line);
        ubiqos_line_u32(&line, (uint32_t)n);
        ubiqos_line_str(&line, n == 1 ? " network" : " networks");
        if (n == 0 && why[0]) {
            ubiqos_line_str(&line, ", chip said ");
            ubiqos_line_str(&line, why);
        }
        ubiqos_line_str(&line, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &line);
        return;
    }

    int32_t r = ubiqos_wifi_version(version, sizeof(version));

    ubiqos_line_reset(&line);
    if (r == 0) {
        ubiqos_line_str(&line, "ESP32-C6 firmware ");
        ubiqos_line_str(&line, version);
    } else if (r == -1) {
        // If the line follows the pull it is floating and nothing is driving it;
        // if it ignores the pull, something is, and the chip is simply busy or
        // asleep.
        ubiqos_line_str(&line, "wifi: handshake never moved, GP3 ");
        ubiqos_line_str(&line, version);
    } else if (r == -2) {
        ubiqos_line_str(&line, "wifi: took the command, never answered");
    } else if (r == -3) {
        ubiqos_line_str(&line, "wifi: answered, but not with 0xE0");
    } else {
        ubiqos_line_str(&line, "wifi: wrong command or parameter count");
    }
    ubiqos_line_str(&line, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &line);
}
