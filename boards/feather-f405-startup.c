#include "../common/ubiqos_abi.h"

// The Feather STM32F405's boot script, run by the shell as /sd/startup would
// be on a board with a card. See run_startup_module in kernel/fsserver.c.
//
// The web server, and with it /api/i2c: the I2C bus over the USB cable's
// network, for an app to drive a sensor on the STEMMA QT connector. It waits
// for the network itself.
__attribute__((section(".rodata.descriptor"), used))
const char startup[] =
    "httpd &\n";
