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
    myrtos_line_t line;
    char version[16];

    // "wifi scan" lists what is on the air. It needs no name and no password:
    // a scan is what the chip hears, not what it joins.
    if (argc > 1 && is(argv[1], "scan")) {
        int32_t n = myrtos_wifi_look();
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
        myrtos_line_str(&line, n == 1 ? " network\n" : " networks\n");
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
