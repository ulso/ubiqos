#include "../../common/ubiqos_abi.h"

// reboot -- starts the machine again.
//
// The counterpart of bootsel, which hands the board to the ROM bootloader
// instead. This one comes back up as UbiqOS: the watchdog fires with a zero
// entry point, the bootrom runs, and the image is copied to RAM again exactly
// as it is from power-on.
//
// One thing does NOT come up fresh, and it is worth knowing before reaching for
// this: the SD card keeps its power. A card latches into SPI mode the moment it
// is addressed that way and stays there until the power is cut, so a reboot
// leaves it on whichever bus the last boot chose. Only pulling the plug -- or
// the card -- changes that.
//
// A filesystem call first, and it is not decoration. The server handles one
// request at a time, so a reply to this one means whatever it was doing has
// finished: a directory being written, a sector on its way to the card. It is
// a barrier for work already under way, not a promise about work that starts a
// moment later -- but the window it closes is the one that matters, because it
// is the window this command itself opens.
void module_main(void) {
    uint32_t size = 0;
    (void)ubiqos_fs_stat("/", &size);

    ubiqos_write_str(UBIQOS_STDOUT, "restarting\n");
    ubiqos_sleep(100);          // let the line reach the host before USB goes
    ubiqos_reboot();
}
