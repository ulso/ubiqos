#include <stdint.h>
#include "../../common/myrtos_abi.h"

// Systemanropen och deras nummer kommer från det gemensamma ABI:t. Modulen
// definierar dem inte längre själv, så en ändring i kärnan ger byggfel i
// stället för ett obegripligt fel vid körning.

void module_main(void) {
    int32_t path = myrtos_open("term");
    if (path < 0) {
        myrtos_exit();
        return;
    }

    myrtos_write_str(path, "\n****************************************\n");
    myrtos_write_str(path, "  MYRTOS SHELL v2 -- LOADED FROM SD CARD\n");
    myrtos_write_str(path, "****************************************\n");
    myrtos_write_str(path, "-> This text exists only in SHELL.MOD on the card.\n");
    myrtos_write_str(path, "-> The kernel in flash was never rebuilt.\n");
    myrtos_write_str(path, "-> If you can read this, modules are truly external.\n");

    for (int i = 0; i < 4; i++) {
        myrtos_write_str(path, "[shell v2] running from the SD card\n");
    }

    myrtos_close(path);
}
