#include "../../common/myrtos_abi.h"

// wifi -- asks the ESP32-C6 what firmware it is running.
//
// A version string coming back settles three things at once: that the wiring is
// right, that the handshake works, and that the chip really speaks NINA -- which
// is what decides that myrtos needs no TCP/IP stack of its own.
void module_main(void) {
    myrtos_line_t line;
    char version[16];

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
