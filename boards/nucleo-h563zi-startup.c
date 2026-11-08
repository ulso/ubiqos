#include "../common/ubiqos_abi.h"

// The NUCLEO-H563ZI's boot script, run by the shell as /sd/startup would be on
// a board with a card. See run_startup_module in kernel/fsserver.c.
//
// The MCP server, on the Ethernet. The board has no USB cable network, so
// mcpd's default -- the cable's subnet only -- would refuse everybody; -a is
// the only way it is any use here, and it opens the tools to the whole LAN.
// Every tool only reads. It waits for the network itself.
__attribute__((section(".rodata.descriptor"), used))
const char startup[] =
    "mcpd -a &\n";
