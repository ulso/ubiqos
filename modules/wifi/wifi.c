#include "../../common/myrtos_abi.h"

// wifi -- asks the ESP32-C6 what firmware it is running.
//
// A version string coming back settles three things at once: that the wiring is
// right, that the handshake works, and that the chip really speaks NINA -- which
// is what decides that myrtos needs no TCP/IP stack of its own.
static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return !*a && !*b;
}

void module_main(int argc, char **argv) {
    if (myrtos_help(argc, argv,
            "usage: wifi [scan | connect | ip | reset]\n\n  (none)    the coprocessor's firmware version\n  scan      list the networks it can hear\n  connect   join one; the password is typed on this machine's own\n            keyboard and never appears as an argument\n  ip        the address it was given\n  reset     hold the chip in reset and let it come back. The way out\n            when the link is wrong in a way talking cannot fix; it\n            comes back knowing no network.\n")) return;

    myrtos_line_t line;
    char version[16];

    // "wifi scan" lists what is on the air. It needs no name and no password:
    // a scan is what the chip hears, not what it joins.
    if (argc > 1 && is(argv[1], "ip")) {
        char addr[48];
        if (myrtos_wifi_address(addr, sizeof(addr)) == 0) {
            myrtos_write_str(MYRTOS_STDOUT, addr);
            myrtos_write_str(MYRTOS_STDOUT, "\r\n");
        } else {
            myrtos_write_str(MYRTOS_STDOUT, "wifi: no address -- not on a network\r\n");
        }
        return;
    }

    // The way back when the link has gone wrong in a way that talking to it
    // cannot fix. It is not a reconnect: the chip comes up knowing nothing, so
    // this says so rather than leaving somebody to wonder why 'ip' went quiet.
    if (argc > 1 && is(argv[1], "reset")) {
        myrtos_write_str(MYRTOS_STDOUT, "resetting the coprocessor...\r\n");
        if (myrtos_wifi_reset() != 0) {
            myrtos_write_str(MYRTOS_STDOUT, "wifi: no coprocessor to reset\r\n");
            return;
        }
        myrtos_write_str(MYRTOS_STDOUT,
            "wifi: reset. It is not on a network -- 'wifi connect <ssid>' again.\r\n");
        return;
    }

    if (argc > 2 && is(argv[1], "connect")) {
        // The name and the secret, back to back, in one buffer that gets wiped
        // before this returns. The secret is never an argument: argv lives in
        // the process's memory and the shell keeps sixteen lines of history.
        char creds[100];
        uint32_t n = 0;
        for (const char *p = argv[2]; *p && n < 33; p++) creds[n++] = *p;
        creds[n++] = 0;
        uint32_t pass_at = n;

        myrtos_write_str(MYRTOS_STDOUT, "password: ");
        for (;;) {
            uint8_t ch;
            if (myrtos_read(MYRTOS_STDIN, &ch, 1) <= 0) continue;
            if (ch == '\r' || ch == '\n') break;
            if (ch == 3) { n = pass_at; break; }          // ctrl-C: forget it
            if (ch == 8 || ch == 127) { if (n > pass_at) n--; continue; }
            // Not echoed, and not drawn. What is typed here should not survive
            // on the screen, in a scrollback, or in anybody's terminal capture.
            if (ch >= ' ' && n < sizeof(creds) - 1) creds[n++] = (char)ch;
        }
        creds[n] = 0;
        myrtos_write_str(MYRTOS_STDOUT, "\r\n");

        int32_t r = (n > pass_at) ? myrtos_wifi_join(creds) : -1;

        // Gone from memory before this process is, rather than left lying in
        // the block until something else is given it.
        for (uint32_t i = 0; i < sizeof(creds); i++) creds[i] = 0;

        if (r == 0)      myrtos_write_str(MYRTOS_STDOUT, "connected\r\n");
        else if (r == 4) myrtos_write_str(MYRTOS_STDOUT, "wifi: wrong password, or the network refused\r\n");
        else if (r == -2) myrtos_write_str(MYRTOS_STDOUT, "wifi: still trying after twenty seconds\r\n");
        else             myrtos_write_str(MYRTOS_STDOUT, "wifi: could not join\r\n");
        return;
    }

    if (argc > 1 && is(argv[1], "scan")) {
        char why[48];   // room for the raw bytes the chip answered with
        why[0] = 0;
        int32_t n = myrtos_wifi_network(-1, why, sizeof(why));
        if (n < 0) {
            myrtos_write_str(MYRTOS_STDOUT, "wifi: no answer\n");
            return;
        }
        for (int32_t i = 0; i < n; i++) {
            char ssid[34];
            int32_t rssi = myrtos_wifi_network(i, ssid, sizeof(ssid));
            myrtos_line_reset(&line);
            myrtos_line_str(&line, ssid);
            uint32_t k = 0;
            while (ssid[k]) k++;
            while (k++ < 34) myrtos_line_str(&line, " ");
            myrtos_line_str(&line, "-");
            myrtos_line_u32(&line, (uint32_t)(-rssi));
            myrtos_line_str(&line, " dBm\n");
            myrtos_line_flush(MYRTOS_STDOUT, &line);
        }
        myrtos_line_reset(&line);
        myrtos_line_u32(&line, (uint32_t)n);
        myrtos_line_str(&line, n == 1 ? " network" : " networks");
        if (n == 0 && why[0]) {
            myrtos_line_str(&line, ", chip said ");
            myrtos_line_str(&line, why);
        }
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);
        return;
    }

    int32_t r = myrtos_wifi_version(version, sizeof(version));

    myrtos_line_reset(&line);
    if (r == 0) {
        myrtos_line_str(&line, "ESP32-C6 firmware ");
        myrtos_line_str(&line, version);
    } else if (r == -1) {
        // If the line follows the pull it is floating and nothing is driving it;
        // if it ignores the pull, something is, and the chip is simply busy or
        // asleep.
        myrtos_line_str(&line, "wifi: handshake never moved, GP3 ");
        myrtos_line_str(&line, version);
    } else if (r == -2) {
        myrtos_line_str(&line, "wifi: took the command, never answered");
    } else if (r == -3) {
        myrtos_line_str(&line, "wifi: answered, but not with 0xE0");
    } else {
        myrtos_line_str(&line, "wifi: wrong command or parameter count");
    }
    myrtos_line_str(&line, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);
}
