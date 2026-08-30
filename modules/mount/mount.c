#include "../../common/myrtos_abi.h"

// mount -- takes the SD card again from the beginning.
//
// The card is mounted once at startup, so one put in or swapped while the board
// is running is not seen. A swapped card needs the whole conversation repeated
// rather than the boot sector reread: a fresh card comes up idle and knows
// nothing of what was asked of the last one.
void module_main(void) {
    if (myrtos_mount() == 0) {
        myrtos_write_str(MYRTOS_STDOUT, "card mounted\n");
    } else {
        myrtos_write_str(MYRTOS_STDOUT, "mount: no card, or not FAT32\n");
    }
}
