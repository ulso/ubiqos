#include "../../common/myrtos_abi.h"

// keys -- reads the USB keyboard and writes what it gets to standard output.
//
// The whole point is how ordinary it is. It opens a device by name and reads
// it; that two PIO state machines are bit-banging USB underneath is the
// driver's business and appears nowhere here.
//
// Escape quits.

void module_main(void) {
    int32_t kbd = myrtos_open("/dev/kbd");
    if (kbd < 0) {
        myrtos_write_str(MYRTOS_STDERR, "keys: no such device\n");
        return;
    }

    myrtos_write_str(MYRTOS_STDOUT, "reading the keyboard, escape to stop\n");

    for (;;) {
        uint8_t c;
        if (myrtos_read(kbd, &c, 1) <= 0) continue;   // blocks until there is one
        if (c == 27) break;
        myrtos_write(MYRTOS_STDOUT, &c, 1);
    }

    myrtos_write_str(MYRTOS_STDOUT, "\ndone\n");
    myrtos_close(kbd);
}
