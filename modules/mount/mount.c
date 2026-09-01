#include "../../common/myrtos_abi.h"

// mount -- takes the SD card from the beginning.
//
//   mount        over SPI, reading and writing
//   mount sdio   over four-bit SDIO, reading only, about twelve times faster
//
// The filesystem server mounts the card over SPI at startup, so by the time
// anyone can type this the card is already latched into SPI and `mount sdio`
// will refuse. Four bits therefore costs a power cycle: cut the power, and this
// is the first thing to address the card. That is not a limitation of the
// command but of the card, which latches into SPI the moment it is asked that
// way and stays there until the power is cut.
//
// The blacked-out screen the old comment here warned about was spoop() in the
// vendored driver reprogramming DMA channel 3, and it is gone. What SDIO still
// cannot do is write: the first attempt wedged the data state machine, so
// myrtos_sd_write_block refuses rather than tearing a block in half.
//
// A swapped card needs the whole conversation repeated rather than the boot
// sector reread: a fresh card comes up idle and knows nothing of what was asked
// of the last one.
static bool same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void module_main(int argc, char **argv) {
    uint32_t bus = MYRTOS_MOUNT_SPI;

    if (argc > 1) {
        if (same(argv[1], "sdio")) {
            bus = MYRTOS_MOUNT_SDIO;
        } else {
            myrtos_write_str(MYRTOS_STDOUT, "usage: mount [sdio]\n");
            return;
        }
    }

    if (myrtos_mount(bus) == 0) {
        myrtos_write_str(MYRTOS_STDOUT, "card mounted\n");
        if (bus == MYRTOS_MOUNT_SDIO)
            myrtos_write_str(MYRTOS_STDOUT,
                             "reading only: writing over SDIO is not implemented\n");
    } else if (bus == MYRTOS_MOUNT_SDIO) {
        myrtos_write_str(MYRTOS_STDOUT,
                         "mount: no SDIO. If the card was already mounted over SPI it\n"
                         "       cannot change bus until the power is cut.\n");
    } else {
        myrtos_write_str(MYRTOS_STDOUT, "mount: no card, or not FAT32\n");
    }
}
