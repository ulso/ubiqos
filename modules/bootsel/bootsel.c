#include "../../common/ubiqos_abi.h"

// bootsel -- hands the board back to the ROM bootloader, so firmware can be
// loaded without reaching for the buttons. The same place BOOTSEL and reset
// take it, reached from software instead.

void module_main(void) {
    ubiqos_write_str(UBIQOS_STDOUT, "rebooting into the bootloader\n");
    ubiqos_sleep(100);          // let the line reach the host before USB goes
    ubiqos_bootsel();
}
