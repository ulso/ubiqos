#include "../../common/myrtos_abi.h"

// mount -- takes the SD card from the beginning.
//
//   mount        over SPI
//   mount sdio   over four-bit SDIO, which reads about twice as fast
//
// The filesystem server mounts the card over SPI at startup, so by the time
// anyone can type this the card is already latched into SPI and `mount sdio`
// will refuse. Four bits therefore costs a power cycle: cut the power, and this
// is the first thing to address the card. That is not a limitation of the
// command but of the card, which latches into SPI the moment it is asked that
// way and stays there until the power is cut.
//
// The filesystem server mounts over SDIO at startup and falls back to SPI, so
// this is mostly for a card put in afterwards, or to take the card again after
// it has been swapped.
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
    } else if (bus == MYRTOS_MOUNT_SDIO) {
        myrtos_write_str(MYRTOS_STDOUT,
                         "mount: no SDIO. If the card was already mounted over SPI it\n"
                         "       cannot change bus until the power is cut.\n");
    } else {
        myrtos_write_str(MYRTOS_STDOUT, "mount: no card, or not FAT32\n");
    }
}
