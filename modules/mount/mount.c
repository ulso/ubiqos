#include "../../common/myrtos_abi.h"

// mount -- takes the SD card from the beginning.
//
//   mount        over SPI, which is safe
//   mount sdio   over four-bit SDIO, which is not
//
// Nothing touches the card at startup, so this is always the first thing to
// address it, and that matters: a card latches into SPI the moment it is asked
// that way and stays there until the power is cut. SDIO has to be asked for
// first or not at all, which is why there is no automatic fallback -- one plain
// `mount` spends the chance for the rest of the power cycle.
//
// SDIO is the dangerous one. It takes DMA channels 8-11 by number without
// claiming them and reprograms PIO1, and both belong to the video chain: the
// screen goes black and stays black until a reset. That is not understood yet,
// so it is behind a word rather than behind a guess.
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
