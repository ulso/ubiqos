#include "../../common/ubiqos_abi.h"

// keys -- reads the USB keyboard and writes what it gets to standard output.
//
// The whole point is how ordinary it is. It opens a device by name and reads
// it; that two PIO state machines are bit-banging USB underneath is the
// driver's business and appears nowhere here.
//
// Escape quits.

void module_main(void) {
    int32_t kbd = ubiqos_open("/dev/kbd");
    if (kbd < 0) {
        ubiqos_write_str(UBIQOS_STDERR, "keys: no such device\n");
        return;
    }

    ubiqos_write_str(UBIQOS_STDOUT, "reading the keyboard, escape to stop\n");

    for (;;) {
        uint8_t c;
        if (ubiqos_read(kbd, &c, 1) <= 0) continue;   // blocks until there is one
        if (c == 27) break;
        ubiqos_write(UBIQOS_STDOUT, &c, 1);
    }

    ubiqos_write_str(UBIQOS_STDOUT, "\ndone\n");
    ubiqos_close(kbd);
}
