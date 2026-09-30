#include "../common/ubiqos_abi.h"

// The Challenger's boot script, run by the shell as /sd/startup would be on a
// board with a card -- and a card's script, if this board ever had one, would
// win. See run_startup_module in kernel/fsserver.c.
//
// The bridge: the radio's Bluetooth heard for HibouAir sensors, and what they
// say served over its WiFi. hibouair waits for the network to have joined
// before it asks the radio for Bluetooth; httpd waits for nothing.
__attribute__((section(".rodata.descriptor"), used))
const char startup[] =
    "hibouair -q &\n"
    "httpd &\n";
