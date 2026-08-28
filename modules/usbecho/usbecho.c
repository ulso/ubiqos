#include "../../common/myrtos_abi.h"

// Skriver till USB-konsolen i stället för den seriella. Samma anrop, samma
// modul-format -- enda skillnaden är vilken enhet som öppnas, och den avgörs
// av en beskrivare.
void module_main(void) {
    int32_t u = myrtos_open("usb");
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
