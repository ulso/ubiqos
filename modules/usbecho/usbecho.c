#include "../../common/myrtos_abi.h"

// Writes to the USB console rather than the serial one. The same calls, the
// same module format -- the only difference is which device is opened, and that
// is decided by a descriptor.
void module_main(void) {
    int32_t u = myrtos_open("/dev/usb");
    if (u < 0) { myrtos_exit(); return; }

    myrtos_line_t line;
    for (int i = 0; i < 20; i++) {
        myrtos_line_reset(&line);
        myrtos_line_str(&line, "[usbecho] hello over USB CDC, line ");
        myrtos_line_u32(&line, (uint32_t)(i + 1));
        myrtos_line_str(&line, "\r\n");
        myrtos_line_flush(u, &line);
        for (volatile int d = 0; d < 2000000; d++) { }
    }
    myrtos_close(u);
}
